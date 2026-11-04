#define _POSIX_C_SOURCE 200809L
#include "process.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pslog.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Only the standalone process owns these handlers. Block stop signals before
 * creating the controller, then unblock them only in this event-loop thread.
 * A self-pipe avoids idle polling and the check-then-sleep signal race. */
static const int handled_signals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                      SIGPIPE};
static volatile sig_atomic_t signal_write_fd = -1;
static volatile sig_atomic_t stop_signal;
typedef struct process_signals {
  struct sigaction saved[5];
  sigset_t original, blocked;
  size_t installed;
  int pipe[2], has_mask;
} process_signals;

static void stop_handler(int number) {
  int saved_errno = errno;
  if (stop_signal == 0) {
    char byte = 1;
    stop_signal = number;
    if (signal_write_fd >= 0) {
      ssize_t written;
      do {
        written = write((int)signal_write_fd, &byte, 1U);
      } while (written < 0 && errno == EINTR);
      (void)written;
    }
  }
  errno = saved_errno;
}
static void signals_close(process_signals *owner) {
  size_t i;
  if (owner->has_mask)
    (void)pthread_sigmask(SIG_BLOCK, &owner->blocked, NULL);
  signal_write_fd = -1;
  for (i = 0U; i < owner->installed; ++i)
    (void)sigaction(handled_signals[i], &owner->saved[i], NULL);
  if (owner->pipe[0] >= 0)
    close(owner->pipe[0]);
  if (owner->pipe[1] >= 0)
    close(owner->pipe[1]);
  if (owner->has_mask)
    (void)pthread_sigmask(SIG_SETMASK, &owner->original, NULL);
}
static int signals_open(process_signals *owner, cai_error *error) {
  struct sigaction action;
  size_t i;
  memset(owner, 0, sizeof(*owner));
  owner->pipe[0] = owner->pipe[1] = -1;
  sigemptyset(&owner->blocked);
  for (i = 0U; i < 4U; ++i)
    sigaddset(&owner->blocked, handled_signals[i]);
  if (pthread_sigmask(SIG_BLOCK, &owner->blocked, &owner->original) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "block server stop signals");
  owner->has_mask = 1;
  if (pipe(owner->pipe) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "create server signal pipe");
  for (i = 0U; i < 2U; ++i)
    if (fcntl(owner->pipe[i], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(owner->pipe[i], F_SETFL, O_NONBLOCK) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "protect server signal pipe");
  stop_signal = 0;
  signal_write_fd = owner->pipe[1];
  memset(&action, 0, sizeof(action));
  action.sa_mask = owner->blocked;
  for (i = 0U; i < 5U; ++i) {
    action.sa_handler = i == 4U ? SIG_IGN : stop_handler;
    if (sigaction(handled_signals[i], &action, &owner->saved[i]) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "install server process signal handler");
    ++owner->installed;
  }
  return CAI_OK;
}
static int signals_enable(process_signals *owner, cai_error *error) {
  sigset_t running = owner->original;
  size_t i;
  for (i = 0U; i < 4U; ++i)
    sigdelset(&running, handled_signals[i]);
  return pthread_sigmask(SIG_SETMASK, &running, NULL) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "enable server process stop signals");
}
static int server_wait(cai_app *app, process_signals *signals,
                       pslog_logger *logger, cai_app_status *status,
                       cai_error *error) {
  struct pollfd interests[2];
  int announced = 0, stopping = 0, rc;
  unsigned long long stop_id;
  interests[0].fd = cai_app_status_fd(app);
  interests[0].events = POLLIN;
  interests[1].fd = signals->pipe[0];
  interests[1].events = POLLIN;
  for (;;) {
    rc = cai_app_observe_status(app, status, error);
    if (rc != CAI_OK || !status->running)
      return rc;
    if (!announced && status->runtime_ready && status->has_server &&
        status->server.phase == CAI_ATTACHMENT_READY) {
      pslog_field fields[3];
      fields[0] = pslog_str("event", "workspace_ready");
      fields[1] = pslog_str("session_id", status->session_id);
      fields[2] = pslog_bool("persistent", status->server.persist);
      logger->info(logger, "Workspace server ready", fields, 3U);
      announced = 1;
    }
    if (stop_signal != 0 && !stopping) {
      rc = cai_app_control(app, CAI_APP_STOP, NULL, &stop_id, error);
      if (rc != CAI_OK) {
        /* Completion can race a signal. Only suppress rejection when the
         * authoritative owner has already stopped. */
        if (cai_app_get_status(app, status, NULL) == CAI_OK &&
            !status->running) {
          cai_error_cleanup(error);
          rc = CAI_OK;
        }
        if (rc != CAI_OK)
          return rc;
      }
      stopping = 1;
      continue;
    }
    rc = poll(interests, 2U, -1);
    if (rc < 0 && errno == EINTR)
      continue;
    if (rc < 0 || ((interests[0].revents | interests[1].revents) &
                   (POLLERR | POLLHUP | POLLNVAL)) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "wait for workspace server status");
    if ((interests[1].revents & POLLIN) != 0) {
      char bytes[32];
      while (read(interests[1].fd, bytes, sizeof(bytes)) > 0 ||
             errno == EINTR) {
      }
    }
  }
}

