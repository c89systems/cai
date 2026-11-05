#define _POSIX_C_SOURCE 200809L
#include "existing_frontend.h"
#include "batch.h"
#include "error.h"
#include "frontend.h"
#include "workspace_claims.h"
#include <cai/workspace_discovery.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
typedef struct existing_frontend {
  cai_app_bridge *bridge;
  cai_workspace_discovery *query;
  cai_cli_workspace_claims requirements;
  cai_app_client_config target;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  char workspace[4096];
  int epoll, watched, found;
} existing_frontend;
static uint64_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int prepare(void *context, struct pollfd *fd, uint64_t *deadline,
                   cai_error *error) {
  existing_frontend *f = context;
  struct pollfd query;
  struct epoll_event event;
  int rc = cai_workspace_discovery_prepare(f->query, &query, deadline, error);
  if (rc != CAI_OK)
    return rc;
  if (f->watched != query.fd) {
    if (f->watched >= 0 &&
        epoll_ctl(f->epoll, EPOLL_CTL_DEL, f->watched, NULL) != 0 &&
        errno != ENOENT && errno != EBADF)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "unwatch workspace probe");
    f->watched = -1;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    if (query.events & POLLOUT)
      event.events |= EPOLLOUT;
    if (query.fd >= 0 &&
        epoll_ctl(f->epoll, EPOLL_CTL_ADD, query.fd, &event) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "watch workspace probe");
    f->watched = query.fd;
  } else if (query.fd >= 0) {
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLRDHUP;
    if (query.events & POLLOUT)
      event.events |= EPOLLOUT;
    if (epoll_ctl(f->epoll, EPOLL_CTL_MOD, query.fd, &event) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "update workspace probe");
  }
  memset(fd, 0, sizeof(*fd));
  fd->fd = f->epoll;
  fd->events = POLLIN;
  return CAI_OK;
}
static int pump(void *context, uint64_t now, cai_app_client_config *target,
                int *ready, cai_error *error) {
  existing_frontend *f = context;
  struct epoll_event events[4];
  cai_workspace_discovery_state state;
  int rc;
  *ready = 0;
  if (epoll_wait(f->epoll, events, 4, 0) < 0 && errno != EINTR)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "observe workspace probe");
  if (cai_app_bridge_cleanup_stop(f->bridge)) {
    *ready = CAI_CLI_FRONTEND_DETACH;
    return CAI_OK;
  }
  if (!f->found) {
    rc = cai_workspace_discovery_pump(f->query, now, error);
    if (rc == CAI_OK)
      rc = cai_workspace_discovery_get_state(f->query, &state, error);
    if (rc != CAI_OK)
      return rc;
    if (state.phase == CAI_WORKSPACE_FOUND) {
      const cai_workspace_discovery_result *result =
          cai_workspace_discovery_get_result(f->query);
      if (result == NULL ||
          strlen(result->record.workspace) >= sizeof(f->workspace))
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "invalid workspace identity");
      strcpy(f->workspace, result->record.workspace);
      memcpy(f->instance, result->record.instance, sizeof(f->instance));
      f->found = 1;
    } else if (state.phase != CAI_WORKSPACE_DISCOVERING)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           state.phase == CAI_WORKSPACE_MISSING
                               ? "no running server matches --server"
                           : state.phase == CAI_WORKSPACE_AMBIGUOUS
                               ? "workspace selector is ambiguous"
                               : "workspace server could not be verified");
  }
  if (f->found) {
    *target = f->target;
    *ready = CAI_CLI_FRONTEND_EXISTING;
  }
  return CAI_OK;
}
int cai_cli_existing_frontend(const cai_app_config *app, cai_app_bridge *bridge,
                              const cai_cli_options *options,
                              cai_config *configuration, int *code,
                              cai_error *error) {
  existing_frontend *f = calloc(1U, sizeof(*f));
  cai_workspace_discovery_config discovery;
  cai_cli_frontend_boot boot;
  struct epoll_event event;
  int rc;
  if (f == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate existing attachment");
  f->bridge = bridge;
  if (strlen(options->directory) >= sizeof(f->workspace)) {
    free(f);
    return cai_cli_error(error, CAI_ERR_LIMIT, "workspace path exceeds limit");
  }
  strcpy(f->workspace, options->directory);
  f->watched = -1;
  f->epoll = epoll_create1(EPOLL_CLOEXEC);
  memset(&event, 0, sizeof(event));
  event.events = EPOLLIN;
  rc = f->epoll >= 0 &&
               epoll_ctl(f->epoll, EPOLL_CTL_ADD, bridge->fd, &event) == 0
           ? CAI_OK
           : cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "watch attachment controls");
  if (rc == CAI_OK)
    rc = cai_cli_workspace_claims_build(options, configuration, 1,
                                        &f->requirements, error);
  cai_workspace_discovery_config_init(&discovery);
  discovery.runtime_directory = options->runtime_dir;
  discovery.directory = options->directory;
  discovery.selector = options->server;
  if (rc == CAI_OK)
    rc = cai_workspace_discovery_open(&discovery, clock_ms(), &f->query, error);
  cai_app_client_config_init(&f->target);
  f->target.runtime_directory = options->runtime_dir;
  f->target.workspace = f->workspace;
  f->target.expected_instance = f->instance;
  f->target.requirements = f->requirements.values;
  f->target.requirement_count = f->requirements.count;
  boot.context = f;
  boot.prepare = prepare;
  boot.pump = pump;
  if (rc == CAI_OK)
    rc = cai_cli_batch_mode(options)
             ? cai_cli_batch_run(app, options, &f->target, &boot, code, error)
             : cai_cli_frontend_run(app, options, &f->target, &boot, code,
                                    error);
  cai_workspace_discovery_close(f->query);
  if (f->epoll >= 0)
    close(f->epoll);
  free(f);
  return rc;
}
