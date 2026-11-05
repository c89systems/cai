#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include "cai_workspace_claims.h"
#include <cai/workspace.h>
#include <cai/workspace_client.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** Owner-thread attachment transport with verified instance and role state.
 * Owns its socket/codecs; never opens agent sessions or writable stores. */
struct cai_workspace_client {
  cai_workspace_endpoint *endpoint;
  cai_wire_peer *wire;
  cai_protocol_decoder *protocol;
  cai_workspace_client_config config;
  cai_workspace_client_state state;
  cai_workspace_claim *requirements;
  char rejection[CAI_WORKSPACE_CLAIM_NAME + 1U];
  size_t rejection_length;
  int rejecting;
  pthread_t owner;
  uint64_t now, role, role_generation;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  unsigned char expected[CAI_WORKSPACE_INSTANCE_BYTES];
  size_t part, offset;
  int fd, pumping, writing, persistence;
  int expecting;
  cai_attachment_phase server_phase;
};
/** Require the opening thread for connection state and codec operations. */
static int valid(cai_workspace_client *client, cai_error *error) {
  return client != NULL && pthread_equal(client->owner, pthread_self())
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_INVALID,
                             "workspace client requires its owner thread");
}
/** Release socket/codecs after an active pump unwinds, retaining last state. */
static void release_transport(cai_workspace_client *client) {
  if (client->pumping)
    return;
  cai_wire_peer_close(client->wire);
  cai_protocol_decoder_close(client->protocol);
  client->wire = NULL;
  client->protocol = NULL;
  client->writing = 0;
  if (client->fd >= 0)
    close(client->fd);
  client->fd = -1;
}
/** Make connection failure terminal and discard its unaccepted transport data.
 */