int cai_cli_headless_run(const cai_app_config *base, int argc,
                         const char *const *argv, int *exit_code,
                         cai_error *error) {
  cai_app_config config = *base;
  cai_app *app = NULL;
  cai_app_status *status = calloc(1U, sizeof(*status));
  cai_attachment_config lifetime;
  process_signals signals;
  pslog_config logging;
  pslog_logger *logger;
  int started = 0, joined, rc;
  unsigned long long stop_id;
  *exit_code = 1;
  if (status == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate server status");
  cai_app_status_init(status);
  pslog_default_config(&logging);
  logging.mode = PSLOG_MODE_JSON;
  logging.color = PSLOG_COLOR_AUTO;
  logging.min_level = PSLOG_LEVEL_INFO;
  logging.output = pslog_output_from_fp(stderr, 0);
  logger = pslog_new_from_env("LOG_", &logging);
  if (logger == NULL) {
    free(status);
    return cai_cli_error(error, CAI_ERR_NOMEM, "create server process logger");
  }
  config.logger = logger;
  config.banner = config.introduction_markdown = NULL;
  rc = signals_open(&signals, error);
  if (rc == CAI_OK)
    rc = cai_app_open(&config, &app, error);
  cai_attachment_config_init(&lifetime);
  lifetime.persist = 1;
  if (rc == CAI_OK) {
    rc = cai_app_start_server(app, NULL, argc, argv, &lifetime, error);
    started = rc == CAI_OK;
  }
  if (rc == CAI_OK)
    rc = signals_enable(&signals, error);
  if (rc == CAI_OK)
    rc = server_wait(app, &signals, logger, status, error);
  if (started) {
    if (rc != CAI_OK)
      (void)cai_app_control(app, CAI_APP_STOP, NULL, &stop_id, NULL);
    joined = cai_app_join(app, exit_code, rc == CAI_OK ? error : NULL);
    if (joined != CAI_OK) {
      /* An unjoined owner still borrows its logger/app. The executable exits
       * on this fatal ownership failure; do not free either under that owner.
       */
      cai_app_status_cleanup(status);
      free(status);
      return rc != CAI_OK ? rc : joined;
    }
    if (rc == CAI_OK)
      rc = cai_app_get_status(app, status, error);
    if (rc == CAI_OK) {
      pslog_field fields[3];
      fields[0] = pslog_str("event", "workspace_stopped");
      fields[1] = pslog_i64("exit_code", *exit_code);
      fields[2] = pslog_i64("signal", stop_signal);
      if (*exit_code != 0)
        logger->error(logger,
                      status->error.message != NULL ? status->error.message
                                                    : "Workspace server failed",
                      fields, 3U);
      else
        logger->info(logger, "Workspace server stopped", fields, 3U);
    }
  }
  if (app != NULL)
    cai_app_close(app);
  signals_close(&signals);
  logger->destroy(logger);
  cai_app_status_cleanup(status);
  free(status);
  return rc;
}
