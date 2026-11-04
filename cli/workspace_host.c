#define _POSIX_C_SOURCE 200809L
#include "workspace_host.h"
#include "identity.h"
#include "pouch_stream.h"
#include <cai/workspace_server.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

struct cai_cli_workspace_host {
  cai_workspace_server *server;
  cai_app_bridge *bridge;
  cai_input_controller_config admission;
};
static int clock_ms(uint64_t *out, cai_error *error) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    (void)cai_cli_error(error, CAI_ERR_TRANSPORT, "read workspace clock");
    return CAI_ERR_TRANSPORT;
  }
  *out = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
  return CAI_OK;
}
static int publish(cai_cli_workspace_host *host, cai_error *error) {
  cai_attachment_state snapshot;
  int rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK)
    cai_app_bridge_server(host->bridge, &snapshot);
  return rc;
}
static int authorize(void *context, uint64_t attachment, uint64_t generation,
                     cai_error *error) {
  cai_cli_workspace_host *host = context;
  int rc = cai_workspace_server_authorize(host->server, attachment, generation,
                                          error);
  if (rc == CAI_OK && host->admission.authorize != NULL)
    rc = host->admission.authorize(host->admission.authorize_context,
                                   attachment, generation, error);
  return rc;
}
static int message(void *context, const cai_workspace_server_event *event,
                   cai_error *error) {
  (void)context;
  if (event->type == CAI_WORKSPACE_MESSAGE)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "application socket actions are not connected");
  return CAI_OK;
}
int cai_cli_workspace_host_open(const char *runtime_directory,
                                const char *workspace,
                                const cai_attachment_config *lifetime,
                                const cai_input_controller_config *admission,
                                cai_app_bridge *bridge,
                                cai_cli_workspace_host **out,
                                cai_error *error) {
  cai_workspace_server_config config;
  cai_cli_workspace_host *host;
  int rc;
  *out = NULL;
  host = calloc(1U, sizeof(*host));
  if (host == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate workspace host");
  host->bridge = bridge;
  if (admission != NULL)
    host->admission = *admission;
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime_directory;
  config.workspace = workspace;
  config.lifetime = *lifetime;
  config.event = message;
  config.context = host;
  rc = cai_cli_private_directory(runtime_directory, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_open(&config, &host->server, error);
  if (rc == CAI_OK)
    rc = publish(host, error);
  if (rc != CAI_OK)
    cai_cli_workspace_host_close(host);
  else
    *out = host;
  return rc;
}
int cai_cli_workspace_host_ready(cai_cli_workspace_host *host,
                                 cai_error *error) {
  cai_attachment_state snapshot;
  uint64_t now;
  int rc;
  if (host == NULL)
    return CAI_OK;
  rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK && snapshot.phase == CAI_ATTACHMENT_STARTING) {
    rc = clock_ms(&now, error);
    if (rc == CAI_OK)
      rc = cai_workspace_server_ready(host->server, now, error);
  }
  return rc == CAI_OK ? publish(host, error) : rc;
}
int cai_cli_workspace_host_prepare(cai_cli_workspace_host *host,
                                   struct pollfd *fds, size_t capacity,
                                   size_t *count, long *timeout,
                                   cai_error *error) {
  uint64_t deadline, now, remaining;
  int rc;
  *count = 0U;
  if (host == NULL)
    return CAI_OK;
  rc = cai_workspace_server_prepare(host->server, fds, capacity, count,
                                    &deadline, error);
  if (rc != CAI_OK || deadline == 0U)
    return rc;
  rc = clock_ms(&now, error);
  if (rc != CAI_OK)
    return rc;
  remaining = deadline > now ? deadline - now : 0U;
  if (remaining > INT_MAX)
    remaining = INT_MAX;
  if (*timeout < 0L || (uint64_t)*timeout > remaining)
    *timeout = (long)remaining;
  return CAI_OK;
}
int cai_cli_workspace_host_pump(cai_cli_workspace_host *host, int *stopping,
                                cai_error *error) {
  cai_attachment_state snapshot;
  uint64_t now;
  int rc;
  *stopping = 0;
  if (host == NULL)
    return CAI_OK;
  rc = clock_ms(&now, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_pump(host->server, now, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc == CAI_OK) {
    cai_app_bridge_server(host->bridge, &snapshot);
    *stopping = snapshot.phase == CAI_ATTACHMENT_STOPPING;
  }
  return rc;
}
void cai_cli_workspace_host_admission(cai_cli_workspace_host *host,
                                      cai_input_controller_config *out) {
  out->authorize = authorize;
  out->authorize_context = host;
}
void cai_cli_workspace_host_stop(cai_cli_workspace_host *host) {
  if (host != NULL) {
    (void)cai_workspace_server_stop(host->server, NULL);
    (void)publish(host, NULL);
  }
}
void cai_cli_workspace_host_close(cai_cli_workspace_host *host) {
  if (host != NULL) {
    cai_workspace_server_close(host->server);
    free(host);
  }
}
