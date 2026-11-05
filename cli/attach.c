#define _GNU_SOURCE
#include "attach.h"
#include "batch.h"
#include "config.h"
#include "error.h"
#include "frontend.h"
#include "identity.h"
#include "run_frontend.h"
#include "spawn.h"
#include "theme.h"
#include "workspace_claims.h"
#include <cai/environment.h>
#include <cai/workspace_discovery.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** Initial lookup and a possible quiet launch share one readiness interest.
 * The editor owns neither this state nor the process. A losing launch is
 * reaped before a fresh identity-only lookup can select the winning owner. */
struct cai_cli_attach_boot {
  cai_cli_frontend_boot adapter;
  const cai_cli_options *options;
  const char *program;
  int argc;
  const char *const *argv;
  cai_workspace_discovery *query;
  cai_cli_spawn *child;
  cai_app_client_config target;
  unsigned char instance[16];
  char workspace[4096];
  char failure[CAI_CLI_BOOTSTRAP_MESSAGE];
  int resolved, reconciled, failure_code;
};
static uint64_t now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int prepare(void *context, struct pollfd *fd, uint64_t *deadline,
                   cai_error *error) {
  cai_cli_attach_boot *s = context;
  if (s->query != NULL)
    return cai_workspace_discovery_prepare(s->query, fd, deadline, error);
  if (s->child != NULL)
    return cai_cli_spawn_prepare(s->child, fd, deadline, error);
  memset(fd, 0, sizeof(*fd));
  fd->fd = -1;
  *deadline = 0U;
  return CAI_OK;
}
static int discover(cai_cli_attach_boot *s, uint64_t now, cai_error *error) {
  cai_workspace_discovery_config config;
  cai_workspace_discovery_config_init(&config);
  config.runtime_directory = s->options->runtime_dir;
  config.directory = s->options->directory;
  config.selector =
      s->options->server != NULL ? s->options->server : s->options->directory;
  return cai_workspace_discovery_open(&config, now, &s->query, error);
}
static int launch(cai_cli_attach_boot *s, uint64_t now, cai_error *error) {
  char program[PATH_MAX];
  const char *executable = s->program;
  if (executable == NULL) {
    ssize_t n = readlink("/proc/self/exe", program, sizeof(program) - 1U);
    if (n <= 0 || (size_t)n >= sizeof(program) - 1U)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "resolve executable for quiet workspace startup");
    program[n] = '\0';
    executable = program;
  }
  return cai_cli_spawn_open(executable, s->argc, s->argv, now, 0U, &s->child,
                            error);
}
static int pump(void *context, uint64_t now, cai_app_client_config *target,
                int *ready, cai_error *error) {
  cai_cli_attach_boot *s = context;
  int rc;
  *ready = 0;
  if (s->child != NULL) {
    const cai_cli_spawn_state *state;
    rc = cai_cli_spawn_pump(s->child, now, error);
    if (rc != CAI_OK)
      return rc;
    state = cai_cli_spawn_get_state(s->child);
    if (state->was_ready) {
      memcpy(s->instance, state->observation.instance, sizeof(s->instance));
      s->resolved = CAI_CLI_FRONTEND_STARTED;
    } else if (state->phase == CAI_CLI_SPAWN_EXITED ||
               state->phase == CAI_CLI_SPAWN_FAILED) {
      s->failure_code = state->observation.code != CAI_OK
                            ? state->observation.code
                            : CAI_ERR_TRANSPORT;
      snprintf(s->failure, sizeof(s->failure), "%s",
               state->observation.message[0] != '\0'
                   ? state->observation.message
                   : "workspace server failed during startup");
      /* A limit can be an endpoint/name claim conflict. It is not proof of
       * a usable winner: reconcile once through the normal verified probe.
       * Other startup failures and failed reconciliation retain the error. */
      if (s->failure_code != CAI_ERR_LIMIT || s->reconciled)
        return cai_cli_error(error, s->failure_code, s->failure);
      rc = cai_cli_spawn_close(s->child, error);
      if (rc != CAI_OK)
        return rc;
      s->child = NULL;
      s->reconciled = 1;
      rc = discover(s, now, error);
      if (rc != CAI_OK)
        return rc;
    }
  }
  if (s->query != NULL) {
    cai_workspace_discovery_state state;
    rc = cai_workspace_discovery_pump(s->query, now, error);
    if (rc == CAI_OK)
      rc = cai_workspace_discovery_get_state(s->query, &state, error);
    if (rc != CAI_OK)
      return rc;
    if (state.phase == CAI_WORKSPACE_FOUND) {
      const cai_workspace_discovery_result *result =
          cai_workspace_discovery_get_result(s->query);
      if (result == NULL ||
          strlen(result->record.workspace) >= sizeof(s->workspace))
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "invalid discovered workspace");
      strcpy(s->workspace, result->record.workspace);
      memcpy(s->instance, result->record.instance, sizeof(s->instance));
      s->resolved = CAI_CLI_FRONTEND_EXISTING;
    } else if (state.phase == CAI_WORKSPACE_DISCOVERING)
      return CAI_OK;
    else if (s->reconciled)
      return cai_cli_error(error, s->failure_code, s->failure);
    else if (state.phase != CAI_WORKSPACE_MISSING &&
             !(s->options->server == NULL &&
               state.phase == CAI_WORKSPACE_UNAVAILABLE))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           state.phase == CAI_WORKSPACE_AMBIGUOUS
                               ? "workspace selector is ambiguous"
                               : "workspace server could not be verified");
    else if (s->options->server != NULL)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "no running server matches --server");
    cai_workspace_discovery_close(s->query);
    s->query = NULL;
    if (!s->resolved)
      return launch(s, now, error);
  }
  if (s->resolved) {
    *target = s->target;
    *ready = s->resolved;
  }
  return CAI_OK;
}
int cai_cli_attach_boot_open(const cai_cli_options *options,
                             const char *program, int argc,
                             const char *const *argv, cai_cli_attach_boot **out,
                             cai_error *error) {
  cai_cli_attach_boot *s;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (options == NULL || out == NULL || options->directory == NULL ||
      options->runtime_dir == NULL || argc < 0 || (argc != 0 && argv == NULL) ||
      strlen(options->directory) >= 4096U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace startup options and output required");
  s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace startup");
  s->options = options;
  s->program = program;
  s->argc = argc;
  s->argv = argv;
  strcpy(s->workspace, options->directory);
  cai_app_client_config_init(&s->target);
  s->target.runtime_directory = options->runtime_dir;
  s->target.workspace = s->workspace;
  s->target.expected_instance = s->instance;
  s->target.requirements = options->workspace_requirements;
  s->target.requirement_count = options->workspace_requirement_count;
  s->adapter.context = s;
  s->adapter.prepare = prepare;
  s->adapter.pump = pump;
  rc = discover(s, now_ms(), error);
  if (rc == CAI_OK)
    *out = s;
  else
    free(s);
  return rc;
}
const cai_cli_frontend_boot *
cai_cli_attach_boot_adapter(cai_cli_attach_boot *s) {
  return s != NULL ? &s->adapter : NULL;
}

int cai_cli_attach_run(const cai_app_config *application, int argc, char **argv,
                       int *exit_code, cai_error *error) {
  cai_config *configuration = NULL;
  cai_environment *environment = NULL;
  cai_cli_options options;
  cai_cli_workspace_claims requirements = {0};
  cai_app_client_config target;
  cai_cli_attach_boot *startup = NULL;
  int rc;
  *exit_code = 1;
  rc = cai_cli_configure(argc, argv, &options, &configuration, application,
                         error);
  if (rc > 0 && cai_cli_theme_validate(&options, error) != CAI_OK)
    rc = -1;
  if (rc <= 0) {
    *exit_code = rc == 0 ? 0 : 2;
    cai_config_close(configuration);
    return rc == 0 ? CAI_OK : CAI_ERR_INVALID;
  }
  rc = cai_config_environment(configuration, &environment, error);
  options.environment = environment;
  if (rc == CAI_OK && options.temporary) {
    cai_app_bridge *bridge = NULL;
    cai_cli_prepared resolved;
    int handled = 0;
    memset(&resolved, 0, sizeof(resolved));
    resolved.configuration = configuration;
    resolved.options = options;
    rc = cai_app_bridge_open(&bridge, application, error);
    if (rc == CAI_OK) {
      cai_app_bridge_begin(bridge);
      rc = cai_cli_run_frontend(application, bridge, argc, argv, &resolved,
                                &handled, exit_code, error);
      cai_app_bridge_end(bridge, rc == CAI_OK ? *exit_code : 1,
                         rc != CAI_OK ? error : NULL);
      cai_app_bridge_close(bridge);
      if (rc == CAI_OK && !handled)
        rc = cai_cli_error(error, CAI_ERR_INVALID,
                           "temporary startup requires an agent frontend");
    }
    goto done;
  }
  if (rc == CAI_OK)
    rc = cai_cli_private_directory(options.runtime_dir, error);
  if (rc == CAI_OK)
    rc = cai_cli_workspace_claims_build(&options, configuration, 1,
                                        &requirements, error);
  options.workspace_requirements = requirements.values;
  options.workspace_requirement_count = requirements.count;
  if (rc == CAI_OK)
    rc = cai_cli_attach_boot_open(&options, NULL, argc - 1,
                                  (const char *const *)(argv + 1), &startup,
                                  error);
  if (rc != CAI_OK)
    goto done;
  cai_app_client_config_init(&target);
  target.runtime_directory = options.runtime_dir;
  target.workspace = options.directory;
  rc = cai_cli_batch_mode(&options)
           ? cai_cli_batch_run(application, &options, &target,
                               cai_cli_attach_boot_adapter(startup), exit_code,
                               error)
           : cai_cli_frontend_run(application, &options, &target,
                                  cai_cli_attach_boot_adapter(startup),
                                  exit_code, error);
done:
  cai_cli_attach_boot_close(startup);
  cai_cli_workspace_claims_cleanup(&requirements);
  cai_environment_close(environment);
  cai_config_close(configuration);
  return rc;
}
void cai_cli_attach_boot_close(cai_cli_attach_boot *s) {
  if (s == NULL)
    return;
  cai_workspace_discovery_close(s->query);
  if (s->child != NULL) {
    const cai_cli_spawn_state *state = cai_cli_spawn_get_state(s->child);
    if (!state->was_ready) {
      (void)cai_cli_spawn_cancel(s->child, now_ms(), NULL);
      while ((state = cai_cli_spawn_get_state(s->child))->phase !=
                 CAI_CLI_SPAWN_EXITED &&
             state->phase != CAI_CLI_SPAWN_FAILED) {
        struct pollfd descriptor;
        uint64_t deadline, now;
        if (cai_cli_spawn_prepare(s->child, &descriptor, &deadline, NULL) !=
            CAI_OK)
          break;
        now = now_ms();
        (void)poll(&descriptor, 1U, deadline > now ? (int)(deadline - now) : 0);
        if (cai_cli_spawn_pump(s->child, now_ms(), NULL) != CAI_OK)
          break;
      }
    }
    /* main exits immediately after this receiver returns. Ready persistent
     * owners and owners with other viewers may survive this attachment. */
    if (cai_cli_spawn_close(s->child, NULL) != CAI_OK)
      cai_cli_spawn_abandon(s->child);
  }
  free(s);
}
