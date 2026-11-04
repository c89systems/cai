#define _GNU_SOURCE
#include "attach.h"
#include "config.h"
#include "error.h"
#include "frontend.h"
#include "identity.h"
#include "spawn.h"
#include <cai/workspace_discovery.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct startup {
  cai_cli_spawn *child;
  cai_app_client_config target;
  unsigned char instance[16];
} startup;
static uint64_t now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int prepare(void *context, struct pollfd *fd, uint64_t *deadline,
                   cai_error *error) {
  startup *s = context;
  return cai_cli_spawn_prepare(s->child, fd, deadline, error);
}
static int pump(void *context, uint64_t now, cai_app_client_config *target,
                int *ready, cai_error *error) {
  startup *s = context;
  const cai_cli_spawn_state *state;
  int rc = cai_cli_spawn_pump(s->child, now, error);
  *ready = 0;
  if (rc != CAI_OK)
    return rc;
  state = cai_cli_spawn_get_state(s->child);
  if (state->was_ready) {
    memcpy(s->instance, state->observation.instance, sizeof(s->instance));
    *target = s->target;
    target->expected_instance = s->instance;
    *ready = 1;
    return CAI_OK;
  }
  if (state->phase == CAI_CLI_SPAWN_EXITED ||
      state->phase == CAI_CLI_SPAWN_FAILED)
    return cai_cli_error(error,
                         state->observation.code != CAI_OK
                             ? state->observation.code
                             : CAI_ERR_TRANSPORT,
                         state->observation.message[0] != '\0'
                             ? state->observation.message
                             : "workspace server failed during startup");
  return CAI_OK;
}
static int discover(const cai_cli_options *options, char workspace[4096],
                    unsigned char instance[16], int *found, cai_error *error) {
  cai_workspace_discovery_config config;
  cai_workspace_discovery *query = NULL;
  cai_workspace_discovery_state state;
  const cai_workspace_discovery_result *result;
  int rc;
  *found = 0;
  cai_workspace_discovery_config_init(&config);
  config.runtime_directory = options->runtime_dir;
  config.directory = options->directory;
  config.selector =
      options->server != NULL ? options->server : options->directory;
  rc = cai_workspace_discovery_open(&config, now_ms(), &query, error);
  while (rc == CAI_OK) {
    struct pollfd descriptor;
    uint64_t deadline, now;
    rc = cai_workspace_discovery_get_state(query, &state, error);
    if (rc != CAI_OK || state.phase != CAI_WORKSPACE_DISCOVERING)
      break;
    rc = cai_workspace_discovery_prepare(query, &descriptor, &deadline, error);
    if (rc != CAI_OK)
      break;
    now = now_ms();
    if (poll(&descriptor, 1U, deadline > now ? (int)(deadline - now) : 0) < 0 &&
        errno != EINTR) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "wait for workspace discovery");
      break;
    }
    rc = cai_workspace_discovery_pump(query, now_ms(), error);
  }
  if (rc == CAI_OK && state.phase == CAI_WORKSPACE_FOUND) {
    result = cai_workspace_discovery_get_result(query);
    if (result == NULL || strlen(result->record.workspace) >= 4096U)
      rc = cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "invalid discovered workspace");
    else {
      strcpy(workspace, result->record.workspace);
      memcpy(instance, result->record.instance, 16U);
      *found = 1;
    }
  } else if (rc == CAI_OK && state.phase != CAI_WORKSPACE_MISSING &&
             !(options->server == NULL &&
               state.phase == CAI_WORKSPACE_UNAVAILABLE))
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       state.phase == CAI_WORKSPACE_AMBIGUOUS
                           ? "workspace selector is ambiguous"
                           : "workspace server could not be verified");
  cai_workspace_discovery_close(query);
  return rc;
}

int cai_cli_attach_run(const cai_app_config *application, int argc, char **argv,
                       int *exit_code, cai_error *error) {
  cai_config *configuration = NULL;
  cai_cli_options options;
  cai_app_client_config target;
  cai_cli_frontend_boot boot;
  startup s;
  char workspace[4096], program[PATH_MAX];
  unsigned char instance[16];
  ssize_t length;
  int rc, found = 0;
  memset(&s, 0, sizeof(s));
  memset(&boot, 0, sizeof(boot));
  *exit_code = 1;
  rc = cai_cli_configure(argc, argv, &options, &configuration, application,
                         error);
  if (rc <= 0) {
    *exit_code = rc == 0 ? 0 : 2;
    cai_config_close(configuration);
    return rc == 0 ? CAI_OK : CAI_ERR_INVALID;
  }
  rc = cai_cli_private_directory(options.runtime_dir, error);
  if (rc == CAI_OK)
    rc = discover(&options, workspace, instance, &found, error);
  if (rc != CAI_OK)
    goto done;
  cai_app_client_config_init(&target);
  target.runtime_directory = options.runtime_dir;
  if (found) {
    target.workspace = workspace;
    target.expected_instance = instance;
    rc = cai_cli_frontend_run(application, &options, &target, NULL, exit_code,
                              error);
  } else if (options.server != NULL)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "no running server matches --server");
  else {
    length = readlink("/proc/self/exe", program, sizeof(program) - 1U);
    if (length <= 0 || (size_t)length >= sizeof(program) - 1U) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "resolve executable for quiet workspace startup");
      goto done;
    }
    program[length] = '\0';
    target.workspace = options.directory;
    s.target = target;
    rc = cai_cli_spawn_open(program, argc - 1, (const char *const *)(argv + 1),
                            now_ms(), 0U, &s.child, error);
    if (rc != CAI_OK)
      goto done;
    boot.context = &s;
    boot.prepare = prepare;
    boot.pump = pump;
    rc = cai_cli_frontend_run(application, &options, &target, &boot, exit_code,
                              error);
  }
done:
  if (s.child != NULL) {
    const cai_cli_spawn_state *state = cai_cli_spawn_get_state(s.child);
    if (!state->was_ready) {
      (void)cai_cli_spawn_cancel(s.child, now_ms(), NULL);
      while ((state = cai_cli_spawn_get_state(s.child))->phase !=
                 CAI_CLI_SPAWN_EXITED &&
             state->phase != CAI_CLI_SPAWN_FAILED) {
        struct pollfd descriptor;
        uint64_t deadline, now;
        if (cai_cli_spawn_prepare(s.child, &descriptor, &deadline, NULL) !=
            CAI_OK)
          break;
        now = now_ms();
        (void)poll(&descriptor, 1U, deadline > now ? (int)(deadline - now) : 0);
        if (cai_cli_spawn_pump(s.child, now_ms(), NULL) != CAI_OK)
          break;
      }
    }
    /* main exits immediately after this receiver returns. Ready persistent
     * owners and owners with other viewers may survive this attachment. */
    if (cai_cli_spawn_close(s.child, NULL) != CAI_OK)
      cai_cli_spawn_abandon(s.child);
  }
  cai_config_close(configuration);
  return rc;
}
