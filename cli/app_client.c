#define _POSIX_C_SOURCE 200809L
#include "input_wire.h"
#include "pouch_stream.h"
#include "status_wire.h"
#include "transcript_wire.h"
#include <cai/app_client.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cai_app_client {
  cai_workspace_client *socket;
  cai_cli_input_wire_decoder *decoder;
  cai_cli_transcript_decoder *transcript_decoder;
  cai_cli_status_decoder *status_decoder;
  cai_app_view view;
  cai_execution_decoder *execution_decoder;
  cai_execution_record execution_record;
  int event_execution, execution_callback;
  int execution_error;
  char execution_reason[192];
  uint64_t now;
  cai_app_client_config config;
  cai_app_client_state state;
  cai_input_observation baseline;
  pthread_t owner;
  unsigned long long wanted_epoch, wanted_producer;
  uint64_t binding_request, snapshot_request;
  char wanted_session[129], baseline_session[129];
  size_t index;
  int snapshot, pumping;
  uint64_t wanted_stream, wanted_conversation, wanted_position, wanted_sequence,
      stream_high, event_sequence, event_offset;
  char transcript_session[129], transcript_collection[21];
  int event_open, event_has_data;
};
static int receive_transcript(cai_app_client *, const cai_protocol_event *,
                              cai_error *);
