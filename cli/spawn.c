#define _POSIX_C_SOURCE 200809L
#include "spawn.h"
#include "../src/cai_process_child.h"
#include "error.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
struct cai_cli_spawn {
  cai_cli_spawn_state state;
  cai_wire_peer *peer;
  cai_protocol_decoder *protocol;
  cai_cli_bootstrap_decoder *decoder;
  pid_t pid;
  uint64_t startup, terminate, reap_due;
  int eof, broken, reaped, cancelling, killed, reported_exit;
};
static uint64_t later(uint64_t now, uint64_t delta) {
  return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}
static int retained_fd(int descriptor) {
  int copy;
  if (descriptor < 0 || descriptor >= 3)
    return descriptor;
  copy = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
  close(descriptor);
  return copy;
}
static void failure(cai_cli_spawn *s, int code, const char *message) {
  if (s->state.observation.code == CAI_OK) {
    s->state.observation.code = code;
    snprintf(s->state.observation.message, sizeof(s->state.observation.message),
             "%s", message);
  }
}
static int record(void *context, const cai_protocol_event *event,
                  cai_error *error) {
  cai_cli_spawn *s = context;
  const cai_cli_bootstrap_message *message = NULL;
  int rc = cai_cli_bootstrap_decode(s->decoder, event, &message, error);
  if (rc != CAI_OK || message == NULL)
    return rc;
  if (s->reported_exit || (message->ready && s->state.was_ready))
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "duplicate workspace bootstrap observation");
  if (s->cancelling)
    return CAI_OK;
  s->state.observation = *message;
  if (message->ready) {
    s->state.was_ready = 1;
    s->state.phase = CAI_CLI_SPAWN_READY;
    s->startup = 0U;
  } else {
    s->reported_exit = 1;
    s->state.phase = CAI_CLI_SPAWN_STOPPING;
  }
  return CAI_OK;
}
void cai_cli_spawn_abandon(cai_cli_spawn *s) {
  if (s == NULL)
    return;
  cai_wire_peer_close(s->peer);
  cai_protocol_decoder_close(s->protocol);
  cai_cli_bootstrap_decoder_close(s->decoder);
  free(s);
}
int cai_cli_spawn_close(cai_cli_spawn *s, cai_error *error) {
  if (s != NULL && s->pid > 0 && !s->reaped)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "workspace child must be reaped before closing its watch");
  cai_cli_spawn_abandon(s);
  return CAI_OK;
}
int cai_cli_spawn_open(const char *program, int argc, const char *const *argv,
                       uint64_t now, uint64_t timeout, cai_cli_spawn **out,
                       cai_error *error) {
  cai_cli_spawn *s;
  struct sigaction child_action;
  sigset_t blocked, original;
  int pair[2] = {-1, -1}, null_fd = -1, i, rc, masked = 0;
  char **arguments = NULL;
  char descriptor[32];
  if (out != NULL)
    *out = NULL;
  if (out == NULL || program == NULL || program[0] != '/' || argc < 0 ||
      argc > 65536 || (argc != 0 && argv == NULL))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "absolute workspace executable and arguments required");
  if (sigaction(SIGCHLD, NULL, &child_action) != 0 ||
      child_action.sa_handler != SIG_DFL ||
      (child_action.sa_flags & SA_NOCLDWAIT))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "workspace launch requires default child reaping before threads");
  if (access(program, X_OK) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace executable is not accessible");
  s = calloc(1U, sizeof(*s));
  arguments = calloc((size_t)argc + 5U, sizeof(*arguments));
  if (s == NULL || arguments == NULL) {
    free(s);
    free(arguments);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace launch");
  }
  rc = cai_cli_bootstrap_decoder_open(&s->decoder, error);
  if (rc == CAI_OK)
    rc = cai_protocol_decoder_open(record, s, &s->protocol, error);
  for (i = 0; i < argc && rc == CAI_OK; i++)
    if (argv[i] == NULL)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "null workspace startup argument");
    else if ((arguments[i + 4] = strdup(argv[i])) == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM,
                         "copy workspace startup argument");
  if (rc == CAI_OK && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "create workspace bootstrap socket");
  if (rc == CAI_OK) {
    pair[0] = retained_fd(pair[0]);
    pair[1] = retained_fd(pair[1]);
  }
  if (rc == CAI_OK &&
      (pair[0] < 3 || pair[1] < 3 || fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0 ||
       fcntl(pair[1], F_SETFD, FD_CLOEXEC) != 0))
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "protect workspace bootstrap socket");
  if (rc == CAI_OK) {
    rc = cai_wire_peer_open(pair[0], 16384U, cai_protocol_decoder_event,
                            s->protocol, &s->peer, error);
    if (rc == CAI_OK)
      pair[0] = -1;
  }
  if (rc == CAI_OK && (null_fd = open("/dev/null", O_RDWR | O_CLOEXEC)) < 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "open quiet workspace standard descriptors");
  if (rc == CAI_OK && (null_fd = retained_fd(null_fd)) < 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "retain quiet workspace standard descriptors");
  if (rc == CAI_OK) {
    snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
    arguments[0] = strdup(program);
    arguments[1] = strdup("--_workspace-owner");
    arguments[2] = strdup(descriptor);
    arguments[3] = strdup("--");
    for (i = 0; i < 4; i++)
      if (arguments[i] == NULL)
        rc = cai_cli_error(error, CAI_ERR_NOMEM,
                           "copy workspace executable arguments");
  }
  if (rc == CAI_OK) {
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &blocked, &original) != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "block workspace launch signals");
    else
      masked = 1;
  }
  if (rc == CAI_OK) {
    s->pid = fork();
    if (s->pid == 0) {
      if (setsid() < 0 || dup2(null_fd, 0) < 0 || dup2(null_fd, 1) < 0 ||
          dup2(null_fd, 2) < 0 || fcntl(pair[1], F_SETFD, 0) != 0 ||
          cai_process_child_signals() != 0)
        _exit(126);
      close(null_fd);
      execv(program, arguments);
      _exit(127);
    }
    if (s->pid < 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "fork workspace owner");
  }
  if (masked)
    (void)sigprocmask(SIG_SETMASK, &original, NULL);
  if (null_fd >= 0)
    close(null_fd);
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  for (i = 0; i < argc + 4; i++)
    free(arguments[i]);
  free(arguments);
  if (rc != CAI_OK) {
    cai_cli_spawn_abandon(s);
    return rc;
  }
  s->startup = later(now, timeout ? timeout : 60000U);
  *out = s;
  return CAI_OK;
}
int cai_cli_spawn_cancel(cai_cli_spawn *s, uint64_t now, cai_error *error) {
  if (s == NULL || s->state.was_ready)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "startup cancellation cannot stop a ready workspace server");
  if (s->reaped || s->cancelling)
    return CAI_OK;
  if (kill(s->pid, SIGTERM) != 0 && errno != ESRCH)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "cancel owned workspace startup");
  s->cancelling = 1;
  s->terminate = later(now, 5000U);
  s->state.phase = CAI_CLI_SPAWN_STOPPING;
  failure(s, CAI_ERR_CANCELLED, "workspace startup cancelled");
  return CAI_OK;
}
static int read_peer(cai_cli_spawn *s, cai_error *error) {
  if (s->broken) {
    unsigned char discard[4096];
    ssize_t n =
        recv(cai_wire_peer_fd(s->peer), discard, sizeof(discard), MSG_DONTWAIT);
    if (n == 0)
      s->eof = 1;
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      s->eof = 1;
    return CAI_OK;
  }
  return cai_wire_peer_read(s->peer, 0U, &s->eof, error);
}
int cai_cli_spawn_pump(cai_cli_spawn *s, uint64_t now, cai_error *error) {
  int status, rc;
  pid_t waited;
  cai_error local;
  if (s == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "workspace launch required");
  if (s->reaped)
    return CAI_OK;
  cai_error_init(&local);
  if (!s->eof) {
    rc = read_peer(s, &local);
    if (rc != CAI_OK) {
      failure(s, rc,
              local.message ? local.message : "workspace bootstrap failed");
      s->broken = 1;
      if (!s->state.was_ready) {
        rc = cai_cli_spawn_cancel(s, now, error);
        if (rc != CAI_OK) {
          cai_error_cleanup(&local);
          return rc;
        }
      }
    }
  }
  cai_error_cleanup(&local);
  if (s->eof && !s->state.was_ready && !s->reported_exit && !s->cancelling) {
    failure(s, CAI_ERR_TRANSPORT,
            "workspace bootstrap closed before readiness");
    rc = cai_cli_spawn_cancel(s, now, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (!s->state.was_ready && !s->cancelling && s->startup &&
      now >= s->startup) {
    failure(s, CAI_ERR_TRANSPORT, "workspace startup readiness timed out");
    rc = cai_cli_spawn_cancel(s, now, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (s->cancelling && !s->killed && now >= s->terminate) {
    if (kill(s->pid, SIGKILL) != 0 && errno != ESRCH)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "stop cancelled workspace child");
    s->killed = 1;
    s->terminate = 0U;
  }
  if (!s->eof)
    return CAI_OK;
  do {
    waited = waitpid(s->pid, &status, WNOHANG);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "reap owned workspace child");
  if (waited == 0) {
    s->reap_due = later(now, 1U);
    return CAI_OK;
  }
  s->reaped = 1;
  s->state.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  s->state.exit_code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + s->state.signal;
  if (!s->state.was_ready)
    failure(s, CAI_ERR_TRANSPORT, "workspace child exited before readiness");
  if (!s->reported_exit || s->state.exit_code != s->state.observation.exit_code)
    failure(s, CAI_ERR_TRANSPORT,
            "workspace child exited without matching final metadata");
  if (s->state.exit_code != 0)
    failure(s, CAI_ERR_TRANSPORT, "workspace child exited unsuccessfully");
  s->state.phase = s->state.observation.code != CAI_OK || s->state.exit_code
                       ? CAI_CLI_SPAWN_FAILED
                       : CAI_CLI_SPAWN_EXITED;
  cai_wire_peer_close(s->peer);
  s->peer = NULL;
  s->startup = s->terminate = s->reap_due = 0U;
  return CAI_OK;
}
int cai_cli_spawn_prepare(cai_cli_spawn *s, struct pollfd *interest,
                          uint64_t *deadline, cai_error *error) {
  if (s == NULL || interest == NULL || deadline == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace launch interest required");
  memset(interest, 0, sizeof(*interest));
  interest->fd = s->reaped || s->eof ? -1 : cai_wire_peer_fd(s->peer);
  interest->events = POLLIN;
  *deadline = s->reaped       ? 0U
              : s->eof        ? s->reap_due
              : s->cancelling ? s->terminate
                              : s->startup;
  return CAI_OK;
}
const cai_cli_spawn_state *cai_cli_spawn_get_state(const cai_cli_spawn *s) {
  return s != NULL ? &s->state : NULL;
}
pid_t cai_cli_spawn_pid(const cai_cli_spawn *s) { return s ? s->pid : -1; }
