#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include <cai/workspace.h>
#include <cai/workspace_server.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define ROLE_NOTICES 16U
typedef struct role_notice {
  uint64_t generation, controller, request;
} role_notice;
typedef struct server_peer {
  struct cai_workspace_server *server;
  cai_wire_peer *wire;
  cai_protocol_decoder *protocol;
  uint64_t attachment, deadline;
  size_t part, offset;
  role_notice notices[ROLE_NOTICES];
  size_t notice_head, notice_count;
  int greeted, detach, failed, writing;
} server_peer;
struct cai_workspace_server {
  cai_workspace_server_config config;
  cai_workspace_endpoint *endpoint;
  cai_attachment_controller *attachments;
  pthread_t owner;
  server_peer peers[CAI_ATTACHMENT_LIMIT];
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  uint64_t now;
  int pumping;
};
static int valid(cai_workspace_server *server, cai_error *error) {
  return server != NULL && pthread_equal(server->owner, pthread_self())
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_INVALID,
                             "workspace server requires its owner thread");
}
static int notify(server_peer *peer, cai_workspace_server_event_type type,
                  const cai_protocol_event *message, const cai_error *cause,
                  cai_error *error) {
  cai_workspace_server_event event;
  if (peer->server->config.event == NULL)
    return CAI_OK;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.attachment = peer->attachment;
  event.message = message;
  event.error = cause;
  return peer->server->config.event(peer->server->config.context, &event,
                                    error);
}
static server_peer *find(cai_workspace_server *server, uint64_t id) {
  size_t i;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (id != 0U && server->peers[i].attachment == id)
      return &server->peers[i];
  return NULL;
}
static void failed(server_peer *peer, int rc, const cai_error *cause) {
  cai_error fallback;
  if (peer->failed)
    return;
  peer->failed = rc;
  cai_error_init(&fallback);
  if (cause == NULL) {
    cai_set_error(&fallback, rc, "workspace subscriber output failed");
    cause = &fallback;
  }
  (void)notify(peer, CAI_WORKSPACE_PEER_ERROR, NULL, cause, NULL);
  cai_error_cleanup(&fallback);
}
static uint32_t string_size(size_t n) {
  return (uint32_t)n + (n < 24U ? 1U : n <= 255U ? 2U : n <= 65535U ? 3U : 5U);
}
static uint32_t integer_size(uint64_t n) {
  return n < 24U           ? 1U
         : n <= UINT8_MAX  ? 2U
         : n <= UINT16_MAX ? 3U
         : n <= UINT32_MAX ? 5U
                           : 9U;
}
static int write_token(server_peer *peer, cai_cbor_type type, uint64_t argument,
                       const void *data, size_t length, cai_error *error) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = data;
  token.length = length;
  return cai_wire_peer_write(peer->wire, &token, error);
}
static int write_bytes(server_peer *peer, const void *data, size_t count,
                       cai_error *error) {
  int rc = write_token(peer, CAI_CBOR_BYTES, count, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_DATA, 0U, data, count, error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
  return rc;
}
static int welcome(server_peer *peer, cai_error *error) {
  cai_workspace_server *server = peer->server;
  cai_attachment_state state;
  cai_protocol_header header;
  const char *directory = cai_workspace_server_directory(server);
  size_t length = strlen(directory);
  int rc =
      cai_attachment_controller_get_state(server->attachments, &state, error);
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_WELCOME;
  header.attachment = peer->attachment;
  header.generation = state.generation;
  if (rc == CAI_OK)
    rc = cai_protocol_peer_begin(peer->wire, &header,
                                 2U + string_size(length) +
                                     string_size(sizeof(server->instance)),
                                 error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_ARRAY, 3U, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = write_bytes(peer, directory, length, error);
  if (rc == CAI_OK)
    rc = write_bytes(peer, server->instance, sizeof(server->instance), error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_BOOL, state.persist != 0, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = cai_protocol_peer_end(peer->wire, error);
  return rc;
}
static int send_role(server_peer *peer, uint64_t generation,
                     uint64_t controller, uint64_t request, cai_error *error) {
  cai_protocol_header header;
  int rc;
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_ROLE;
  header.attachment = peer->attachment;
  header.generation = generation;
  header.request = request;
  rc = cai_protocol_peer_begin(peer->wire, &header, integer_size(controller),
                               error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_UINT, controller, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = cai_protocol_peer_end(peer->wire, error);
  return rc;
}
static int defer_role(server_peer *peer, uint64_t generation,
                      uint64_t controller, uint64_t request, cai_error *error) {
  role_notice *notice;
  size_t slot = (peer->notice_head + peer->notice_count) % ROLE_NOTICES;
  if (request == 0U && peer->notice_count != 0U &&
      peer->notices[(slot + ROLE_NOTICES - 1U) % ROLE_NOTICES].request == 0U)
    slot = (slot + ROLE_NOTICES - 1U) % ROLE_NOTICES;
  else {
    if (peer->notice_count == ROLE_NOTICES)
      return cai_set_error(error, CAI_ERR_LIMIT,
                           "workspace subscriber control backlog exceeded");
    ++peer->notice_count;
  }
  notice = &peer->notices[slot];
  notice->generation = generation;
  notice->controller = controller;
  notice->request = request;
  return CAI_OK;
}
static int drain_roles(server_peer *peer, cai_error *error) {
  while (peer->notice_count != 0U) {
    const role_notice *notice = &peer->notices[peer->notice_head];
    int rc = send_role(peer, notice->generation, notice->controller,
                       notice->request, error);
    if (rc != CAI_OK)
      return rc;
    peer->notice_head = (peer->notice_head + 1U) % ROLE_NOTICES;
    --peer->notice_count;
  }
  return CAI_OK;
}
static int roles(cai_workspace_server *server, uint64_t requester,
                 uint64_t request, cai_error *error) {
  cai_attachment_state state;
  size_t i;
  int rc =
      cai_attachment_controller_get_state(server->attachments, &state, error);
  if (rc != CAI_OK)
    return rc;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    server_peer *peer = &server->peers[i];
    cai_error local;
    int sent;
    if (peer->attachment == 0U || peer->failed || peer->detach)
      continue;
    cai_error_init(&local);
    sent =
        peer->writing
            ? defer_role(peer, state.generation, state.controller,
                         peer->attachment == requester ? request : 0U, &local)
            : send_role(peer, state.generation, state.controller,
                        peer->attachment == requester ? request : 0U, &local);
    if (sent != CAI_OK)
      failed(peer, sent, &local);
    cai_error_cleanup(&local);
  }
  return CAI_OK;
}
static int greeting(server_peer *peer, const cai_protocol_event *event,
                    cai_error *error) {
  const cai_cbor_token *token = event->token;
  const char *expected;
  size_t length;
  if (event->type == CAI_PROTOCOL_HEADER) {
    const cai_protocol_header *h = event->header;
    if (peer->greeted || h->kind != CAI_PROTOCOL_HELLO || h->request != 0U ||
        h->conversation != 0U || h->generation != 0U || h->sequence != 0U ||
        h->stream != 0U || h->attachment != 0U)
      goto invalid;
    peer->part = peer->offset = 0U;
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (peer->part != 6U)
      goto invalid;
    peer->greeted = 1;
    peer->deadline = 0U;
    return CAI_OK;
  }
  if (token->indefinite)
    goto invalid;
  switch (peer->part) {
  case 0:
    if (token->type != CAI_CBOR_ARRAY || token->argument != 2U)
      goto invalid;
    peer->part = 1U;
    break;
  case 1:
  case 3:
    expected = peer->part == 1U ? cai_workspace_server_directory(peer->server)
                                : CAI_WORKSPACE_SCHEMA;
    length = strlen(expected);
    if (token->type != (peer->part == 1U ? CAI_CBOR_BYTES : CAI_CBOR_TEXT) ||
        token->argument != length)
      goto invalid;
    peer->offset = 0U;
    ++peer->part;
    break;
  case 2:
  case 4:
    expected = peer->part == 2U ? cai_workspace_server_directory(peer->server)
                                : CAI_WORKSPACE_SCHEMA;
    length = strlen(expected);
    if (token->type == CAI_CBOR_DATA) {
      if (token->length > length - peer->offset ||
          memcmp(token->data, expected + peer->offset, token->length) != 0)
        goto invalid;
      peer->offset += token->length;
    } else if (token->type == CAI_CBOR_END && peer->offset == length &&
               token->argument ==
                   (peer->part == 2U ? CAI_CBOR_BYTES : CAI_CBOR_TEXT))
      ++peer->part;
    else
      goto invalid;
    break;
  case 5:
    if (token->type != CAI_CBOR_END || token->argument != CAI_CBOR_ARRAY)
      goto invalid;
    peer->part = 6U;
    break;
  default:
    goto invalid;
  }
  return CAI_OK;
invalid:
  return cai_set_error(error, CAI_ERR_PROTOCOL,
                       "workspace greeting identity or schema mismatch");
}
static int receive(void *context, const cai_protocol_event *event,
                   cai_error *error) {
  server_peer *peer = context;
  cai_workspace_server *server = peer->server;
  const cai_protocol_header *header = event->header;
  int control;
  if (peer->detach)
    return cai_set_error(error, CAI_ERR_PROTOCOL, "request after detach");
  if (peer->attachment == 0U)
    return greeting(peer, event, error);
  if (event->type == CAI_PROTOCOL_HEADER) {
    cai_attachment_state state;
    int rc =
        cai_attachment_controller_get_state(server->attachments, &state, error);
    if (rc != CAI_OK)
      return rc;
    if (state.phase == CAI_ATTACHMENT_STOPPING)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "workspace server is stopping");
  }
  if (header->attachment != peer->attachment)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "request attachment does not match its connection");
  control = header->kind == CAI_PROTOCOL_DETACH ||
            header->kind == CAI_PROTOCOL_TAKEOVER ||
            header->kind == CAI_PROTOCOL_SHUTDOWN;
  if (control) {
    if (header->conversation != 0U || header->sequence != 0U ||
        header->stream != 0U)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "invalid workspace control metadata");
    if (event->type == CAI_PROTOCOL_PAYLOAD &&
        event->token->type != CAI_CBOR_NULL)
      return cai_set_error(error, CAI_ERR_PROTOCOL,
                           "workspace controls require a null payload");
    if (event->type == CAI_PROTOCOL_COMPLETE) {
      if (header->kind == CAI_PROTOCOL_DETACH)
        peer->detach = 1;
      else if (header->kind == CAI_PROTOCOL_TAKEOVER) {
        uint64_t generation;
        cai_attachment_state state;
        int rc = cai_attachment_controller_get_state(server->attachments,
                                                     &state, error);
        if (rc == CAI_OK && header->generation != state.generation)
          rc = cai_set_error(
              error, CAI_ERR_INVALID,
              "takeover generation is stale; synchronize before retrying");
        if (rc == CAI_OK)
          rc = cai_attachment_controller_takeover(
              server->attachments, peer->attachment, &generation, error);
        if (rc == CAI_OK)
          rc = roles(server, peer->attachment, header->request, error);
        return rc;
      } else {
        int rc = cai_workspace_server_authorize(server, peer->attachment,
                                                header->generation, error);
        if (rc == CAI_OK)
          rc = cai_workspace_server_stop(server, error);
        return rc;
      }
    }
    return CAI_OK;
  }
  if (header->kind != CAI_PROTOCOL_REQUEST &&
      header->kind != CAI_PROTOCOL_SYNCHRONIZE)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "unsupported client message kind");
  return notify(peer, CAI_WORKSPACE_MESSAGE, event, NULL, error);
}
void cai_workspace_server_config_init(cai_workspace_server_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
int cai_workspace_server_open(const cai_workspace_server_config *config,
                              cai_workspace_server **out, cai_error *error) {
  cai_workspace_server *server;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || out == NULL ||
      config->backlog_limit > 16U * 1024U * 1024U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid workspace server configuration");
  server = cai_alloc(NULL, sizeof(*server));
  if (server == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate workspace server");
  memset(server, 0, sizeof(*server));
  server->config = *config;
  server->config.runtime_directory = server->config.workspace = NULL;
  server->owner = pthread_self();
  if (server->config.handshake_timeout_ms == 0U)
    server->config.handshake_timeout_ms = 10000U;
  rc = RAND_bytes(server->instance, sizeof(server->instance)) == 1
           ? CAI_OK
           : cai_set_error(error, CAI_ERR_TRANSPORT,
                           "generate workspace server instance identity");
  if (rc == CAI_OK)
    rc = cai_workspace_endpoint_open(
        config->runtime_directory, config->workspace, &server->endpoint, error);
  if (rc == CAI_OK)
    rc = cai_attachment_controller_open(&config->lifetime, &server->attachments,
                                        error);
  if (rc == CAI_OK)
    rc = cai_workspace_endpoint_listen(server->endpoint, error);
  if (rc == CAI_OK)
    *out = server;
  else
    cai_workspace_server_close(server);
  return rc;
}
static int advance(cai_workspace_server *server, uint64_t now,
                   cai_error *error) {
  if (now < server->now)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace server time must not decrease");
  server->now = now;
  return cai_attachment_controller_advance(server->attachments, now, error);
}
int cai_workspace_server_ready(cai_workspace_server *server, uint64_t now,
                               cai_error *error) {
  int rc = valid(server, error);
  if (rc == CAI_OK)
    rc = advance(server, now, error);
  return rc == CAI_OK
             ? cai_attachment_controller_ready(server->attachments, now, error)
             : rc;
}
int cai_workspace_server_prepare(cai_workspace_server *server,
                                 struct pollfd *fds, size_t capacity,
                                 size_t *count, uint64_t *deadline,
                                 cai_error *error) {
  cai_attachment_state state;
  size_t i, n = 0U;
  int available = 0;
  int rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  if (fds == NULL || capacity < CAI_WORKSPACE_SERVER_FDS || count == NULL ||
      deadline == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace poll outputs have insufficient capacity");
  rc = cai_attachment_controller_get_state(server->attachments, &state, error);
  if (rc != CAI_OK)
    return rc;
  memset(fds, 0, CAI_WORKSPACE_SERVER_FDS * sizeof(*fds));
  *deadline = state.deadline_ms;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (server->peers[i].wire == NULL)
      available = 1;
  if (available && state.phase != CAI_ATTACHMENT_STOPPING) {
    fds[n].fd = cai_workspace_endpoint_fd(server->endpoint);
    fds[n++].events = POLLIN;
  }
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    server_peer *peer = &server->peers[i];
    if (peer->wire == NULL)
      continue;
    fds[n].fd = cai_wire_peer_fd(peer->wire);
    fds[n].events = POLLIN;
    if (cai_wire_peer_pending(peer->wire) != 0U)
      fds[n].events |= POLLOUT;
    ++n;
    if (peer->deadline != 0U && (*deadline == 0U || peer->deadline < *deadline))
      *deadline = peer->deadline;
  }
  *count = n;
  return CAI_OK;
}
static void drop(server_peer *peer, int announce) {
  cai_workspace_server *server = peer->server;
  if (peer->attachment != 0U) {
    (void)cai_attachment_controller_detach(server->attachments,
                                           peer->attachment, NULL);
    if (announce)
      (void)notify(peer, CAI_WORKSPACE_DETACHED, NULL, NULL, NULL);
  }
  cai_wire_peer_close(peer->wire);
  cai_protocol_decoder_close(peer->protocol);
  memset(peer, 0, sizeof(*peer));
}
int cai_workspace_server_pump(cai_workspace_server *server, uint64_t now,
                              cai_error *error) {
  cai_attachment_state state;
  size_t i;
  int changed = 0, rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  if (server->pumping)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace pump is not reentrant");
  rc = advance(server, now, error);
  if (rc != CAI_OK)
    return rc;
  server->pumping = 1;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i) {
    server_peer *peer = &server->peers[i];
    cai_error local;
    int eof = 0;
    if (peer->wire == NULL)
      continue;
    cai_error_init(&local);
    rc = peer->failed;
    if (rc == CAI_OK && peer->deadline != 0U && now >= peer->deadline)
      rc = cai_set_error(&local, CAI_ERR_LIMIT, "workspace greeting timed out");
    if (rc == CAI_OK)
      rc = cai_wire_peer_read(peer->wire, 0U, &eof, &local);
    (void)cai_attachment_controller_get_state(server->attachments, &state,
                                              NULL);
    if (rc == CAI_OK && !eof && !peer->detach && peer->greeted &&
        peer->attachment == 0U && state.phase == CAI_ATTACHMENT_READY) {
      rc = cai_attachment_controller_attach(server->attachments, now,
                                            &peer->attachment, &local);
      if (rc == CAI_OK) {
        changed = 1;
        rc = welcome(peer, &local);
      }
      if (rc == CAI_OK)
        rc = notify(peer, CAI_WORKSPACE_ATTACHED, NULL, NULL, &local);
    }
    if (rc == CAI_OK && !eof && !peer->detach)
      rc = cai_wire_peer_flush(peer->wire, 0U, &local);
    if (rc != CAI_OK && local.code == CAI_OK)
      cai_set_error(&local, rc, "workspace subscriber processing failed");
    if (rc != CAI_OK && !peer->failed)
      (void)notify(peer, CAI_WORKSPACE_PEER_ERROR, NULL, &local, NULL);
    if (rc != CAI_OK || eof || peer->detach) {
      changed |= peer->attachment != 0U;
      drop(peer, 1);
    }
    cai_error_cleanup(&local);
  }
  (void)cai_attachment_controller_get_state(server->attachments, &state, NULL);
  for (i = 0U;
       state.phase != CAI_ATTACHMENT_STOPPING && i < CAI_ATTACHMENT_LIMIT;
       ++i) {
    server_peer *peer = &server->peers[i];
    cai_error local;
    int descriptor = -1;
    if (peer->wire != NULL)
      continue;
    cai_error_init(&local);
    rc = cai_workspace_endpoint_accept(server->endpoint, &descriptor, &local);
    if (rc == CAI_OK && descriptor >= 0) {
      peer->server = server;
      peer->deadline = server->config.handshake_timeout_ms > UINT64_MAX - now
                           ? UINT64_MAX
                           : now + server->config.handshake_timeout_ms;
      rc = cai_protocol_decoder_open(receive, peer, &peer->protocol, &local);
      if (rc == CAI_OK)
        rc = cai_wire_peer_open(descriptor, server->config.backlog_limit,
                                cai_protocol_decoder_event, peer->protocol,
                                &peer->wire, &local);
      if (rc != CAI_OK) {
        close(descriptor);
        (void)notify(peer, CAI_WORKSPACE_PEER_ERROR, NULL, &local, NULL);
        drop(peer, 0);
      }
    }
    if (rc != CAI_OK && descriptor < 0) {
      peer->server = server;
      (void)notify(peer, CAI_WORKSPACE_PEER_ERROR, NULL, &local, NULL);
      peer->server = NULL;
    }
    cai_error_cleanup(&local);
    if (rc != CAI_OK || descriptor < 0)
      break;
  }
  if (changed)
    (void)roles(server, 0U, 0U, NULL);
  server->pumping = 0;
  return CAI_OK;
}
int cai_workspace_server_get_state(cai_workspace_server *server,
                                   cai_attachment_state *out,
                                   cai_error *error) {
  int rc = valid(server, error);
  return rc == CAI_OK ? cai_attachment_controller_get_state(server->attachments,
                                                            out, error)
                      : rc;
}
int cai_workspace_server_authorize(cai_workspace_server *server,
                                   uint64_t attachment, uint64_t generation,
                                   cai_error *error) {
  int rc = valid(server, error);
  return rc == CAI_OK ? cai_attachment_controller_authorize(
                            server->attachments, attachment, generation, error)
                      : rc;
}
int cai_workspace_server_stop(cai_workspace_server *server, cai_error *error) {
  int rc = valid(server, error);
  return rc == CAI_OK
             ? cai_attachment_controller_stop(server->attachments, error)
             : rc;
}
int cai_workspace_server_pending(cai_workspace_server *server,
                                 uint64_t attachment, size_t *out,
                                 cai_error *error) {
  server_peer *peer;
  int rc = valid(server, error);
  if (out != NULL)
    *out = 0U;
  if (rc != CAI_OK)
    return rc;
  peer = find(server, attachment);
  if (peer == NULL || peer->failed || peer->detach || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "attachment output is unavailable");
  *out = cai_wire_peer_pending(peer->wire);
  return CAI_OK;
}
int cai_workspace_server_disconnect(cai_workspace_server *server,
                                    uint64_t attachment, cai_error *error) {
  server_peer *peer;
  int rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  peer = find(server, attachment);
  if (server->pumping || peer == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "detach requires a live attachment outside pump");
  drop(peer, 1);
  return roles(server, 0U, 0U, error);
}
int cai_workspace_server_begin(cai_workspace_server *server,
                               uint64_t attachment,
                               const cai_protocol_header *header,
                               uint32_t length, cai_error *error) {
  cai_attachment_state state;
  cai_protocol_header copy;
  server_peer *peer;
  int rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  peer = find(server, attachment);
  if (peer == NULL || peer->failed || peer->detach || peer->writing ||
      header == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "attachment is not writable");
  rc = cai_attachment_controller_get_state(server->attachments, &state, error);
  if (rc == CAI_OK) {
    copy = *header;
    copy.attachment = attachment;
    copy.generation = state.generation;
    rc = cai_protocol_peer_begin(peer->wire, &copy, length, error);
    if (rc == CAI_OK)
      peer->writing = 1;
    if (rc != CAI_OK)
      failed(peer, rc, error);
  }
  return rc;
}
int cai_workspace_server_write(cai_workspace_server *server,
                               uint64_t attachment, const cai_cbor_token *token,
                               cai_error *error) {
  server_peer *peer;
  int rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  peer = find(server, attachment);
  if (peer == NULL || peer->failed || peer->detach || !peer->writing)
    return cai_set_error(error, CAI_ERR_INVALID, "attachment is not writable");
  rc = cai_wire_peer_write(peer->wire, token, error);
  if (rc != CAI_OK)
    failed(peer, rc, error);
  return rc;
}
int cai_workspace_server_end(cai_workspace_server *server, uint64_t attachment,
                             cai_error *error) {
  server_peer *peer;
  int rc = valid(server, error);
  if (rc != CAI_OK)
    return rc;
  peer = find(server, attachment);
  if (peer == NULL || peer->failed || peer->detach || !peer->writing)
    return cai_set_error(error, CAI_ERR_INVALID, "attachment is not writable");
  rc = cai_protocol_peer_end(peer->wire, error);
  if (rc == CAI_OK) {
    peer->writing = 0;
    rc = drain_roles(peer, error);
  }
  if (rc != CAI_OK)
    failed(peer, rc, error);
  return rc;
}
const char *cai_workspace_server_directory(const cai_workspace_server *server) {
  return server != NULL ? cai_workspace_endpoint_directory(server->endpoint)
                        : NULL;
}
const unsigned char *
cai_workspace_server_instance(const cai_workspace_server *server) {
  return server != NULL ? server->instance : NULL;
}
void cai_workspace_server_close(cai_workspace_server *server) {
  size_t i;
  if (server == NULL || valid(server, NULL) != CAI_OK || server->pumping)
    return;
  for (i = 0U; i < CAI_ATTACHMENT_LIMIT; ++i)
    if (server->peers[i].wire != NULL)
      drop(&server->peers[i], 0);
  cai_attachment_controller_close(server->attachments);
  cai_workspace_endpoint_close(server->endpoint);
  cai_free_mem(NULL, server);
}