static int failed(cai_workspace_client *client, int rc) {
  client->state.phase = CAI_WORKSPACE_FAILED;
  client->state.error_code = rc;
  client->state.deadline_ms = 0U;
  release_transport(client);
  return rc;
}
static int emit(cai_workspace_client *client, const cai_protocol_event *event,
                cai_error *error) {
  return client->config.event != NULL
             ? client->config.event(client->config.context, event, error)
             : CAI_OK;
}
static int invalid(cai_error *error) {
  return cai_set_error(error, CAI_ERR_PROTOCOL,
                       "workspace server identity, role or message is invalid");
}
static int identity(cai_workspace_client *client,
                    const cai_protocol_event *event, cai_error *error) {
  const cai_protocol_header *h = event->header;
  const cai_cbor_token *value = event->token;
  const char *expected = client->part == 3U || client->part == 4U
                             ? CAI_WORKSPACE_SCHEMA
                             : cai_workspace_client_directory(client);
  size_t length = client->part == 5U || client->part == 6U
                      ? sizeof(client->instance)
                      : strlen(expected);
  cai_cbor_type type =
      client->part == 3U || client->part == 4U ? CAI_CBOR_TEXT : CAI_CBOR_BYTES;
  if (event->type == CAI_PROTOCOL_HEADER) {
    if (client->state.has_identity || h->kind != CAI_PROTOCOL_IDENTITY ||
        h->request != 0U || h->conversation != 0U || h->generation != 0U ||
        h->sequence != 0U || h->stream != 0U || h->attachment != 0U)
      return invalid(error);
    client->part = client->offset = 0U;
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (client->part != 10U)
      return invalid(error);
    if (client->expecting && memcmp(client->instance, client->expected,
                                    sizeof(client->instance)) != 0)
      return invalid(error);
    memcpy(client->state.instance, client->instance, sizeof(client->instance));
    client->state.persist = client->persistence;
    client->state.server_phase = client->server_phase;
    client->state.has_identity = 1;
    client->state.phase = CAI_WORKSPACE_IDENTIFIED;
    client->state.deadline_ms = 0U;
    return emit(client, event, error);
  }
  if (value->indefinite)
    return invalid(error);
  switch (client->part) {
  case 0:
    if (value->type != CAI_CBOR_ARRAY || value->argument != 5U)
      return invalid(error);
    ++client->part;
    break;
  case 1:
  case 3:
  case 5:
    if (value->type != type || value->argument != length)
      return invalid(error);
    client->offset = 0U;
    ++client->part;
    break;
  case 2:
  case 4:
  case 6:
    if (value->type == CAI_CBOR_DATA) {
      if (value->length > length - client->offset)
        return invalid(error);
      if (client->part == 6U)
        memcpy(client->instance + client->offset, value->data, value->length);
      else if (memcmp(value->data, expected + client->offset, value->length))
        return invalid(error);
      client->offset += value->length;
    } else if (value->type == CAI_CBOR_END && value->argument == type &&
               client->offset == length)
      ++client->part;
    else
      return invalid(error);
    break;
  case 7:
    if (value->type != CAI_CBOR_BOOL)
      return invalid(error);
    client->persistence = value->argument != 0U;
    ++client->part;
    break;
  case 8:
    if (value->type != CAI_CBOR_UINT ||
        value->argument > CAI_ATTACHMENT_STOPPING)
      return invalid(error);
    client->server_phase = (cai_attachment_phase)value->argument;
    ++client->part;
    break;
  case 9:
    if (value->type != CAI_CBOR_END || value->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    ++client->part;
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
static int welcome(cai_workspace_client *client,
                   const cai_protocol_event *event, cai_error *error) {
  const cai_protocol_header *h = event->header;
  const cai_cbor_token *token = event->token;
  const char *directory = cai_workspace_client_directory(client);
  size_t length = strlen(directory);
  if (event->type == CAI_PROTOCOL_HEADER) {
    if (client->state.has_welcome || h->kind != CAI_PROTOCOL_WELCOME ||
        h->attachment == 0U || h->generation == 0U || h->request != 0U ||
        h->conversation != 0U || h->sequence != 0U || h->stream != 0U)
      return invalid(error);
    client->part = client->offset = 0U;
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (client->part != 7U)
      return invalid(error);
    if (client->expecting && memcmp(client->instance, client->expected,
                                    sizeof(client->instance)) != 0)
      return invalid(error);
    client->state.attachment = h->attachment;
    client->state.generation = h->generation;
    memcpy(client->state.instance, client->instance, sizeof(client->instance));
    client->state.persist = client->persistence;
    client->state.has_welcome = 1;
    return emit(client, event, error);
  }
  if (token->indefinite)
    return invalid(error);
  switch (client->part) {
  case 0:
    if (token->type != CAI_CBOR_ARRAY || token->argument != 3U)
      return invalid(error);
    ++client->part;
    break;
  case 1:
  case 3:
    if (token->type != CAI_CBOR_BYTES ||
        token->argument !=
            (client->part == 1U ? length : sizeof(client->instance)))
      return invalid(error);
    client->offset = 0U;
    ++client->part;
    break;
  case 2:
  case 4:
    if (token->type == CAI_CBOR_DATA) {
      size_t size = client->part == 2U ? length : sizeof(client->instance);
      if (token->length > size - client->offset)
        return invalid(error);
      if (client->part == 2U) {
        if (memcmp(token->data, directory + client->offset, token->length) != 0)
          return invalid(error);
      } else
        memcpy(client->instance + client->offset, token->data, token->length);
      client->offset += token->length;
    } else if (token->type == CAI_CBOR_END &&
               token->argument == CAI_CBOR_BYTES &&
               client->offset ==
                   (client->part == 2U ? length : sizeof(client->instance)))
      ++client->part;
    else
      return invalid(error);
    break;
  case 5:
    if (token->type != CAI_CBOR_BOOL)
      return invalid(error);
    client->persistence = token->argument != 0U;
    ++client->part;
    break;
  case 6:
    if (token->type != CAI_CBOR_END || token->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    ++client->part;
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
static int receive(void *context, const cai_protocol_event *event,
                   cai_error *error) {
  cai_workspace_client *client = context;
  const cai_protocol_header *h = event->header;
  if (client->state.phase == CAI_WORKSPACE_FAILED)
    return client->state.error_code;
  if (!client->state.has_welcome && !client->state.has_identity &&
      (client->rejecting || h->kind == CAI_PROTOCOL_REJECT)) {
    const cai_cbor_token *t = event->token;
    if (event->type == CAI_PROTOCOL_HEADER) {
      if (client->rejecting || h->request || h->conversation || h->generation ||
          h->sequence || h->stream || h->attachment)
        return invalid(error);
      client->rejecting = 1;
      client->part = client->offset = 0U;
    } else if (event->type == CAI_PROTOCOL_COMPLETE) {
      const char *directory = cai_workspace_client_directory(client);
      size_t capacity;
      char *message;
      int rc;
      if (client->part != 2U)
        return invalid(error);
      capacity = strlen(directory) + strlen(client->rejection) + 160U;
      message = cai_alloc(NULL, capacity);
      if (message == NULL)
        return cai_set_error(error, CAI_ERR_NOMEM,
                             "allocate workspace rejection diagnostic");
      snprintf(message, capacity,
               "workspace server '%s' conflicts with startup setting '%s'; "
               "use matching settings or stop that server first",
               directory, client->rejection);
      rc = cai_set_error(error, CAI_ERR_INVALID, message);
      cai_free_mem(NULL, message);
      return rc;
    } else if (t->indefinite)
      return invalid(error);
    else if (client->part == 0U) {
      if (t->type != CAI_CBOR_BYTES || t->argument == 0U ||
          t->argument > CAI_WORKSPACE_CLAIM_NAME)
        return invalid(error);
      client->part = 1U;
      client->rejection_length = (size_t)t->argument;
    } else if (client->part == 1U && t->type == CAI_CBOR_DATA) {
      if (t->length > client->rejection_length - client->offset ||
          (t->length && !cai_workspace_claim_name(t->data, t->length)))
        return invalid(error);
      memcpy(client->rejection + client->offset, t->data, t->length);
      client->offset += t->length;
    } else if (client->part == 1U && t->type == CAI_CBOR_END &&
               t->argument == CAI_CBOR_BYTES &&
               client->offset == client->rejection_length) {
      client->rejection[client->offset] = '\0';
      client->part = 2U;
    } else
      return invalid(error);
    return CAI_OK;
  }
  if (client->config.probe)
    return identity(client, event, error);
  if (!client->state.has_welcome)
    return welcome(client, event, error);
  if (h->attachment != client->state.attachment ||
      h->generation < client->state.generation ||
      h->kind == CAI_PROTOCOL_HELLO || h->kind == CAI_PROTOCOL_WELCOME ||
      h->kind == CAI_PROTOCOL_REQUEST || h->kind == CAI_PROTOCOL_SYNCHRONIZE ||
      h->kind == CAI_PROTOCOL_DETACH || h->kind == CAI_PROTOCOL_TAKEOVER ||
      h->kind == CAI_PROTOCOL_SHUTDOWN || h->kind == CAI_PROTOCOL_SUBSCRIBE ||
      h->kind == CAI_PROTOCOL_UNSUBSCRIBE || h->kind == CAI_PROTOCOL_PROBE ||
      h->kind == CAI_PROTOCOL_IDENTITY || h->kind == CAI_PROTOCOL_CANCEL ||
      h->kind == CAI_PROTOCOL_REJECT || h->kind == CAI_PROTOCOL_HISTORY_GET ||
      h->kind == CAI_PROTOCOL_HISTORY_APPEND ||
      h->kind == CAI_PROTOCOL_INPUT_SETTLED)
    return invalid(error);
  if (h->kind == CAI_PROTOCOL_ROLE) {
    if (h->conversation != 0U || h->sequence != 0U || h->stream != 0U)
      return invalid(error);
    if (event->type == CAI_PROTOCOL_PAYLOAD) {
      if (event->token->type != CAI_CBOR_UINT)
        return invalid(error);
      client->role = event->token->argument;
    }
    if (event->type != CAI_PROTOCOL_COMPLETE)
      return CAI_OK;
    if (client->role_generation == h->generation &&
        client->role != client->state.controller)
      return invalid(error);
    client->state.controller = client->role;
    client->role_generation = h->generation;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    client->state.generation = h->generation;
    if (client->state.phase != CAI_WORKSPACE_DETACHING) {
      client->state.phase = client->role_generation == h->generation
                                ? CAI_WORKSPACE_CONNECTED
                                : CAI_WORKSPACE_GREETING;
      if (client->state.phase == CAI_WORKSPACE_CONNECTED)
        client->state.deadline_ms = 0U;
    }
  }
  return emit(client, event, error);
}
static uint32_t size_of_string(size_t length) {
  return (uint32_t)length + (length < 24U ? 1U : length < 256U ? 2U : 3U);
}
static int token(cai_workspace_client *client, cai_cbor_type type,
                 uint64_t argument, const void *data, size_t length,
                 cai_error *error) {
  cai_cbor_token value;
  memset(&value, 0, sizeof(value));
  value.type = type;
  value.argument = argument;
  value.data = data;
  value.length = length;
  return cai_wire_peer_write(client->wire, &value, error);
}
static int string(cai_workspace_client *client, cai_cbor_type type,
                  const char *value, cai_error *error) {
  size_t length = strlen(value);
  int rc = token(client, type, length, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(client, CAI_CBOR_DATA, 0U, value, length, error);
  if (rc == CAI_OK)
    rc = token(client, CAI_CBOR_END, type, NULL, 0U, error);
  return rc;
}
static int connected(cai_workspace_client *client, cai_error *error) {
  const char *directory = cai_workspace_client_directory(client);
  cai_protocol_header header;
  int rc = cai_protocol_decoder_open(receive, client, &client->protocol, error);
  if (rc == CAI_OK)
    rc = cai_wire_peer_open(client->fd, client->config.backlog_limit,
                            cai_protocol_decoder_event, client->protocol,
                            &client->wire, error);
  if (rc != CAI_OK)
    return rc;
  client->fd = -1;
  client->state.phase = CAI_WORKSPACE_GREETING;
  memset(&header, 0, sizeof(header));
  header.kind = client->config.probe ? CAI_PROTOCOL_PROBE : CAI_PROTOCOL_HELLO;
  rc = cai_protocol_peer_begin(
      client->wire, &header,
      1U + size_of_string(strlen(directory)) +
          size_of_string(strlen(CAI_WORKSPACE_SCHEMA)) +
          (client->config.probe ? 0U
           : client->expecting  ? 17U
                                : 1U) +
          cai_workspace_claim_size(client->requirements,
                                   client->config.requirement_count),
      error);
  if (rc == CAI_OK)
    rc = token(client, CAI_CBOR_ARRAY, client->config.probe ? 3U : 4U, NULL, 0U,
               error);
  if (rc == CAI_OK)
    rc = string(client, CAI_CBOR_BYTES, directory, error);
  if (rc == CAI_OK)
    rc = string(client, CAI_CBOR_TEXT, CAI_WORKSPACE_SCHEMA, error);
  if (rc == CAI_OK && !client->config.probe) {
    if (!client->expecting)
      rc = token(client, CAI_CBOR_NULL, 0U, NULL, 0U, error);
    else {
      rc = token(client, CAI_CBOR_BYTES, sizeof(client->expected), NULL, 0U,
                 error);
      if (rc == CAI_OK)
        rc = token(client, CAI_CBOR_DATA, 0U, client->expected,
                   sizeof(client->expected), error);
      if (rc == CAI_OK)
        rc = token(client, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
    }
  }
  if (rc == CAI_OK)
    rc = cai_workspace_claim_write(client->wire, client->requirements,
                                   client->config.requirement_count, error);
  if (rc == CAI_OK)
    rc = token(client, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = cai_protocol_peer_end(client->wire, error);
  return rc;
}
void cai_workspace_client_config_init(cai_workspace_client_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
int cai_workspace_client_open(const cai_workspace_client_config *config,
                              uint64_t now, cai_workspace_client **out,
                              cai_error *error) {
  cai_workspace_client *client;
  int rc, connecting;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || out == NULL ||
      config->backlog_limit > 16U * 1024U * 1024U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid workspace client configuration");
  client = cai_alloc(NULL, sizeof(*client));
  if (client == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate workspace client");
  memset(client, 0, sizeof(*client));
  client->fd = -1;
  client->config = *config;
  client->config.runtime_directory = client->config.workspace = NULL;
  client->config.expected_instance = NULL;
  client->config.requirements = NULL;
  client->expecting = config->expected_instance != NULL;
  if (client->expecting)
    memcpy(client->expected, config->expected_instance,
           sizeof(client->expected));
  client->owner = pthread_self();
  client->now = now;
  if (config->handshake_timeout_ms != 0U)
    client->state.deadline_ms = config->handshake_timeout_ms > UINT64_MAX - now
                                    ? UINT64_MAX
                                    : now + config->handshake_timeout_ms;
  rc = cai_workspace_claim_copy(config->requirements, config->requirement_count,
                                &client->requirements, error);
  if (rc == CAI_OK)
    rc = cai_workspace_endpoint_open(
        config->runtime_directory, config->workspace, &client->endpoint, error);
  if (rc == CAI_OK)
    rc = cai_workspace_endpoint_connect(client->endpoint, &client->fd,
                                        &connecting, error);
  if (rc == CAI_OK && !connecting)
    rc = connected(client, error);
  if (rc == CAI_OK)
    *out = client;
  else
    cai_workspace_client_close(client);
  return rc;
}
int cai_workspace_client_prepare(cai_workspace_client *client,
                                 struct pollfd *fd, uint64_t *deadline,
                                 cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (fd == NULL || deadline == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace client poll outputs required");
  memset(fd, 0, sizeof(*fd));
  fd->fd = -1;
  *deadline = client->state.deadline_ms;
  if (client->state.phase == CAI_WORKSPACE_CONNECTING) {
    fd->fd = client->fd;
    fd->events = POLLOUT;
  } else if (client->wire != NULL &&
             client->state.phase != CAI_WORKSPACE_FAILED &&
             client->state.phase != CAI_WORKSPACE_IDENTIFIED &&
             client->state.phase != CAI_WORKSPACE_DISCONNECTED) {
    fd->fd = cai_wire_peer_fd(client->wire);
    fd->events = POLLIN;
    if (cai_wire_peer_pending(client->wire) != 0U)
      fd->events |= POLLOUT;
  }
  return CAI_OK;
}
int cai_workspace_client_pump(cai_workspace_client *client, uint64_t now,
                              cai_error *error) {
  int rc = valid(client, error), eof = 0;
  if (rc != CAI_OK)
    return rc;
  if (client->pumping || now < client->now)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace client pump reentry or decreasing time");
  client->now = now;
  if (client->state.phase == CAI_WORKSPACE_FAILED)
    return cai_set_error(error, client->state.error_code,
                         "workspace client has failed");
  if (client->state.phase == CAI_WORKSPACE_DISCONNECTED ||
      client->state.phase == CAI_WORKSPACE_IDENTIFIED)
    return CAI_OK;
  if (client->state.deadline_ms != 0U && now >= client->state.deadline_ms) {
    rc = cai_set_error(error, CAI_ERR_LIMIT, "workspace greeting timed out");
    return failed(client, rc);
  }
  if (client->state.phase == CAI_WORKSPACE_CONNECTING) {
    struct pollfd fd;
    memset(&fd, 0, sizeof(fd));
    fd.fd = client->fd;
    fd.events = POLLOUT;
    rc = poll(&fd, 1U, 0);
    if (rc == 0 || (rc < 0 && errno == EINTR))
      return CAI_OK;
    if (rc < 0)
      return failed(client, cai_set_error(error, CAI_ERR_TRANSPORT,
                                          "inspect workspace connection"));
    rc = cai_workspace_endpoint_connect_finish(client->fd, error);
    if (rc == CAI_OK)
      rc = connected(client, error);
    if (rc != CAI_OK)
      return failed(client, rc);
  }
  client->pumping = 1;
  rc = cai_wire_peer_flush(client->wire, 0U, error);
  if (rc == CAI_OK)
    rc = cai_wire_peer_read(client->wire, 0U, &eof, error);
  client->pumping = 0;
  if (client->state.phase == CAI_WORKSPACE_FAILED)
    rc = client->state.error_code;
  if (rc != CAI_OK) {
    if (error != NULL && error->code == CAI_OK)
      cai_set_error(error, rc, "workspace client callback or transport failed");
    return failed(client, rc);
  }
  if (client->state.phase == CAI_WORKSPACE_IDENTIFIED) {
    release_transport(client);
    return CAI_OK;
  }
  if (eof && client->role_generation == 0U)
    return failed(client,
                  cai_set_error(error, CAI_ERR_TRANSPORT,
                                "workspace closed before greeting completed"));
  if (eof || (client->state.phase == CAI_WORKSPACE_DETACHING &&
              cai_wire_peer_pending(client->wire) == 0U)) {
    client->state.phase = CAI_WORKSPACE_DISCONNECTED;
    client->state.deadline_ms = 0U;
    release_transport(client);
  }
  return CAI_OK;
}
int cai_workspace_client_get_state(cai_workspace_client *client,
                                   cai_workspace_client_state *out,
                                   cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace client state output required");
  *out = client->state;
  return CAI_OK;
}
int cai_workspace_client_begin(cai_workspace_client *client,
                               const cai_protocol_header *header,
                               uint32_t length, int mutation,
                               cai_error *error) {
  cai_protocol_header copy;
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (header == NULL || !client->state.has_welcome || client->writing ||
      (client->state.phase != CAI_WORKSPACE_GREETING &&
       client->state.phase != CAI_WORKSPACE_CONNECTED) ||
      (header->kind != CAI_PROTOCOL_REQUEST &&
       header->kind != CAI_PROTOCOL_SYNCHRONIZE &&
       header->kind != CAI_PROTOCOL_SUBSCRIBE &&
       header->kind != CAI_PROTOCOL_UNSUBSCRIBE &&
       header->kind != CAI_PROTOCOL_CANCEL &&
       header->kind != CAI_PROTOCOL_HISTORY_GET &&
       header->kind != CAI_PROTOCOL_HISTORY_APPEND &&
       header->kind != CAI_PROTOCOL_INPUT_SETTLED) ||
      (mutation && (client->state.phase != CAI_WORKSPACE_CONNECTED ||
                    client->state.controller != client->state.attachment ||
                    header->generation != client->state.generation)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace request is not currently admissible");
  copy = *header;
  copy.attachment = client->state.attachment;
  rc = cai_protocol_peer_begin(client->wire, &copy, length, error);
  if (rc == CAI_OK)
    client->writing = 1;
  return rc == CAI_OK ? rc : failed(client, rc);
}
int cai_workspace_client_write(cai_workspace_client *client,
                               const cai_cbor_token *value, cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (!client->writing || client->state.phase == CAI_WORKSPACE_FAILED)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace output record is not open");
  rc = cai_wire_peer_write(client->wire, value, error);
  return rc == CAI_OK ? rc : failed(client, rc);
}
int cai_workspace_client_end(cai_workspace_client *client, cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (!client->writing || client->state.phase == CAI_WORKSPACE_FAILED)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace output record is not open");
  rc = cai_protocol_peer_end(client->wire, error);
  if (rc == CAI_OK)
    client->writing = 0;
  return rc == CAI_OK ? rc : failed(client, rc);
}
static int control(cai_workspace_client *client, cai_protocol_kind kind,
                   uint64_t request, uint64_t generation, cai_error *error) {
  cai_protocol_header header;
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (!client->state.has_welcome || client->writing ||
      (client->state.phase != CAI_WORKSPACE_CONNECTED &&
       client->state.phase != CAI_WORKSPACE_GREETING) ||
      (kind != CAI_PROTOCOL_DETACH && generation != client->state.generation) ||
      (kind == CAI_PROTOCOL_SHUTDOWN &&
       (client->state.phase != CAI_WORKSPACE_CONNECTED ||
        client->state.controller != client->state.attachment)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "workspace control is not currently admissible");
  memset(&header, 0, sizeof(header));
  header.kind = kind;
  header.request = request;
  header.attachment = client->state.attachment;
  header.generation = generation;
  rc = cai_protocol_peer_begin(client->wire, &header, 1U, error);
  if (rc == CAI_OK)
    rc = token(client, CAI_CBOR_NULL, 0U, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = cai_protocol_peer_end(client->wire, error);
  return rc == CAI_OK ? rc : failed(client, rc);
}
int cai_workspace_client_takeover(cai_workspace_client *client,
                                  uint64_t request, uint64_t generation,
                                  cai_error *error) {
  return control(client, CAI_PROTOCOL_TAKEOVER, request, generation, error);
}
int cai_workspace_client_shutdown(cai_workspace_client *client,
                                  uint64_t request, uint64_t generation,
                                  cai_error *error) {
  return control(client, CAI_PROTOCOL_SHUTDOWN, request, generation, error);
}
int cai_workspace_client_detach(cai_workspace_client *client,
                                cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (client->pumping)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "detach cannot reenter client callbacks");
  if (client->state.phase == CAI_WORKSPACE_DETACHING ||
      client->state.phase == CAI_WORKSPACE_IDENTIFIED ||
      client->state.phase == CAI_WORKSPACE_DISCONNECTED)
    return CAI_OK;
  if (client->state.phase == CAI_WORKSPACE_FAILED)
    return cai_set_error(error, client->state.error_code,
                         "workspace client has failed");
  if (!client->state.has_welcome || client->writing) {
    client->state.phase = CAI_WORKSPACE_DISCONNECTED;
    client->state.deadline_ms = 0U;
    release_transport(client);
    return CAI_OK;
  }
  rc =
      control(client, CAI_PROTOCOL_DETACH, 0U, client->state.generation, error);
  if (rc == CAI_OK) {
    client->state.phase = CAI_WORKSPACE_DETACHING;
    client->state.deadline_ms = 0U;
  }
  return rc;
}
const char *cai_workspace_client_directory(const cai_workspace_client *client) {
  return client != NULL ? cai_workspace_endpoint_directory(client->endpoint)
                        : NULL;
}
void cai_workspace_client_close(cai_workspace_client *client) {
  if (client == NULL || valid(client, NULL) != CAI_OK || client->pumping)
    return;
  release_transport(client);
  cai_workspace_endpoint_close(client->endpoint);
  cai_workspace_claim_free(client->requirements,
                           client->config.requirement_count);
  cai_free_mem(NULL, client);
}
