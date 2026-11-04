#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include <cai/workspace.h>
#include <cai/workspace_discovery.h>
#include <pthread.h>
#include <string.h>

struct cai_workspace_discovery {
  cai_workspace_discovery_config config;
  cai_workspace_discovery_state state;
  cai_workspace_registry *registry;
  cai_workspace_endpoint *path;
  cai_workspace_client *probe;
  cai_workspace_discovery_result candidate, found;
  char workspace[4096], name[256], id[33];
  char *runtime, *selector;
  pthread_t owner;
  uint64_t now;
  int pumping, direct_done, unavailable;
};
static int valid(cai_workspace_discovery *d, cai_error *error) {
  return d != NULL && pthread_equal(d->owner, pthread_self())
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_INVALID,
                             "workspace discovery requires its owner thread");
}
static int failed(cai_workspace_discovery *d, int rc, cai_error *error) {
  d->state.phase = CAI_WORKSPACE_DISCOVERY_FAILED;
  d->state.error_code = rc;
  cai_workspace_client_close(d->probe);
  d->probe = NULL;
  if (error != NULL && error->code == CAI_OK)
    cai_set_error(error, rc, "workspace discovery failed");
  return rc;
}
static int notify(cai_workspace_discovery *d,
                  const cai_workspace_discovery_result *entry,
                  const cai_error *cause, cai_error *error) {
  cai_workspace_discovery_event event;
  event.entry = entry;
  event.error = cause;
  return d->config.event ? d->config.event(d->config.context, &event, error)
                         : CAI_OK;
}
static int explicit_path(const char *selector) {
  return selector[0] == '/' || strchr(selector, '/') != NULL ||
         strcmp(selector, ".") == 0 || strcmp(selector, "..") == 0;
}
void cai_workspace_discovery_config_init(
    cai_workspace_discovery_config *config) {
  if (config)
    memset(config, 0, sizeof(*config));
}
int cai_workspace_discovery_open(const cai_workspace_discovery_config *config,
                                 uint64_t now, cai_workspace_discovery **out,
                                 cai_error *error) {
  cai_workspace_discovery *d;
  char *path = NULL;
  int rc;
  if (out)
    *out = NULL;
  if (config == NULL || out == NULL || config->runtime_directory == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery configuration and output required");
  d = cai_alloc(NULL, sizeof(*d));
  if (!d)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate workspace discovery");
  memset(d, 0, sizeof(*d));
  d->owner = pthread_self();
  d->now = now;
  d->config = *config;
  d->config.runtime_directory = d->config.selector = d->config.directory = NULL;
  d->runtime = cai_strdup(NULL, config->runtime_directory);
  d->selector = cai_strdup(NULL, config->selector ? config->selector : "");
  rc = d->runtime && d->selector
           ? CAI_OK
           : cai_set_error(error, CAI_ERR_NOMEM, "copy discovery context");
  if (!d->config.probe_timeout_ms)
    d->config.probe_timeout_ms = 10000U;
  if (rc == CAI_OK)
    rc = cai_workspace_registry_open(d->runtime, &d->registry, error);
  if (rc == CAI_OK && explicit_path(d->selector)) {
    if (d->selector[0] == '/')
      path = cai_strdup(NULL, d->selector);
    else if (config->directory == NULL || config->directory[0] != '/')
      rc = cai_set_error(
          error, CAI_ERR_INVALID,
          "relative workspace selector requires an absolute base directory");
    else {
      size_t n = strlen(config->directory) + strlen(d->selector) + 2U;
      if (strlen(config->directory) >= sizeof(d->workspace) ||
          strlen(d->selector) >= sizeof(d->workspace))
        rc = cai_set_error(error, CAI_ERR_INVALID,
                           "workspace selector path exceeds bounds");
      else {
        path = cai_alloc(NULL, n);
        if (path)
          snprintf(path, n, "%s/%s", config->directory, d->selector);
      }
    }
    if (rc == CAI_OK)
      rc = path ? cai_workspace_endpoint_open(d->runtime, path, &d->path, error)
                : cai_set_error(error, CAI_ERR_NOMEM,
                                "copy workspace selector path");
  }
  cai_free_mem(NULL, path);
  if (rc == CAI_OK)
    *out = d;
  else
    cai_workspace_discovery_close(d);
  return rc;
}
int cai_workspace_discovery_prepare(cai_workspace_discovery *d,
                                    struct pollfd *fd, uint64_t *deadline,
                                    cai_error *error) {
  int rc = valid(d, error);
  if (rc != CAI_OK)
    return rc;
  if (fd == NULL || deadline == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery poll outputs required");
  memset(fd, 0, sizeof(*fd));
  fd->fd = -1;
  *deadline = 0U;
  if (d->state.phase != CAI_WORKSPACE_DISCOVERING)
    return CAI_OK;
  if (d->probe)
    return cai_workspace_client_prepare(d->probe, fd, deadline, error);
  *deadline = d->now ? d->now : 1U;
  return CAI_OK;
}
static int unavailable(cai_workspace_discovery *d, const cai_error *cause,
                       cai_error *error) {
  int rc;
  ++d->state.errors;
  d->state.error_code = cause->code;
  if (d->path || cai_workspace_registry_prune(d->registry, NULL) != CAI_OK)
    d->unavailable = 1;
  rc = notify(d, &d->candidate, cause, error);
  cai_workspace_client_close(d->probe);
  d->probe = NULL;
  return rc;
}
static int found(cai_workspace_discovery *d,
                 const cai_workspace_client_state *state, cai_error *error) {
  const cai_workspace_record *record = &d->candidate.record;
  int rc;
  d->candidate.phase = state->server_phase;
  d->candidate.persist = state->persist;
  memcpy(d->candidate.record.instance, state->instance,
         sizeof(state->instance));
  ++d->state.matches;
  if (d->selector[0]) {
    if (!d->state.first[0]) {
      size_t n = strlen(record->workspace), m = strlen(record->name);
      if (n >= sizeof(d->workspace) || m >= sizeof(d->name))
        return cai_set_error(error, CAI_ERR_LIMIT,
                             "workspace discovery metadata exceeds bounds");
      memcpy(d->workspace, record->workspace, n + 1U);
      memcpy(d->name, record->name, m + 1U);
      memcpy(d->id, record->id, sizeof(d->id));
      d->found = d->candidate;
      d->found.record.workspace = d->workspace;
      d->found.record.name = d->name;
      d->found.record.id = d->id;
      memcpy(d->state.first, record->id, sizeof(d->state.first));
    } else if (strcmp(d->state.first, record->id) != 0) {
      memcpy(d->state.second, record->id, sizeof(d->state.second));
      d->state.phase = CAI_WORKSPACE_AMBIGUOUS;
    } else if (memcmp(d->found.record.instance, state->instance,
                      sizeof(state->instance)))
      d->unavailable = 1;
  }
  rc = notify(d, &d->candidate, NULL, error);
  cai_workspace_client_close(d->probe);
  d->probe = NULL;
  return rc;
}
static int next(cai_workspace_discovery *d, cai_error *error) {
  const cai_workspace_record *record = NULL;
  cai_workspace_client_config config;
  cai_error local;
  int rc;
  cai_error_init(&local);
  memset(&d->candidate, 0, sizeof(d->candidate));
  if (d->path && !d->direct_done) {
    d->direct_done = 1;
    d->candidate.record.workspace = cai_workspace_endpoint_directory(d->path);
    d->candidate.record.id = cai_workspace_endpoint_id(d->path);
    d->candidate.record.name = "";
    record = &d->candidate.record;
    rc = CAI_OK;
  } else if (d->path)
    rc = CAI_OK;
  else
    rc = cai_workspace_registry_next(d->registry, &record, &local);
  if (rc != CAI_OK) {
    if (rc == CAI_ERR_NOMEM || rc == CAI_ERR_TRANSPORT) {
      rc = cai_set_error_detail(error, rc, "read workspace discovery registry",
                                local.message ? local.message
                                              : "registry unavailable");
      cai_error_cleanup(&local);
      return rc;
    }
    ++d->state.errors;
    d->state.error_code = rc;
    rc = notify(d, NULL, &local, error);
  } else if (record == NULL) {
    d->state.phase = !d->selector[0]    ? CAI_WORKSPACE_LISTED
                     : d->unavailable   ? CAI_WORKSPACE_UNAVAILABLE
                     : d->state.matches ? CAI_WORKSPACE_FOUND
                                        : CAI_WORKSPACE_MISSING;
  } else if (!d->selector[0] || d->path || !strcmp(record->name, d->selector) ||
             !strcmp(record->id, d->selector)) {
    d->candidate.record = *record;
    cai_workspace_client_config_init(&config);
    config.runtime_directory = d->runtime;
    config.workspace = record->workspace;
    config.expected_instance = d->path ? NULL : record->instance;
    config.probe = 1;
    config.handshake_timeout_ms = d->config.probe_timeout_ms;
    rc = cai_workspace_client_open(&config, d->now, &d->probe, &local);
    if (rc == CAI_ERR_NOMEM)
      rc = cai_set_error(error, rc, "allocate workspace identity probe");
    else if (rc != CAI_OK)
      rc = unavailable(d, &local, error);
  }
  cai_error_cleanup(&local);
  return rc;
}
int cai_workspace_discovery_pump(cai_workspace_discovery *d, uint64_t now,
                                 cai_error *error) {
  cai_workspace_client_state state;
  cai_error local;
  int rc = valid(d, error);
  if (rc != CAI_OK)
    return rc;
  if (d->pumping || now < d->now)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery pump reentry or decreasing time");
  d->now = now;
  if (d->state.phase == CAI_WORKSPACE_DISCOVERY_FAILED)
    return cai_set_error(error, d->state.error_code,
                         "workspace discovery has failed");
  if (d->state.phase != CAI_WORKSPACE_DISCOVERING)
    return CAI_OK;
  d->pumping = 1;
  if (!d->probe)
    rc = next(d, error);
  else {
    cai_error_init(&local);
    rc = cai_workspace_client_pump(d->probe, now, &local);
    if (rc == CAI_OK)
      rc = cai_workspace_client_get_state(d->probe, &state, &local);
    if (rc == CAI_ERR_NOMEM)
      rc = cai_set_error(error, rc, "allocate workspace identity response");
    else if (rc != CAI_OK)
      rc = unavailable(d, &local, error);
    else if (state.phase == CAI_WORKSPACE_IDENTIFIED) {
      if (state.server_phase == CAI_ATTACHMENT_STOPPING) {
        cai_set_error(&local, CAI_ERR_LIMIT, "workspace owner is stopping");
        rc = unavailable(d, &local, error);
      } else
        rc = found(d, &state, error);
    }
    cai_error_cleanup(&local);
  }
  d->pumping = 0;
  return rc == CAI_OK ? rc : failed(d, rc, error);
}
int cai_workspace_discovery_get_state(cai_workspace_discovery *d,
                                      cai_workspace_discovery_state *out,
                                      cai_error *error) {
  int rc = valid(d, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "discovery state output required");
  *out = d->state;
  return CAI_OK;
}
const cai_workspace_discovery_result *
cai_workspace_discovery_get_result(const cai_workspace_discovery *d) {
  return d && pthread_equal(d->owner, pthread_self()) &&
                 d->state.phase == CAI_WORKSPACE_FOUND
             ? &d->found
             : NULL;
}
void cai_workspace_discovery_close(cai_workspace_discovery *d) {
  if (d && pthread_equal(d->owner, pthread_self()) && !d->pumping) {
    cai_workspace_client_close(d->probe);
    cai_workspace_registry_close(d->registry);
    cai_workspace_endpoint_close(d->path);
    cai_free_mem(NULL, d->runtime);
    cai_free_mem(NULL, d->selector);
    cai_free_mem(NULL, d);
  }
}
