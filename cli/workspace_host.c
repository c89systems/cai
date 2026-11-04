#define _POSIX_C_SOURCE 200809L
#include "workspace_host.h"
#include "identity.h"
#include "input_wire.h"
#include "log.h"
#include "pouch_stream.h"
#include <cai/workspace_server.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct host_peer {
  cai_cli_workspace_host *host;
  uint64_t attachment;
  cai_cli_input_wire_decoder *decoder;
  cai_input_connection *input;
  unsigned long long binding_request, snapshot_request;
  int detached, binding;
} host_peer;
struct cai_cli_workspace_host {
  cai_workspace_server *server;
  cai_app_bridge *bridge;
  pslog_logger *logger;
  cai_input_controller_config admission;
  cai_input_controller *input;
  char session[129];
  uint64_t generation;
  host_peer peers[CAI_ATTACHMENT_LIMIT];
};
static int output_begin(void *context, const cai_protocol_header *header,
                        uint32_t size, cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_begin(peer->host->server, peer->attachment,
                                    header, size, error);
}
static int output_write(void *context, const cai_cbor_token *token,
                        cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_write(peer->host->server, peer->attachment, token,
                                    error);
}
static int output_end(void *context, cai_error *error) {
  host_peer *peer = context;
  return cai_workspace_server_end(peer->host->server, peer->attachment, error);
}
static int send_message(host_peer *peer, const cai_cli_input_wire_message *m,
                        cai_error *error) {
  cai_cli_input_wire_output output;
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  return cai_cli_input_wire_write(&output, m, error);
}
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
  cai_cli_workspace_host *host = context;
  host_peer *peer = NULL;
  const cai_cli_input_wire_message *m;
  size_t i;
  int rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (host->peers[i].attachment == event->attachment &&
        event->attachment != 0U)
      peer = &host->peers[i];
  if (event->type == CAI_WORKSPACE_ATTACHED) {
    if (peer != NULL)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "duplicate workspace attachment");
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
      if (host->peers[i].attachment == 0U) {
        peer = &host->peers[i];
        break;
      }
    if (peer == NULL)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "workspace input attachment limit reached");
    peer->host = host;
    peer->attachment = event->attachment;
    return CAI_OK;
  }
  if (event->type == CAI_WORKSPACE_DETACHED) {
    if (peer != NULL)
      peer->detached = 1;
    return CAI_OK;
  }
  if (event->type == CAI_WORKSPACE_PEER_ERROR && event->error != NULL)
    cai_cli_log_error(host->logger, "workspace attachment", event->error);
  if (event->type != CAI_WORKSPACE_MESSAGE)
    return CAI_OK;
  if (peer == NULL || peer->detached)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "workspace input attachment is unavailable");
  if (peer->decoder == NULL) {
    rc = cai_cli_input_wire_decoder_open(&peer->decoder, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_cli_input_wire_decode(peer->decoder, event->message, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  if (m->header.kind == CAI_PROTOCOL_SYNCHRONIZE) {
    unsigned long long epoch = 0U;
    cai_error binding;
    cai_error_init(&binding);
    rc = host->input != NULL
             ? cai_input_controller_conversation(host->input, &epoch, &binding)
             : cai_cli_error(&binding, CAI_ERR_INVALID,
                             "no active conversation");
    if (rc == CAI_OK &&
        (peer->input != NULL ||
         (m->header.conversation != 0U && m->header.conversation != epoch) ||
         (m->producer != 0U && (m->session == NULL || m->session[0] == '\0')) ||
         (m->session != NULL && m->session[0] != '\0' &&
          strcmp(m->session, host->session) != 0)))
      rc = cai_cli_error(&binding, CAI_ERR_INVALID,
                         "producer binding or conversation does not match");
    if (rc == CAI_OK)
      rc = m->producer != 0U
               ? cai_input_controller_resume(host->input, peer->attachment,
                                             epoch, m->producer, &peer->input,
                                             &binding)
               : cai_input_controller_connect(host->input, peer->attachment,
                                              &peer->input, &binding);
    if (rc == CAI_OK) {
      peer->binding_request = m->header.request;
      peer->binding = 1;
    } else {
      cai_cli_input_wire_message reply;
      memset(&reply, 0, sizeof(reply));
      reply.header.kind = CAI_PROTOCOL_RESULT;
      reply.header.request = m->header.request;
      reply.header.conversation = m->header.conversation;
      reply.category = CAI_CLI_INPUT_BIND_REJECTED;
      reply.observation.result.outcome = CAI_INPUT_REJECTED;
      reply.observation.result.code = rc;
      snprintf(reply.observation.result.reason,
               sizeof(reply.observation.result.reason), "%s",
               binding.message != NULL ? binding.message
                                       : "input binding failed");
      rc = send_message(peer, &reply, error);
    }
    cai_error_cleanup(&binding);
    return rc;
  }
  if (peer->input == NULL || peer->binding)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "bind the input producer before submitting work");
  {
    cai_input_scope scope;
    scope.conversation = m->header.conversation;
    scope.generation = m->header.generation;
    rc = cai_input_connection_request(peer->input, &scope, &m->request, error);
    return rc;
  }
}
static int release_peer(host_peer *peer, cai_error *error) {
  int rc = peer->host != NULL && peer->host->input != NULL
               ? cai_input_controller_release_edits(peer->host->input,
                                                    peer->attachment, error)
               : CAI_OK;
  if (peer->input != NULL) {
    cai_input_connection_close(peer->input);
    peer->input = NULL;
  }
  return rc;
}
static int output_one(host_peer *peer, cai_error *error) {
  cai_input_observation *o;
  cai_cli_input_wire_message m;
  int rc;
  if (peer->input == NULL)
    return CAI_OK;
  rc = cai_input_connection_poll(peer->input, &o, error);
  if (rc != CAI_OK || o == NULL)
    return rc;
  memset(&m, 0, sizeof(m));
  m.observation = *o;
  m.header.conversation = o->conversation;
  m.header.sequence = o->state.view_revision;
  switch (o->type) {
  case CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN:
    m.header.kind = CAI_PROTOCOL_QUEUE_BEGIN;
    m.session = peer->host->session;
    peer->snapshot_request = peer->binding ? peer->binding_request : 0U;
    m.header.request = peer->snapshot_request;
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM:
    m.header.kind = CAI_PROTOCOL_QUEUE_ITEM;
    m.header.request = peer->snapshot_request;
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_END:
    m.header.kind = CAI_PROTOCOL_QUEUE_END;
    m.header.request = peer->snapshot_request;
    peer->binding = 0;
    break;
  case CAI_INPUT_OBSERVATION_CHANGE:
    m.header.kind = CAI_PROTOCOL_QUEUE_CHANGE;
    break;
  case CAI_INPUT_OBSERVATION_RESULT:
    m.header.kind = CAI_PROTOCOL_RESULT;
    m.header.sequence = 0U;
    m.header.request = o->result.request_id;
    break;
  case CAI_INPUT_OBSERVATION_DISCONNECTED:
    m.header.kind = CAI_PROTOCOL_RESULT;
    m.header.sequence = 0U;
    m.category = peer->binding && o->result.code != CAI_OK
                     ? CAI_CLI_INPUT_BIND_REJECTED
                     : CAI_CLI_INPUT_RESET;
    m.header.request =
        m.category == CAI_CLI_INPUT_BIND_REJECTED ? peer->binding_request : 0U;
    if (o->result.code != CAI_OK) {
      cai_input_connection_close(peer->input);
      peer->input = NULL;
      peer->binding = 0;
    }
    break;
  }
  rc = send_message(peer, &m, error);
  cai_input_observation_close(o);
  return rc;
}
int cai_cli_workspace_host_input(cai_cli_workspace_host *host,
                                 cai_input_controller *input,
                                 const char *session, cai_error *error) {
  size_t i;
  int result = CAI_OK;
  if (host == NULL)
    return CAI_OK;
  if (input != NULL) {
    if (host->input != NULL || session == NULL || strlen(session) > 128U)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "workspace input binding is invalid");
    host->input = input;
    strcpy(host->session, session);
    return CAI_OK;
  }
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    cai_error local;
    int rc;
    cai_error_init(&local);
    if (peer->input != NULL && !peer->detached) {
      cai_cli_input_wire_message reset;
      memset(&reset, 0, sizeof(reset));
      reset.header.kind = CAI_PROTOCOL_RESULT;
      reset.category = CAI_CLI_INPUT_RESET;
      (void)send_message(peer, &reset, NULL);
    }
    rc = release_peer(peer, &local);
    if (rc != CAI_OK && result == CAI_OK) {
      result = rc;
      cai_cli_error(error, rc,
                    local.message != NULL ? local.message
                                          : "release workspace editor");
    }
    cai_error_cleanup(&local);
  }
  host->input = NULL;
  host->session[0] = '\0';
  return result;
}
int cai_cli_workspace_host_open(const char *runtime_directory,
                                const char *workspace,
                                const cai_attachment_config *lifetime,
                                const cai_input_controller_config *admission,
                                cai_app_bridge *bridge, pslog_logger *logger,
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
  host->logger = logger;
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
  size_t i;
  int rc;
  *count = 0U;
  if (host == NULL)
    return CAI_OK;
  if (capacity < CAI_CLI_WORKSPACE_HOST_FDS)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "workspace poll capacity is insufficient");
  rc = cai_workspace_server_prepare(host->server, fds, CAI_WORKSPACE_SERVER_FDS,
                                    count, &deadline, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    size_t pending;
    if (peer->input != NULL && !peer->detached &&
        cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                     NULL) == CAI_OK &&
        pending <= 65536U) {
      fds[*count].fd = cai_input_connection_fd(peer->input);
      fds[(*count)++].events = POLLIN;
    }
    if (peer->detached)
      *timeout = 0L;
  }
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
  size_t i;
  *stopping = 0;
  if (host == NULL)
    return CAI_OK;
  rc = clock_ms(&now, error);
  if (rc == CAI_OK)
    rc = cai_workspace_server_pump(host->server, now, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_workspace_server_get_state(host->server, &snapshot, error);
  if (rc != CAI_OK)
    return rc;
  if (host->input != NULL && host->generation != 0U &&
      host->generation != snapshot.generation)
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
      if (host->peers[i].attachment != 0U) {
        rc = cai_input_controller_release_edits(
            host->input, host->peers[i].attachment, error);
        if (rc != CAI_OK)
          return rc;
      }
  host->generation = snapshot.generation;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    host_peer *peer = &host->peers[i];
    size_t pending;
    cai_error local;
    if (peer->attachment == 0U)
      continue;
    if (peer->detached) {
      rc = release_peer(peer, error);
      cai_cli_input_wire_decoder_close(peer->decoder);
      memset(peer, 0, sizeof(*peer));
      if (rc != CAI_OK)
        return rc;
      continue;
    }
    cai_error_init(&local);
    rc = cai_workspace_server_pending(host->server, peer->attachment, &pending,
                                      &local);
    if (rc == CAI_OK && pending <= 65536U)
      rc = output_one(peer, &local);
    if (rc != CAI_OK) {
      cai_cli_log_error(host->logger, "workspace input output", &local);
      (void)cai_workspace_server_disconnect(host->server, peer->attachment,
                                            NULL);
    }
    cai_error_cleanup(&local);
  }
  rc = CAI_OK;
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
  size_t i;
  if (host != NULL) {
    for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
      cai_input_connection_close(host->peers[i].input);
      cai_cli_input_wire_decoder_close(host->peers[i].decoder);
    }
    cai_workspace_server_close(host->server);
    free(host);
  }
}
