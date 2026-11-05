#define _POSIX_C_SOURCE 200809L
#include "run_frontend.h"
#include "batch.h"
#include "config.h"
#include "error.h"
#include "existing_frontend.h"
#include "frontend.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Only immutable startup views cross threads. Runtime callbacks stay on the
 * calling owner; the frontend consumes its own Unix socket and status wakeup.
 * No journal, output document or model event queue is copied here. */
typedef struct frontend_run {
  const cai_app_config *application;
  cai_app_bridge *bridge;
  cai_config_context startup;
  cai_config *configuration;
  cai_cli_options options;
  cai_cli_prepared prepared;
  cai_cli_temp temporary;
  cai_app_client_config target;
  cai_cli_frontend_boot boot;
  cai_app_frontend_status status;
  cai_error error;
  char *directory;
  int rc, code;
} frontend_run;

static int agent_mode(const cai_cli_options *o) {
  return cai_cli_batch_mode(o) ||
         (!o->headless && !o->non_interactive && !o->review &&
          !cai_cli_options_utility(o) && isatty(STDIN_FILENO) &&
          isatty(STDOUT_FILENO));
}
static int prepare(void *context, struct pollfd *fd, uint64_t *deadline,
                   cai_error *error) {
  frontend_run *r = context;
  (void)error;
  memset(fd, 0, sizeof(*fd));
  fd->fd = r->bridge->frontend_fd;
  fd->events = POLLIN;
  *deadline = 0U;
  return CAI_OK;
}
static int pump(void *context, uint64_t now, cai_app_client_config *target,
                int *ready, cai_error *error) {
  frontend_run *r = context;
  int rc;
  (void)now;
  *ready = 0;
  rc = cai_app_bridge_frontend_observe(r->bridge, &r->status, error);
  if (rc != CAI_OK)
    return rc;
  if (!r->status.running ||
      (r->status.has_server && r->status.phase == CAI_ATTACHMENT_STOPPING)) {
    int stopping = cai_app_bridge_stopping(r->bridge);
    if (r->status.exit_code != 0 || (r->status.code != CAI_OK && !stopping))
      return cai_cli_error(
          error, r->status.code != CAI_OK ? r->status.code : CAI_ERR_TRANSPORT,
          r->status.reason[0] != '\0'
              ? r->status.reason
              : "application owner failed during startup");
    if (cai_cli_batch_mode(&r->options) && !stopping)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "workspace owner stopped before batch completion");
    *ready = CAI_CLI_FRONTEND_STOPPED;
  } else if (r->status.ready && r->status.has_server &&
             r->status.phase == CAI_ATTACHMENT_READY) {
    *target = r->target;
    target->expected_instance = r->status.instance;
    *ready = CAI_CLI_FRONTEND_STARTED;
  }
  return CAI_OK;
}
static void *frontend(void *context) {
  frontend_run *r = context;
  r->rc = cai_cli_batch_mode(&r->options)
              ? cai_cli_batch_run(r->application, &r->options, &r->target,
                                  &r->boot, &r->code, &r->error)
              : cai_cli_frontend_run(r->application, &r->options, &r->target,
                                     &r->boot, &r->code, &r->error);
  /* Terminal return ends only the controller owned by this run. Other visited
   * workspace owners retain their normal attachment/persistence lifetimes. */
  cai_app_bridge_frontend_done(r->bridge);
  return NULL;
}
int cai_cli_run_frontend(const cai_app_config *application,
                         cai_app_bridge *bridge, int argc, char **argv,
                         const cai_cli_prepared *resolved, int *handled,
                         int *exit_code, cai_error *error) {
  frontend_run *r;
  cai_cli_options early;
  cai_attachment_config lifetime;
  pthread_t thread;
  int parsed, rc = CAI_OK, code = 1, started = 0;
  *handled = 0;
  if (resolved != NULL)
    early = resolved->options;
  else if (cai_cli_read_options(argc, argv, NULL, &early) != 1)
    return CAI_OK;
  if (!agent_mode(&early))
    return CAI_OK;
  r = calloc(1U, sizeof(*r));
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate frontend composition");
  r->application = application;
  r->bridge = bridge;
  cai_cli_temp_init(&r->temporary);
  cai_error_init(&r->error);
  r->directory =
      resolved != NULL ? strdup(resolved->options.directory) : getcwd(NULL, 0U);
  if (r->directory == NULL)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "capture application directory");
  if (rc == CAI_OK) {
    if (resolved != NULL && resolved->options.environment != NULL) {
      rc = cai_environment_retain(resolved->options.environment, error);
      if (rc == CAI_OK)
        r->startup.environment = resolved->options.environment;
    } else
      rc = cai_environment_open(NULL, &r->startup.environment, error);
  }
  r->startup.directory = r->directory;
  if (rc != CAI_OK)
    goto done;
  if (resolved != NULL) {
    r->options = resolved->options;
    r->options.environment = NULL;
    r->configuration = resolved->configuration;
    parsed = 1;
  } else
    parsed = cai_cli_configure_at(argc, argv, &r->options, &r->configuration,
                                  application, &r->startup, &r->error);
  if (parsed <= 0 || !agent_mode(&r->options)) {
    /* Configuration errors still use the common diagnostic path. A completed
     * help/dump utility has already written its explicit result exactly once.
     */
    if (parsed == 0) {
      *handled = 1;
      *exit_code = 0;
    }
    goto done;
  }
  *handled = 1;
  rc = cai_config_environment(r->configuration, &r->options.environment, error);
  if (rc != CAI_OK)
    goto done;
  r->prepared.configuration = r->configuration;
  r->prepared.normal_options = r->options;
  if (r->options.temporary) {
    rc = cai_cli_temp_open(&r->temporary, &r->options, error);
    if (rc == CAI_OK)
      rc = cai_cli_temp_runtime_open(&r->temporary, &r->options, error);
    if (rc != CAI_OK)
      goto done;
    r->prepared.temporary = &r->temporary;
    if (cai_cli_batch_mode(&r->options)) {
      cai_cli_log *log = NULL;
      pslog_field field = pslog_str("workspace", r->temporary.workspace);
      rc = cai_cli_log_new_at(&log, PSLOG_LEVEL_INFO, 0, application->logger,
                              r->options.environment, r->options.directory,
                              error);
      if (rc == CAI_OK)
        cai_cli_log_message(log->logger, PSLOG_LEVEL_INFO,
                            "temp_workspace_opened", "diagnostic",
                            "Temporary workspace created", 27U, &field, 1U);
      if (cai_cli_log_release(log) != 0 && rc == CAI_OK)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "write temporary workspace notice");
      if (rc != CAI_OK)
        goto done;
    }
  }
  r->prepared.options = r->options;
  if (r->options.server != NULL) {
    rc = cai_cli_existing_frontend(application, bridge, &r->options,
                                   r->configuration, exit_code, error);
    goto done;
  }
  cai_app_client_config_init(&r->target);
  r->target.runtime_directory = r->options.runtime_dir;
  r->target.workspace = r->options.directory;
  r->boot.context = r;
  r->boot.prepare = prepare;
  r->boot.pump = pump;
  rc = cai_app_bridge_frontend_open(bridge, error);
  if (rc != CAI_OK)
    goto done;
  parsed = pthread_create(&thread, NULL, frontend, r);
  if (parsed != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "start frontend adapter");
    goto done;
  }
  started = 1;
  cai_attachment_config_init(&lifetime);
  code = cai_cli_app_controller_run(
      argc, argv, application, bridge, &r->startup, &lifetime, 0,
      cai_cli_batch_mode(&r->options) ? CAI_CLI_OWNER_BATCH_FRONTEND
                                      : CAI_CLI_OWNER_FRONTEND,
      &r->prepared);
  /* Wake a terminal whose owner failed or stopped before it could attach.
   * The outer run finalizes the same status after joining this receiver. */
  cai_app_bridge_frontend_owner_end(bridge, code);
  (void)pthread_join(thread, NULL); /* Exact owned, joinable frontend. */
  if (r->rc != CAI_OK && code == 0)
    rc = cai_cli_error(error, r->rc,
                       r->error.message != NULL ? r->error.message
                                                : "terminal attachment failed");
  *exit_code = code != 0 ? code : r->code;