static void discard_execution(cai_app_client *c) {
  c->event_execution = 0;
  if (c->execution_callback)
    return; /* subscribe() may replace this stream inside its borrowed callback.
             */
  c->execution_error = 0;
  c->execution_reason[0] = '\0';
  if (c->execution_decoder != NULL)
    (void)cai_execution_decoder_reset(c->execution_decoder, NULL);
  cai_execution_record_cleanup(&c->execution_record);
}
static int execution_content_error(cai_app_client *c, int rc,
                                   cai_error *error) {
  if (rc != CAI_ERR_PROTOCOL && rc != CAI_ERR_LIMIT)
    return rc;
  c->execution_error = rc;
  snprintf(c->execution_reason, sizeof(c->execution_reason), "%s",
           error != NULL && error->detail != NULL ? error->detail
           : error != NULL && error->message != NULL
               ? error->message
               : "Malformed historical execution content");
  cai_error_cleanup(error);
  return CAI_OK;
}
static int valid(cai_app_client *client, cai_error *error) {
  return client != NULL && pthread_equal(client->owner, pthread_self())
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_INVALID,
                             "application client requires its owner thread");
}
static int invalid(cai_error *error) {
  return cai_cli_error(error, CAI_ERR_PROTOCOL,
                       "workspace queue observation is inconsistent");
}
static int notify(cai_app_client *client, cai_app_client_event_type type,
                  uint64_t request, const cai_input_observation *input,
                  cai_error *error) {
  cai_app_client_event event;
  event.type = type;
  event.request = request;
  event.state = &client->state;
  event.input = input;
  event.status = type == CAI_APP_CLIENT_STATUS ? &client->view : NULL;
  return client->config.event != NULL
             ? client->config.event(client->config.context, &event, error)
             : CAI_OK;
}
static int same_baseline(cai_app_client *client,
                         const cai_cli_input_wire_message *m) {
  const cai_input_observation *o = &m->observation;
  return client->snapshot && o->conversation == client->baseline.conversation &&
         o->state.view_revision == client->baseline.state.view_revision &&
         m->header.request == client->snapshot_request;
}
static int valid_change(const cai_app_client_state *state,
                        const cai_input_observation *o) {
  const cai_input_change *change = &o->change;
  if (change->previous_view_revision != state->queue.view_revision ||
      change->previous_view_revision == UINT64_MAX ||
      o->state.view_revision != change->previous_view_revision + 1U ||
      o->state.revision < state->queue.revision)
    return 0;
  if (change->type == CAI_INPUT_STATE)
    return o->state.count == state->queue.count;
  if (change->entry.id == 0U)
    return 0;
  if (change->type == CAI_INPUT_REMOVE)
    return o->state.count + 1U == state->queue.count &&
           change->index < state->queue.count;
  return change->entry.text != NULL &&
         ((o->state.count == state->queue.count &&
           change->index < state->queue.count) ||
          (o->state.count == state->queue.count + 1U &&
           change->index == state->queue.count));
}
static int receive(void *context, const cai_protocol_event *event,
                   cai_error *error) {
  cai_app_client *client = context;
  const cai_cli_input_wire_message *m = NULL;
  cai_input_observation o;
  int rc;
  if (event->header->kind == CAI_PROTOCOL_STATUS) {
    const cai_app_view *view = NULL;
    if (client->status_decoder == NULL) {
      rc = cai_cli_status_decoder_open(&client->status_decoder, error);
      if (rc != CAI_OK)
        return rc;
    }
    rc = cai_cli_status_decode(client->status_decoder, event, &view, error);
    if (rc != CAI_OK || view == NULL)
      return rc;
    if (view->revision <= client->view.revision)
      return invalid(error);
    cai_cli_status_take(client->status_decoder, &client->view);
    client->view.received_ms = client->now;
    return notify(client, CAI_APP_CLIENT_STATUS, 0U, NULL, error);
  }
  if (event->header->kind == CAI_PROTOCOL_WELCOME ||
      event->header->kind == CAI_PROTOCOL_ROLE) {
    rc = cai_workspace_client_get_state(client->socket,
                                        &client->state.transport, error);
    return rc == CAI_OK ? notify(client, CAI_APP_CLIENT_CONNECTION,
                                 event->header->request, NULL, error)
                        : rc;
  }
  if (event->header->kind == CAI_PROTOCOL_REPLAY_BEGIN ||
      event->header->kind == CAI_PROTOCOL_REPLAY_END ||
      event->header->kind == CAI_PROTOCOL_STREAM_BEGIN ||
      event->header->kind == CAI_PROTOCOL_STREAM_CHUNK ||
      event->header->kind == CAI_PROTOCOL_STREAM_END ||
      event->header->kind == CAI_PROTOCOL_SUBSCRIPTION_ERROR)
    return receive_transcript(client, event, error);
  if (event->header->kind != CAI_PROTOCOL_QUEUE_BEGIN &&
      event->header->kind != CAI_PROTOCOL_QUEUE_ITEM &&
      event->header->kind != CAI_PROTOCOL_QUEUE_END &&
      event->header->kind != CAI_PROTOCOL_QUEUE_CHANGE &&
      event->header->kind != CAI_PROTOCOL_RESULT)
    return invalid(error);
  rc = cai_cli_input_wire_decode(client->decoder, event, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  o = m->observation;
  rc = cai_workspace_client_get_state(client->socket, &client->state.transport,
                                      error);
  if (rc != CAI_OK)
    return rc;
  switch (event->header->kind) {
  case CAI_PROTOCOL_QUEUE_BEGIN:
    if (o.conversation == 0U ||
        (!client->state.binding && !client->state.bound) || client->snapshot ||
        (client->state.binding &&
         (m->header.request != client->binding_request ||
          (client->wanted_epoch != 0U &&
           o.conversation != client->wanted_epoch) ||
          (client->wanted_producer != 0U &&
           o.producer != client->wanted_producer) ||
          (client->wanted_session[0] != '\0' &&
           strcmp(client->wanted_session, m->session) != 0))) ||
        (client->state.bound &&
         (o.conversation != client->state.conversation ||
          o.producer != client->state.producer ||
          strcmp(m->session, client->state.session) != 0)) ||
        (client->state.conversation == o.conversation &&
         strcmp(client->state.session, m->session) == 0 &&
         (o.state.view_revision < client->state.queue.view_revision ||
          o.state.revision < client->state.queue.revision)))
      return invalid(error);
    client->baseline = o;
    client->snapshot_request = m->header.request;
    strcpy(client->baseline_session, m->session);
    client->snapshot = 1;
    client->index = 0U;
    client->state.ready = 0;
    break;
  case CAI_PROTOCOL_QUEUE_ITEM:
    if (!same_baseline(client, m) || o.index != client->index ||
        client->index >= client->baseline.state.count)
      return invalid(error);
    o.state = client->baseline.state;
    o.producer = client->baseline.producer;
    o.last_request = client->baseline.last_request;
    ++client->index;
    break;
  case CAI_PROTOCOL_QUEUE_END:
    if (!same_baseline(client, m) || o.index != client->index ||
        client->index != client->baseline.state.count ||
        o.state.revision != client->baseline.state.revision)
      return invalid(error);
    o.state = client->baseline.state;
    o.producer = client->baseline.producer;
    o.last_request = client->baseline.last_request;
    if (client->state.producer != o.producer ||
        client->state.conversation != o.conversation ||
        strcmp(client->state.session, client->baseline_session) != 0 ||
        client->state.last_request < o.last_request)
      client->state.last_request = o.last_request;
    client->state.producer = o.producer;
    client->state.conversation = o.conversation;
    client->state.queue = o.state;
    strcpy(client->state.session, client->baseline_session);
    client->state.binding = client->snapshot = 0;
    client->state.bound = client->state.ready = 1;
    break;
  case CAI_PROTOCOL_QUEUE_CHANGE:
    if (!client->state.ready || client->snapshot ||
        o.conversation != client->state.conversation ||
        o.producer != client->state.producer ||
        !valid_change(&client->state, &o))
      return invalid(error);
    client->state.queue = o.state;
    if (client->state.last_request < o.last_request)
      client->state.last_request = o.last_request;
    break;
  case CAI_PROTOCOL_RESULT:
    if (m->category == CAI_CLI_INPUT_CONTROL_RESULT)
      return notify(client, CAI_APP_CLIENT_CONTROL_RESULT, m->header.request,
                    &o, error);
    if (m->category == CAI_CLI_INPUT_BIND_REJECTED) {
      if (!client->state.binding ||
          m->header.request != client->binding_request)
        return invalid(error);
      client->state.binding = client->state.bound = client->state.ready =
          client->snapshot = 0;
      return notify(client, CAI_APP_CLIENT_BIND_REJECTED, m->header.request, &o,
                    error);
    }
    if (m->category == CAI_CLI_INPUT_RESET) {
      if (m->header.request != 0U ||
          (client->state.bound && o.conversation != 0U &&
           o.conversation != client->state.conversation))
        return invalid(error);
      client->state.ready = client->snapshot = 0;
      if (o.conversation == 0U || o.result.code != CAI_OK)
        client->state.binding = client->state.bound = 0;
    } else {
      if (!client->state.bound || m->header.request == 0U ||
          o.conversation != client->state.conversation ||
          o.producer != client->state.producer)
        return invalid(error);
      if (client->state.last_request < o.last_request)
        client->state.last_request = o.last_request;
    }
    break;
  default:
    return invalid(error);
  }
  return notify(client, CAI_APP_CLIENT_INPUT, m->header.request, &o, error);
}
void cai_app_client_config_init(cai_app_client_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
int cai_app_client_open(const cai_app_client_config *config, uint64_t now,
                        cai_app_client **out, cai_error *error) {
  cai_workspace_client_config socket;
  cai_app_client *client;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (config == NULL || out == NULL)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "application client configuration and output required");
  client = calloc(1U, sizeof(*client));
  if (client == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate application client");
  client->owner = pthread_self();
  client->config = *config;
  client->config.runtime_directory = client->config.workspace = NULL;
  client->config.expected_instance = NULL;
  cai_workspace_client_config_init(&socket);
  socket.runtime_directory = config->runtime_directory;
  socket.workspace = config->workspace;
  socket.handshake_timeout_ms = config->handshake_timeout_ms;
  socket.expected_instance = config->expected_instance;
  socket.backlog_limit = config->backlog_limit;
  socket.event = receive;
  socket.context = client;
  rc = cai_cli_input_wire_decoder_open(&client->decoder, error);
  if (rc == CAI_OK)
    rc = cai_workspace_client_open(&socket, now, &client->socket, error);
  if (rc != CAI_OK)
    cai_app_client_close(client);
  else
    *out = client;
  return rc;
}
int cai_app_client_prepare(cai_app_client *client, struct pollfd *fd,
                           uint64_t *deadline, cai_error *error) {
  int rc = valid(client, error);
  return rc == CAI_OK
             ? cai_workspace_client_prepare(client->socket, fd, deadline, error)
             : rc;
}
int cai_app_client_get_state(cai_app_client *client, cai_app_client_state *out,
                             cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "client state output required");
  rc = cai_workspace_client_get_state(client->socket, &client->state.transport,
                                      error);
  if (rc == CAI_OK) {
    if (client->state.transport.phase == CAI_WORKSPACE_FAILED ||
        client->state.transport.phase == CAI_WORKSPACE_DISCONNECTED) {
      client->state.ready = client->state.bound = client->state.binding =
          client->snapshot = 0;
      client->state.transcript.active = client->state.transcript.replaying =
          client->state.transcript.subscribing = client->event_open = 0;
      client->view.stale = 1;
    }
    *out = client->state;
  }
  return rc;
}
int cai_app_client_get_view(cai_app_client *client, const cai_app_view **out,
                            cai_error *error) {
  cai_app_client_state state;
  int rc = cai_app_client_get_state(client, &state, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "status view output required");
  *out = client->view.revision == 0U ? NULL : &client->view;
  return CAI_OK;
}
int cai_app_client_pump(cai_app_client *client, uint64_t now,
                        cai_error *error) {
  cai_workspace_client_phase before;
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (client->pumping)
    return cai_cli_error(error, CAI_ERR_INVALID, "client pump cannot reenter");
  before = client->state.transport.phase;
  client->now = now;
  client->pumping = 1;
  rc = cai_workspace_client_pump(client->socket, now, error);
  (void)cai_workspace_client_get_state(client->socket, &client->state.transport,
                                       NULL);
  if (client->state.transport.phase == CAI_WORKSPACE_FAILED ||
      client->state.transport.phase == CAI_WORKSPACE_DISCONNECTED) {
    client->state.ready = client->state.bound = client->state.binding =
        client->snapshot = 0;
    client->state.transcript.active = client->state.transcript.replaying =
        client->state.transcript.subscribing = client->event_open = 0;
    client->view.stale = 1;
    cai_cli_status_decoder_close(client->status_decoder);
    client->status_decoder = NULL;
    discard_execution(client);
    if (rc == CAI_OK && before != client->state.transport.phase)
      rc = notify(client, CAI_APP_CLIENT_CONNECTION, 0U, NULL, error);
  }
  client->pumping = 0;
  return rc;
}
static int output_begin(void *context, const cai_protocol_header *h,
                        uint32_t length, cai_error *error) {
  cai_app_client *client = context;
  /* Receipt lookup precedes authority at the server, including observers and
   * stale generations. Never upgrade a request's original scope here. */
  return cai_workspace_client_begin(client->socket, h, length, 0, error);
}
static int output_write(void *context, const cai_cbor_token *token,
                        cai_error *error) {
  cai_app_client *client = context;
  return cai_workspace_client_write(client->socket, token, error);
}
static int output_end(void *context, cai_error *error) {
  cai_app_client *client = context;
  return cai_workspace_client_end(client->socket, error);
}
static int send(cai_app_client *client, const cai_cli_input_wire_message *m,
                cai_error *error) {
  cai_cli_input_wire_output output;
  output.context = client;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  return cai_cli_input_wire_write(&output, m, error);
}
static int receive_transcript(cai_app_client *c,
                              const cai_protocol_event *event,
                              cai_error *error) {
  const cai_cli_transcript_message *m = NULL;
  const cai_transcript_event *e;
  cai_app_transcript_state *s = &c->state.transcript;
  const char *reason = "workspace transcript observation is inconsistent";
  int rc;
  if (c->wanted_stream == 0U || event->header->stream > c->wanted_stream)
    return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
  if (event->header->stream < c->wanted_stream)
    return CAI_OK;
  if (event->header->request != c->wanted_stream)
    return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
  if (c->transcript_decoder == NULL) {
    rc = cai_cli_transcript_decoder_open(&c->transcript_decoder, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_cli_transcript_decode(c->transcript_decoder, event, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  e = &m->event;
  switch (e->kind) {
  case CAI_TRANSCRIPT_REPLAY_BEGIN:
    if (!s->subscribing || e->session == NULL || e->collection == NULL ||
        e->collection[0] == '\0' || e->conversation == 0U ||
        (c->wanted_conversation != 0U &&
         e->conversation != c->wanted_conversation) ||
        strcmp(e->session, c->transcript_session) != 0 ||
        (c->transcript_collection[0] != '\0' &&
         strcmp(e->collection, c->transcript_collection) != 0) ||
        e->watermark < c->wanted_position)
      return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
    strcpy(s->session, e->session);
    strcpy(s->collection, e->collection);
    s->stream = e->stream;
    s->conversation = e->conversation;
    s->watermark = e->watermark;
    s->visibility = e->visibility;
    s->sequence = c->wanted_sequence;
    s->position = c->wanted_position;
    s->active = s->replaying = 1;
    s->subscribing = c->event_open = 0;
    break;
  case CAI_TRANSCRIPT_EVENT_BEGIN:
    if (!s->active || c->event_open || e->conversation != s->conversation ||
        e->sequence <= s->sequence || e->type == NULL || e->type[0] == '\0')
      return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
    c->event_open = 1;
    c->event_sequence = e->sequence;
    c->event_offset = 0U;
    c->event_has_data = e->has_data;
    discard_execution(c);
    if (c->config.execution != NULL && (strcmp(e->type, "execution") == 0 ||
                                        strcmp(e->type, "subagent") == 0)) {
      c->event_execution = strcmp(e->type, "subagent") == 0 ? 2 : 1;
      if (!e->has_data) {
        c->execution_error = CAI_ERR_PROTOCOL;
        strcpy(c->execution_reason,
               "execution transcript entry has no JSON data");
      } else if (c->execution_decoder == NULL) {
        rc = cai_execution_decoder_open(&c->execution_decoder, error);
        if (rc != CAI_OK)
          return rc;
      }
    }
    break;
  case CAI_TRANSCRIPT_DATA:
    if (!s->active || !c->event_open || !c->event_has_data ||
        e->conversation != s->conversation ||
        e->sequence != c->event_sequence || e->offset != c->event_offset ||
        e->length == 0U || e->length > UINT64_MAX - c->event_offset)
      return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
    if (c->event_execution && !c->execution_error) {
      rc = cai_execution_decoder_feed(c->execution_decoder, e->data, e->length,
                                      error);
      if (rc != CAI_OK && execution_content_error(c, rc, error) != CAI_OK)
        return rc;
    }
    c->event_offset += e->length;
    break;
  case CAI_TRANSCRIPT_EVENT_END:
    if (!s->active || !c->event_open || e->conversation != s->conversation ||
        e->sequence != c->event_sequence || e->offset != c->event_offset ||
        e->position <= s->position ||
        (s->replaying ? e->position > s->watermark
                      : e->position <= s->watermark))
      return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
    if (c->event_execution) {
      cai_app_execution_event execution;
      const cai_execution_record *r = &c->execution_record;
      if (!c->execution_error) {
        rc = cai_execution_decoder_finish(c->execution_decoder,
                                          &c->execution_record, error);
        if (rc != CAI_OK && execution_content_error(c, rc, error) != CAI_OK)
          return rc;
      }
      if (!c->execution_error && c->event_execution == 2 &&
          (r->subagent == NULL || r->parent_session_id == NULL ||
           r->parent_call_id == NULL || r->source_sequence == 0U)) {
        c->execution_error = CAI_ERR_PROTOCOL;
        strcpy(c->execution_reason,
               "subagent execution has no source identity");
      }
      execution.boundary = e;
      execution.record = c->execution_error ? NULL : r;
      execution.code = c->execution_error;
      execution.reason = c->execution_error ? c->execution_reason : NULL;
      execution.replay = s->replaying;
      execution.subagent = c->event_execution == 2;
      c->execution_callback = 1;
      rc = c->config.execution(c->config.execution_context, &execution, error);
      c->execution_callback = 0;
      cai_execution_record_cleanup(&c->execution_record);
      if (rc != CAI_OK)
        return rc;
    }
    break;
  case CAI_TRANSCRIPT_REPLAY_END:
    if (!s->active || !s->replaying || c->event_open ||
        e->conversation != s->conversation || e->watermark != s->watermark)
      return cai_cli_error(error, CAI_ERR_PROTOCOL, reason);
    s->replaying = 0;
    break;
  case CAI_TRANSCRIPT_ERROR:
    s->active = s->replaying = s->subscribing = c->event_open = 0;
    discard_execution(c);
    break;
  }
  rc = c->config.transcript != NULL
           ? c->config.transcript(c->config.transcript_context, e, error)
           : CAI_OK;
  if (c->wanted_stream != e->stream)
    return rc; /* A callback explicitly replaced this subscription. */
  if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_EVENT_END) {
    s->sequence = e->sequence;
    s->position = e->position;
    c->event_open = 0;
  } else if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_REPLAY_END)
    s->position = e->watermark;
  return rc;
}
static int send_transcript(cai_app_client *c,
                           const cai_cli_transcript_message *m,
                           cai_error *error) {
  cai_cli_input_wire_output out;
  out.context = c;
  out.begin = output_begin;
  out.write = output_write;
  out.end = output_end;
  return cai_cli_transcript_write(&out, m, error);
}
int cai_app_client_subscribe(cai_app_client *c, uint64_t request,
                             uint64_t conversation, const char *session,
                             const char *collection, uint64_t after,
                             uint64_t sequence, cai_error *error) {
  cai_cli_transcript_message m;
  int rc = valid(c, error);
  if (rc != CAI_OK)
    return rc;
  rc = cai_workspace_client_get_state(c->socket, &c->state.transport, error);
  if (rc != CAI_OK)
    return rc;
  if (c->state.transport.phase != CAI_WORKSPACE_CONNECTED || session == NULL ||
      session[0] == '\0' || strnlen(session, 129U) > 128U ||
      (collection != NULL && strnlen(collection, 21U) > 20U) ||
      request <= c->stream_high ||
      ((after != 0U || sequence != 0U) &&
       (collection == NULL || collection[0] == '\0')) ||
      (after == 0U && sequence != 0U))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "Connected client and verified transcript cursor required");
  memset(&m, 0, sizeof(m));
  m.header.kind = CAI_PROTOCOL_SUBSCRIBE;
  m.header.request = request;
  m.header.conversation = conversation;
  m.header.generation = c->state.transport.generation;
  m.event.session = session;
  m.event.collection = collection;
  m.event.position = after;
  m.event.sequence = sequence;
  rc = send_transcript(c, &m, error);
  if (rc == CAI_OK) {
    c->wanted_stream = c->stream_high = request;
    c->wanted_conversation = conversation;
    c->wanted_position = after;
    c->wanted_sequence = sequence;
    strcpy(c->transcript_session, session);
    snprintf(c->transcript_collection, sizeof(c->transcript_collection), "%s",
             collection != NULL ? collection : "");
    c->state.transcript.subscribing = 1;
    c->state.transcript.active = 0;
    c->state.transcript.replaying = c->event_open = 0;
    discard_execution(c);
  }
  return rc;
}
int cai_app_client_cancel(cai_app_client *c, uint64_t request,
                          uint64_t generation, cai_error *error) {
  cai_protocol_header header;
  cai_cbor_token token;
  int rc = valid(c, error);
  if (rc != CAI_OK)
    return rc;
  if (request == 0U || !c->state.ready || c->state.conversation == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "nonzero cancellation ID required");
  memset(&header, 0, sizeof(header));
  memset(&token, 0, sizeof(token));
  header.kind = CAI_PROTOCOL_CANCEL;
  header.conversation = c->state.conversation;
  header.request = request;
  header.generation = generation;
  header.attachment = c->state.transport.attachment;
  token.type = CAI_CBOR_NULL;
  rc = cai_workspace_client_begin(c->socket, &header, 1U, 1, error);
  if (rc == CAI_OK)
    rc = cai_workspace_client_write(c->socket, &token, error);
  if (rc == CAI_OK)
    rc = cai_workspace_client_end(c->socket, error);
  return rc;
}

int cai_app_client_unsubscribe(cai_app_client *c, cai_error *error) {
  cai_cli_transcript_message m;
  int rc = valid(c, error);
  if (rc != CAI_OK)
    return rc;
  if (c->wanted_stream == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID, "No transcript subscription");
  memset(&m, 0, sizeof(m));
  m.header.kind = CAI_PROTOCOL_UNSUBSCRIBE;
  m.header.request = m.header.stream = c->wanted_stream;
  m.header.conversation = c->state.transcript.conversation;
  m.header.generation = c->state.transport.generation;
  return send_transcript(c, &m, error);
}
int cai_app_client_bind(cai_app_client *client, uint64_t request,
                        unsigned long long epoch, unsigned long long producer,
                        const char *session, cai_error *error) {
  cai_cli_input_wire_message m;
  int rc = valid(client, error);
  size_t length = session != NULL ? strnlen(session, 129U) : 0U;
  if (rc != CAI_OK)
    return rc;
  rc = cai_workspace_client_get_state(client->socket, &client->state.transport,
                                      error);
  if (rc != CAI_OK)
    return rc;
  if (client->state.transport.phase != CAI_WORKSPACE_CONNECTED ||
      client->state.binding || client->state.bound || request == 0U ||
      length > 128U || (producer != 0U && length == 0U))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "connected unbound client and valid producer required");
  memset(&m, 0, sizeof(m));
  m.header.kind = CAI_PROTOCOL_SYNCHRONIZE;
  m.header.request = request;
  m.header.conversation = epoch;
  m.header.generation = client->state.transport.generation;
  m.producer = producer;
  m.session = session;
  rc = send(client, &m, error);
  if (rc == CAI_OK) {
    client->binding_request = request;
    client->wanted_epoch = epoch;
    client->wanted_producer = producer;
    if (length != 0U)
      memcpy(client->wanted_session, session, length);
    client->wanted_session[length] = '\0';
    client->state.binding = 1;
  }
  return rc;
}
int cai_app_client_request(cai_app_client *client, const cai_input_scope *scope,
                           const cai_input_request *request, cai_error *error) {
  cai_cli_input_wire_message m;
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (scope == NULL || request == NULL || request->request_id == 0U ||
      !client->state.bound ||
      scope->conversation != client->state.conversation ||
      (!client->state.ready && request->operation != CAI_INPUT_SYNCHRONIZE &&
       request->operation != CAI_INPUT_RESOLVE))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "bound producer, original scope and request required");
  memset(&m, 0, sizeof(m));
  m.header.kind = CAI_PROTOCOL_REQUEST;
  m.header.request = request->request_id;
  m.header.generation = scope->generation;
  m.header.conversation = scope->conversation;
  m.request = *request;
  return send(client, &m, error);
}
int cai_app_client_takeover(cai_app_client *client, uint64_t request,
                            uint64_t generation, cai_error *error) {
  int rc = valid(client, error);
  return rc == CAI_OK ? cai_workspace_client_takeover(client->socket, request,
                                                      generation, error)
                      : rc;
}
int cai_app_client_shutdown(cai_app_client *client, uint64_t request,
                            uint64_t generation, cai_error *error) {
  int rc = valid(client, error);
  return rc == CAI_OK ? cai_workspace_client_shutdown(client->socket, request,
                                                      generation, error)
                      : rc;
}
int cai_app_client_detach(cai_app_client *client, cai_error *error) {
  int rc = valid(client, error);
  if (rc != CAI_OK)
    return rc;
  if (client->pumping)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "detach cannot reenter client");
  return cai_workspace_client_detach(client->socket, error);
}
const char *cai_app_client_directory(const cai_app_client *client) {
  return client != NULL && pthread_equal(client->owner, pthread_self())
             ? cai_workspace_client_directory(client->socket)
             : NULL;
}
void cai_app_client_close(cai_app_client *client) {
  if (client == NULL || valid(client, NULL) != CAI_OK || client->pumping)
    return;
  cai_workspace_client_close(client->socket);
  cai_cli_input_wire_decoder_close(client->decoder);
  cai_cli_transcript_decoder_close(client->transcript_decoder);
  cai_cli_status_decoder_close(client->status_decoder);
  cai_cli_status_cleanup(&client->view);
  cai_execution_record_cleanup(&client->execution_record);
  cai_execution_decoder_close(client->execution_decoder);
  free(client);
}
