#define _POSIX_C_SOURCE 200809L
#include "run_terminal.h"
#include "config.h"
#include "error.h"
#include "frontend.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Only immutable startup views cross threads. Runtime callbacks stay on the
 * calling owner; the frontend consumes its own Unix socket and status wakeup.
 * No journal, output document or model event queue is copied here. */
typedef struct terminal_run {
  const cai_app_config *application;
  cai_app_bridge *bridge;
  cai_config_context startup;
  cai_config *configuration;
  cai_cli_options options;
  cai_app_client_config target;
  cai_cli_frontend_boot boot;
  cai_app_frontend_status status;
  cai_error error;
  char *directory;
  int rc, code;
} terminal_run;

static int agent_mode(const cai_cli_options *o) {
  return !o->headless && o->server == NULL && !o->temporary &&
         !o->non_interactive && !o->review && !o->list && !o->list_all &&
         !o->resume_list && !o->delete_list && o->delete_id == NULL &&
         o->export_id == NULL && o->import_file == NULL && !o->login &&
         !o->chrome_setup && !o->chrome_remove && o->install_skill == NULL &&
         o->message == NULL;
}
static int prepare(void *context, struct pollfd *fd, uint64_t *deadline,
                   cai_error *error) {
  terminal_run *r = context;
  (void)error;
  memset(fd, 0, sizeof(*fd));
  fd->fd = r->bridge->frontend_fd;
  fd->events = POLLIN;
  *deadline = 0U;
  return CAI_OK;
}
static int pump(void *context, uint64_t now, cai_app_client_config *target,
                int *ready, cai_error *error) {
  terminal_run *r = context;
  int rc;
  (void)now;
  *ready = 0;
  rc = cai_app_bridge_frontend_observe(r->bridge, &r->status, error);
  if (rc != CAI_OK)
    return rc;
  if (!r->status.running ||
      (r->status.has_server && r->status.phase == CAI_ATTACHMENT_STOPPING)) {
    if (r->status.exit_code != 0)
      return cai_cli_error(
          error, r->status.code != CAI_OK ? r->status.code : CAI_ERR_TRANSPORT,
          r->status.reason[0] != '\0'
              ? r->status.reason
              : "application owner failed during startup");
    *ready = CAI_CLI_FRONTEND_STOPPED;
  } else if (r->status.ready && r->status.has_server &&
             r->status.phase == CAI_ATTACHMENT_READY) {
    *target = r->target;
    target->expected_instance = r->status.instance;
    *ready = CAI_CLI_FRONTEND_STARTED;
  }
  return CAI_OK;
}
static void *terminal(void *context) {
  terminal_run *r = context;
  r->rc = cai_cli_frontend_run(r->application, &r->options, &r->target,
                               &r->boot, &r->code, &r->error);
  /* Terminal return ends only the controller owned by this run. Other visited
   * workspace owners retain their normal attachment/persistence lifetimes. */
  cai_app_bridge_frontend_done(r->bridge);
  return NULL;
}
int cai_cli_run_terminal(const cai_app_config *application,
                         cai_app_bridge *bridge, int argc, char **argv,
                         int *handled, int *exit_code, cai_error *error) {
  terminal_run *r;
  cai_cli_options early;
  cai_attachment_config lifetime;
  pthread_t thread;
  int parsed, rc = CAI_OK, code = 1, started = 0;
  *handled = 0;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
      cai_cli_read_options(argc, argv, NULL, &early) != 1 ||
      !agent_mode(&early))
    return CAI_OK;
  r = calloc(1U, sizeof(*r));
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate terminal composition");
  r->application = application;
  r->bridge = bridge;
  cai_error_init(&r->error);
  r->directory = getcwd(NULL, 0U);
  if (r->directory == NULL)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                       "capture application directory");
  if (rc == CAI_OK)
    rc = cai_environment_open(NULL, &r->startup.environment, error);
  r->startup.directory = r->directory;
  if (rc != CAI_OK)
    goto done;
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
  cai_app_client_config_init(&r->target);
  r->target.runtime_directory = r->options.runtime_dir;
  r->target.workspace = r->options.directory;
  r->boot.context = r;
  r->boot.prepare = prepare;
  r->boot.pump = pump;
  rc = cai_app_bridge_frontend_open(bridge, error);
  if (rc != CAI_OK)
    goto done;
  parsed = pthread_create(&thread, NULL, terminal, r);
  if (parsed != 0) {
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "start terminal adapter");
    goto done;
  }
  started = 1;
  cai_attachment_config_init(&lifetime);
  code = cai_cli_app_controller_run(argc, argv, application, bridge,
                                    &r->startup, &lifetime, 0);
  /* Wake a terminal whose owner failed or stopped before it could attach.
   * The outer run finalizes the same status after joining this receiver. */
  cai_app_bridge_end(bridge, code, NULL);
  (void)pthread_join(thread, NULL); /* Exact owned, joinable frontend. */
  if (r->rc != CAI_OK && code == 0)
    rc = cai_cli_error(error, r->rc,
                       r->error.message != NULL ? r->error.message
                                                : "terminal attachment failed");
  *exit_code = code != 0 ? code : r->code;
  if (rc == CAI_OK && *exit_code == 0) {
    int host_stop;
    pthread_mutex_lock(&bridge->lock);
    host_stop = bridge->stop_requested;
    pthread_mutex_unlock(&bridge->lock);
    if (!host_stop)
      cai_app_bridge_outcome(bridge, CAI_APP_COMPLETED);
  }
done:
  if (*handled || started)
    cai_app_bridge_frontend_close(bridge);
  cai_environment_close(r->options.environment);
  cai_environment_close(r->startup.environment);
  cai_config_close(r->configuration);
  cai_error_cleanup(&r->error);
  free(r->directory);
  free(r);
  return rc;
}