done:
  if (r->temporary.parent_fd >= 0) {
    cai_error cleanup;
    int kept = 0, cleanup_rc, stopped;
    cai_error_init(&cleanup);
    /* Both the terminal and the owner (including store clients, readers and
     * endpoint ownership) have returned before this plain-input question.
     * Host STOP remains accepted until run() itself returns. */
    stopped = cai_app_bridge_cleanup_stop(bridge);
    cleanup_rc = cai_cli_temp_close(
        &r->temporary, r->options.force,
        started && !cai_cli_batch_mode(&r->options) && rc == CAI_OK &&
            *exit_code == 0 && !stopped,
        bridge->fd, cai_app_bridge_cleanup_stop, bridge, &kept, &cleanup);
    if (kept) {
      cai_cli_log *log = NULL;
      pslog_logger *logger = application->logger;
      pslog_field field = pslog_str("workspace", r->temporary.workspace);
      if (cai_cli_batch_mode(&r->options) && logger == NULL) {
        int log_rc = cai_cli_log_new_at(&log, PSLOG_LEVEL_INFO, 0, NULL,
                                        r->options.environment,
                                        r->options.directory, &cleanup);
        if (log_rc != CAI_OK)
          cleanup_rc = log_rc;
        else
          logger = log->logger;
      }
      cai_cli_log_message(logger, PSLOG_LEVEL_INFO, "temp_workspace_kept",
                          "diagnostic", "Temporary workspace kept", 24U, &field,
                          1U);
      if (!cai_cli_batch_mode(&r->options))
        fprintf(stdout, "\nKept temporary workspace: %s\n",
                r->temporary.workspace);
      if (cai_cli_log_release(log) != 0 && cleanup_rc == CAI_OK)
        cleanup_rc = cai_cli_error(&cleanup, CAI_ERR_TRANSPORT,
                                   "write temporary workspace notice");
    }
    if (cleanup_rc != CAI_OK) {
      cai_app_bridge_error(bridge, &cleanup);
      if (rc == CAI_OK)
        rc = cai_cli_error(error, cleanup_rc,
                           cleanup.message != NULL
                               ? cleanup.message
                               : "clean temporary workspace");
      *exit_code = 1;
    }
    cai_error_cleanup(&cleanup);
  }
  if (*handled && rc == CAI_OK && *exit_code == 0 &&
      !cai_app_bridge_cleanup_stop(bridge))
    cai_app_bridge_outcome(bridge, CAI_APP_COMPLETED);
  if (*handled || started)
    cai_app_bridge_frontend_close(bridge);
  cai_environment_close(r->options.environment);
  cai_environment_close(r->startup.environment);
  if (resolved == NULL)
    cai_config_close(r->configuration);
  cai_error_cleanup(&r->error);
  free(r->directory);
  free(r);
  return rc;
}
