#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/catalog_wire.h"
#include "../cli/input_wire.h"
#include "../cli/status_wire.h"
#include "../cli/transcript_wire.h"
#include <assert.h>
#include <cai/app_client.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct fixture {
  cai_workspace_server *server;
  cai_app_client *client;
  cai_cli_input_wire_decoder *decoder;
  cai_cli_transcript_decoder *transcript_decoder;
  uint64_t attachment;
  size_t binds, requests, begins, rows, ends, results, resets, rejected, roles;
  cai_input_request request;
  cai_protocol_header header;
  char text[80];
  int cancel;
  size_t subscriptions, spans, record_ends;
  size_t statuses;
  size_t executions;
  size_t execution_warnings;
  int reject_execution, replace_execution, live_execution, child_execution;
  int replace_transcript;
  cai_transcript_kind reject_kind;
  int reject_transcript;
  size_t catalogs, catalog_items;
  int reject_catalog;
} fixture;
static int catalog_observed(void *context, const cai_app_catalog_event *e,
                            cai_error *error) {
  fixture *f = context;
  (void)error;
  assert(cai_app_client_pump(f->client, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  cai_app_client_close(f->client);
  if (f->reject_catalog)
    return CAI_ERR_CANCELLED;
  ++f->catalogs;
  if (e->phase == CAI_APP_CATALOG_ITEM) {
    assert(strcmp(e->text, "example-skill") == 0);
    ++f->catalog_items;
  }
  return CAI_OK;
}
static int received(void *context, const cai_workspace_server_event *event,
                    cai_error *error) {
  fixture *f = context;
  const cai_cli_input_wire_message *m = NULL;
  int rc;
  if (event->type == CAI_WORKSPACE_ATTACHED)
    f->attachment = event->attachment;
  if (event->type != CAI_WORKSPACE_MESSAGE)
    return CAI_OK;
  if (event->message->header->kind == CAI_PROTOCOL_SUBSCRIBE ||
      event->message->header->kind == CAI_PROTOCOL_UNSUBSCRIBE) {
    const cai_cli_transcript_message *transcript = NULL;
    rc = cai_cli_transcript_decode(f->transcript_decoder, event->message,
                                   &transcript, error);
    if (rc == CAI_OK && transcript != NULL)
      ++f->subscriptions;
    return rc;
  }
  rc = cai_cli_input_wire_decode(f->decoder, event->message, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  f->header = m->header;
  if (m->header.kind == CAI_PROTOCOL_SYNCHRONIZE)
    ++f->binds;
  else {
    ++f->requests;
    f->request = m->request;
    if (m->request.text != NULL) {
      assert(strlen(m->request.text) < sizeof(f->text));
      strcpy(f->text, m->request.text);
      f->request.text = f->text;
    }
  }
  return CAI_OK;
}
static int transcript_observed(void *context, const cai_transcript_event *event,
                               cai_error *error) {
  fixture *f = context;
  cai_app_client_state s;
  (void)error;
  assert(cai_app_client_get_state(f->client, &s, NULL) == CAI_OK);
  assert(cai_app_client_pump(f->client, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  cai_app_client_close(f->client);
  ++f->spans;
  if (f->reject_transcript && event->kind == f->reject_kind)
    return CAI_ERR_CANCELLED;
  if (event->kind == CAI_TRANSCRIPT_EVENT_END) {
    /* Cursor acknowledgement follows callback acceptance. */
    assert(s.transcript.position < event->position &&
           s.transcript.sequence < event->sequence);
    ++f->record_ends;
    if (f->replace_transcript)
      assert(cai_app_client_subscribe(f->client, 2U, 11U, "session", NULL, 0U,
                                      0U, -1LL, NULL) == CAI_OK);
  }
  return CAI_OK;
}
static int observed(void *context, const cai_app_client_event *event,
                    cai_error *error) {
  fixture *f = context;
  cai_app_client_state state;
  (void)error;
  if (f->cancel)
    return CAI_ERR_CANCELLED;
  assert(cai_app_client_get_state(f->client, &state, NULL) == CAI_OK);
  assert(state.ready == event->state->ready);
  assert(cai_app_client_pump(f->client, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  cai_app_client_close(f->client); /* Cannot destroy a callback's owner. */
  if (event->type == CAI_APP_CLIENT_CONNECTION) {
    ++f->roles;
    assert(event->input == NULL);
  } else if (event->type == CAI_APP_CLIENT_STATUS) {
    const cai_app_view *view;
    assert(event->input == NULL && event->status != NULL);
    assert(cai_app_client_get_view(f->client, &view, NULL) == CAI_OK &&
           view == event->status && !view->stale);
    ++f->statuses;
  } else if (event->type == CAI_APP_CLIENT_BIND_REJECTED) {
    ++f->rejected;
    assert(!state.bound && !state.ready && !state.binding);
  } else {
    const cai_input_observation *o = event->input;
    assert(o != NULL);
    if (o->type == CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN) {
      assert(!state.ready);
      ++f->begins;
    } else if (o->type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM) {
      assert(!state.ready && o->state.count == 2U && o->producer == 7U &&
             o->entry.text != NULL);
      ++f->rows;
    } else if (o->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END) {
      assert(state.ready && state.bound && !state.binding &&
             o->state.count == state.queue.count);
      ++f->ends;
    } else if (o->type == CAI_INPUT_OBSERVATION_RESULT)
      ++f->results;
    else if (o->type == CAI_INPUT_OBSERVATION_DISCONNECTED)
      ++f->resets;
  }
  return CAI_OK;
}
static int execution_observed(void *context, const cai_app_execution_event *e,
                              cai_error *error) {
  fixture *f = context;
  cai_app_client_state s;
  (void)error;
  assert(e->boundary != NULL && e->boundary->kind == CAI_TRANSCRIPT_EVENT_END);
  assert(cai_app_client_get_state(f->client, &s, NULL) == CAI_OK &&
         s.transcript.position < e->boundary->position);
  if (e->code != CAI_OK) {
    assert(e->record == NULL && e->reason != NULL && e->reason[0] != '\0');
    ++f->execution_warnings;
    return f->reject_execution ? CAI_ERR_CANCELLED : CAI_OK;
  }
  assert(e->record != NULL && e->reason == NULL);
  assert(strcmp(e->record->kind, "tool.output") == 0 &&
         strcmp(e->record->call_id, "call") == 0 &&
         strcmp(e->record->data, "first\nλ") == 0);
  assert(e->replay == !f->live_execution && e->subagent == f->child_execution);
  assert(cai_app_client_get_state(f->client, &s, NULL) == CAI_OK &&
         s.transcript.position < e->boundary->position);
  assert(cai_app_client_pump(f->client, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  cai_app_client_close(f->client);
  ++f->executions;
  if (f->replace_execution)
    assert(cai_app_client_subscribe(f->client, 2U, 11U, "session", NULL, 0U, 0U,
                                    -1LL, NULL) == CAI_OK);
  /* Replacement must not free the borrowed metadata before callback return. */
  assert(strcmp(e->record->call_id, "call") == 0);
  return f->reject_execution ? CAI_ERR_CANCELLED : CAI_OK;
}
static void open_fixture(fixture *f, const char *runtime,
                         const char *workspace) {
  cai_workspace_server_config server;
  cai_app_client_config client;
  memset(f, 0, sizeof(*f));
  assert(cai_cli_input_wire_decoder_open(&f->decoder, NULL) == CAI_OK);
  assert(cai_cli_transcript_decoder_open(&f->transcript_decoder, NULL) ==
         CAI_OK);
  cai_workspace_server_config_init(&server);
  server.runtime_directory = runtime;
  server.workspace = workspace;
  server.lifetime.persist = 1;
  server.event = received;
  server.context = f;
  assert(cai_workspace_server_open(&server, &f->server, NULL) == CAI_OK);
  assert(cai_workspace_server_ready(f->server, 0U, NULL) == CAI_OK);
  cai_app_client_config_init(&client);
  client.runtime_directory = runtime;
  client.workspace = workspace;
  client.event = observed;
  client.context = f;
  client.transcript = transcript_observed;
  client.transcript_context = f;
  client.execution = execution_observed;
  client.execution_context = f;
  client.catalog = catalog_observed;
  client.catalog_context = f;
  assert(cai_app_client_open(&client, 0U, &f->client, NULL) == CAI_OK);
}
static int exchange(fixture *f) {
  size_t i;
  for (i = 0U; i < 12U; ++i) {
    int rc = cai_app_client_pump(f->client, 1U, NULL);
    if (rc != CAI_OK)
      return rc;
    assert(cai_workspace_server_pump(f->server, 1U, NULL) == CAI_OK);
  }
  return CAI_OK;
}
static cai_app_client_state state(fixture *f) {
  cai_app_client_state s;
  assert(cai_app_client_get_state(f->client, &s, NULL) == CAI_OK);
  return s;
}
static void close_fixture(fixture *f) {
  cai_app_client_close(f->client);
  cai_workspace_server_close(f->server);
  cai_cli_input_wire_decoder_close(f->decoder);
  cai_cli_transcript_decoder_close(f->transcript_decoder);
}
static int begin(void *context, const cai_protocol_header *header,
                 uint32_t length, cai_error *error) {
  fixture *f = context;
  return cai_workspace_server_begin(f->server, f->attachment, header, length,
                                    error);
}
static int write_token(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  fixture *f = context;
  return cai_workspace_server_write(f->server, f->attachment, token, error);
}
static int end(void *context, cai_error *error) {
  fixture *f = context;
  return cai_workspace_server_end(f->server, f->attachment, error);
}
static void message(cai_cli_input_wire_message *m, cai_protocol_kind kind) {
  cai_input_observation *o = &m->observation;
  memset(m, 0, sizeof(*m));
  m->header.kind = kind;
  m->header.conversation = 11U;
  m->header.request = 90U;
  m->header.sequence = kind == CAI_PROTOCOL_RESULT ? 0U : 5U;
  m->session = "session";
  o->producer = 7U;
  o->last_request = 3U;
  o->state.count = 2U;
  o->state.revision = 2U;
  o->state.auto_approval = 1;
  o->entry.text = "original text";
  o->entry.id = o->entry.revision = 1U;
}
static void send(fixture *f, cai_cli_input_wire_message *m) {
  cai_cli_input_wire_output output;
  output.context = f;
  output.begin = begin;
  output.write = write_token;
  output.end = end;
  assert(cai_cli_input_wire_write(&output, m, NULL) == CAI_OK);
}
static void bind(fixture *f) {
  assert(exchange(f) == CAI_OK &&
         state(f).transport.phase == CAI_WORKSPACE_CONNECTED);
  assert(cai_app_client_bind(f->client, 90U, 11U, 7U, "session", NULL) ==
         CAI_OK);
  assert(exchange(f) == CAI_OK && f->binds == 1U);
}
static void baseline(fixture *f, int starting, uint64_t view) {
  cai_cli_input_wire_message m;
  size_t i;
  if (starting) {
    message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
    m.header.sequence = view;
    send(f, &m);
  }
  for (i = 0U; i < 2U; ++i) {
    message(&m, CAI_PROTOCOL_QUEUE_ITEM);
    m.header.sequence = view;
    m.observation.index = i;
    m.observation.entry.id = i + 1U;
    send(f, &m);
  }
  message(&m, CAI_PROTOCOL_QUEUE_END);
  m.header.sequence = view;
  send(f, &m);
  assert(exchange(f) == CAI_OK && state(f).ready);
}
static void *wrong_owner(void *context) {
  fixture *f = context;
  cai_app_client_state s;
  struct pollfd fd;
  uint64_t deadline;
  assert(cai_app_client_get_state(f->client, &s, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_prepare(f->client, &fd, &deadline, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_app_client_pump(f->client, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_bind(f->client, 1U, 0U, 0U, NULL, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_app_client_takeover(f->client, 1U, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_shutdown(f->client, 1U, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_cancel(f->client, 1U, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_subscribe(f->client, 1U, 0U, "session", NULL, 0U, 0U,
                                  -1LL, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_unsubscribe(f->client, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_directory(f->client) == NULL);
  cai_app_client_close(f->client);
  return NULL;
}
static void transcript_message(cai_cli_transcript_message *m,
                               cai_protocol_kind kind) {
  memset(m, 0, sizeof(*m));
  m->header.kind = kind;
  m->header.conversation = 11U;
  m->header.request = m->header.stream = 1U;
  if (kind == CAI_PROTOCOL_STREAM_BEGIN || kind == CAI_PROTOCOL_STREAM_CHUNK ||
      kind == CAI_PROTOCOL_STREAM_END)
    m->header.sequence = 8U;
  m->event.session = "session";
  m->event.collection = "collection";
  m->event.watermark = 10U;
  m->event.type = "assistant_text_delta";
  m->event.has_data = 1;
  m->event.position = 9U;
  m->event.data = "abc";
  m->event.length = 3U;
  m->event.failure = CAI_TRANSCRIPT_HISTORY_CHANGED;
  m->event.code = CAI_ERR_INVALID;
  m->event.reason = "History changed";
}
static void transcript_send(fixture *f, cai_cli_transcript_message *m) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = write_token;
  out.end = end;
  assert(cai_cli_transcript_write(&out, m, NULL) == CAI_OK);
}
static void subscribe(fixture *f) {
  assert(exchange(f) == CAI_OK);
  assert(cai_app_client_subscribe(f->client, 1U, 11U, "session", NULL, 0U, 0U,
                                  -1LL, NULL) == CAI_OK);
  assert(exchange(f) == CAI_OK && f->subscriptions == 1U &&
         state(f).transcript.subscribing && !state(f).bound);
}
static void transcript_tests(const char *runtime, const char *workspace) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_transcript_message m;
  unsigned int scenario;
  assert(f != NULL);
  /* Invalid framing is terminal only for the receiving attachment. The
   * provisional record never advances the accepted reconnect cursor. */
  for (scenario = 0U; scenario < 18U; ++scenario) {
    open_fixture(f, runtime, workspace);
    subscribe(f);
    transcript_message(&m, CAI_PROTOCOL_REPLAY_BEGIN);
    if (scenario == 0U)
      m.event.session = "wrong";
    if (scenario == 1U)
      m.header.conversation = 12U;
    if (scenario == 2U)
      m.header.request = m.header.stream = 2U;
    if (scenario < 3U) {
      transcript_send(f, &m);
    } else {
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK && state(f).transcript.active);
      transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
      if (scenario == 3U)
        m.event.type = "";
      if (scenario == 4U)
        m.header.conversation = 12U;
      if (scenario >= 5U && scenario != 15U && scenario != 16U) {
        if (scenario == 14U)
          m.event.has_data = 0;
        transcript_send(f, &m);
        assert(exchange(f) == CAI_OK);
        if (scenario == 5U) {
          /* Duplicate BEGIN while a candidate is open. */
        } else if (scenario == 6U || scenario == 7U || scenario == 14U) {
          transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
          if (scenario == 6U)
            m.event.offset = 1U;
          if (scenario == 7U)
            m.header.sequence = 9U;
        } else if (scenario == 8U) {
          transcript_message(&m, CAI_PROTOCOL_REPLAY_END);
        } else {
          transcript_message(&m, CAI_PROTOCOL_STREAM_END);
          if (scenario == 9U)
            m.event.offset = 3U;
          if (scenario == 10U)
            m.event.position = 11U;
          if (scenario == 11U)
            m.event.position = 0U;
          if (scenario == 12U)
            m.header.sequence = 9U;
          if (scenario == 13U) {
            f->reject_transcript = 1;
            f->reject_kind = CAI_TRANSCRIPT_EVENT_END;
          }
          if (scenario == 17U) {
            cai_workspace_server_close(f->server);
            f->server = NULL;
            assert(cai_app_client_pump(f->client, 2U, NULL) == CAI_OK &&
                   !state(f).transcript.active &&
                   state(f).transcript.position == 0U);
            close_fixture(f);
            continue;
          }
        }
      } else if (scenario == 15U) {
        transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
      } else if (scenario == 16U) {
        transcript_message(&m, CAI_PROTOCOL_REPLAY_END);
        m.event.watermark = 11U;
      }
      transcript_send(f, &m);
    }
    assert(exchange(f) ==
           (scenario == 13U ? CAI_ERR_CANCELLED : CAI_ERR_PROTOCOL));
    assert(state(f).transport.phase == CAI_WORKSPACE_FAILED &&
           !state(f).transcript.active && state(f).transcript.position == 0U &&
           f->record_ends == 0U);
    close_fixture(f);
  }
  open_fixture(f, runtime, workspace);
  subscribe(f);
  assert(cai_app_client_subscribe(f->client, 1U, 11U, "session", NULL, 0U, 0U,
                                  -1LL, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_subscribe(f->client, 2U, 11U, "session", NULL, 9U, 8U,
                                  -1LL, NULL) == CAI_ERR_INVALID);
  transcript_message(&m, CAI_PROTOCOL_REPLAY_BEGIN);
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK);
  transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
  transcript_send(f, &m);
  transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK && state(f).transcript.position == 0U);
  transcript_message(&m, CAI_PROTOCOL_STREAM_END);
  m.event.offset = 3U;
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK && f->record_ends == 1U &&
         state(f).transcript.position == 9U &&
         state(f).transcript.sequence == 8U);
  transcript_message(&m, CAI_PROTOCOL_REPLAY_END);
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).transcript.replaying &&
         state(f).transcript.position == 10U);
  /* Explicit replacement hides stale in-flight frames, then a refusal leaves
   * no old subscription alive. It preserves the accepted cursor for recovery.
   */
  assert(cai_app_client_subscribe(f->client, 2U, 11U, "session", "collection",
                                  10U, 8U, -1LL, NULL) == CAI_OK);
  assert(exchange(f) == CAI_OK);
  transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).transcript.active);
  transcript_message(&m, CAI_PROTOCOL_SUBSCRIPTION_ERROR);
  m.header.request = m.header.stream = 2U;
  transcript_send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).transcript.active &&
         !state(f).transcript.subscribing &&
         state(f).transcript.position == 10U &&
         state(f).transcript.sequence == 8U);
  close_fixture(f);
  free(f);
}
static void roundtrip(const char *runtime, const char *workspace) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_input_wire_message m;
  cai_input_request r;
  cai_input_scope scope;
  cai_app_client_state s;
  struct pollfd fd;
  uint64_t deadline;
  char *text = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
  pthread_t other;
  assert(f != NULL && text != NULL);
  open_fixture(f, runtime, workspace);
  assert(cai_app_client_bind(f->client, 1U, 0U, 0U, NULL, NULL) ==
         CAI_ERR_INVALID);
  bind(f);
  assert(pthread_create(&other, NULL, wrong_owner, f) == 0);
  assert(pthread_join(other, NULL) == 0);
  assert(strcmp(cai_app_client_directory(f->client), workspace) == 0);
  assert(cai_app_client_get_state(f->client, NULL, NULL) == CAI_ERR_INVALID);
  memset(&r, 0, sizeof(r));
  r.request_id = 4U;
  r.operation = CAI_INPUT_OFFER;
  r.text = "hello";
  scope.conversation = 11U;
  scope.generation = 1U;
  assert(cai_app_client_request(f->client, &scope, &r, NULL) ==
         CAI_ERR_INVALID);
  message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
  send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).ready);
  /* A mutation invalidates a partial binding, then restarts its baseline. */
  message(&m, CAI_PROTOCOL_RESULT);
  m.category = CAI_CLI_INPUT_RESET;
  m.header.request = 0U;
  send(f, &m);
  assert(exchange(f) == CAI_OK && state(f).binding && f->resets == 1U);
  message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
  send(f, &m);
  memset(text, 'x', CAI_CLI_INPUT_TEXT_MAX);
  text[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  message(&m, CAI_PROTOCOL_QUEUE_ITEM);
  m.observation.entry.text = text;
  send(f, &m);
  assert(exchange(f) == CAI_OK && f->rows == 1U && !state(f).ready);
  message(&m, CAI_PROTOCOL_QUEUE_ITEM);
  m.observation.index = 1U;
  m.observation.entry.id = 2U;
  send(f, &m);
  message(&m, CAI_PROTOCOL_QUEUE_END);
  send(f, &m);
  assert(exchange(f) == CAI_OK && f->ends == 1U);
  s = state(f);
  assert(s.ready && s.bound && s.producer == 7U && s.conversation == 11U &&
         s.queue.auto_approval && s.last_request == 3U &&
         strcmp(s.session, "session") == 0);
  /* The exact stale generation goes to the server, rather than being upgraded
   * or suppressed before its durable receipt can be looked up. */
  scope.generation = UINT64_MAX;
  assert(cai_app_client_request(f->client, &scope, &r, NULL) == CAI_OK);
  assert(exchange(f) == CAI_OK && f->requests == 1U &&
         f->header.generation == UINT64_MAX && f->request.request_id == 4U &&
         strcmp(f->request.text, "hello") == 0);
  message(&m, CAI_PROTOCOL_RESULT);
  m.header.request = 4U;
  m.observation.last_request = 4U;
  send(f, &m);
  assert(exchange(f) == CAI_OK && state(f).last_request == 4U);
  /* Older snapshot metadata must not lower a later rejection watermark. */
  message(&m, CAI_PROTOCOL_RESULT);
  m.category = CAI_CLI_INPUT_RESET;
  m.header.request = 0U;
  send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).ready);
  assert(cai_app_client_request(f->client, &scope, &r, NULL) ==
         CAI_ERR_INVALID);
  baseline(f, 1, 5U);
  assert(state(f).last_request == 4U);
  message(&m, CAI_PROTOCOL_QUEUE_CHANGE);
  m.header.request = 0U;
  m.header.sequence = 6U;
  m.observation.last_request = 4U;
  m.observation.change.previous_view_revision = 5U;
  m.observation.change.type = CAI_INPUT_STATE;
  send(f, &m);
  assert(exchange(f) == CAI_OK && state(f).queue.view_revision == 6U);
  /* A terminal per-consumer observation failure releases the binding. An
   * explicit resume on the same transport retains its processed watermark. */
  message(&m, CAI_PROTOCOL_RESULT);
  m.category = CAI_CLI_INPUT_RESET;
  m.header.request = 0U;
  m.observation.result.code = CAI_ERR_NOMEM;
  send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).bound && !state(f).ready);
  assert(cai_app_client_bind(f->client, 90U, 11U, 7U, "session", NULL) ==
         CAI_OK);
  assert(exchange(f) == CAI_OK && f->binds == 2U);
  baseline(f, 1, 6U);
  assert(state(f).last_request == 4U);
  assert(cai_app_client_prepare(f->client, &fd, &deadline, NULL) == CAI_OK &&
         fd.events == POLLIN && deadline == 0U);
  assert(cai_app_client_detach(f->client, NULL) == CAI_OK);
  assert(exchange(f) == CAI_OK);
  s = state(f);
  assert(!s.ready && !s.bound &&
         s.transport.phase == CAI_WORKSPACE_DISCONNECTED && s.producer == 7U &&
         strcmp(s.session, "session") == 0);
  close_fixture(f);
  free(text);
  free(f);
}
static void malformed(const char *runtime, const char *workspace) {
  unsigned int scenario;
  for (scenario = 0U; scenario < 17U; ++scenario) {
    fixture *f = calloc(1U, sizeof(*f));
    cai_cli_input_wire_message m;
    assert(f != NULL);
    open_fixture(f, runtime, workspace);
    bind(f);
    if (scenario != 0U && scenario != 10U && scenario != 11U) {
      message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
      send(f, &m);
      assert(exchange(f) == CAI_OK);
    }
    if (scenario == 8U || scenario == 9U || scenario >= 13U)
      baseline(f, 0, 5U);
    message(&m, CAI_PROTOCOL_QUEUE_ITEM);
    switch (scenario) {
    case 0U:
      break; /* No baseline. */
    case 1U:
      m.observation.index = 1U;
      break;
    case 2U:
      m.header.conversation = 12U;
      break;
    case 3U:
      m.header.sequence = 6U;
      break;
    case 4U:
      m.header.kind = CAI_PROTOCOL_QUEUE_BEGIN;
      break;
    case 5U:
      m.header.kind = CAI_PROTOCOL_QUEUE_END;
      break;
    case 6U:
      message(&m, CAI_PROTOCOL_QUEUE_ITEM);
      send(f, &m);
      m.observation.index = 1U;
      send(f, &m);
      message(&m, CAI_PROTOCOL_QUEUE_END);
      m.observation.state.revision = 3U;
      break;
    case 7U:
      m.header.kind = CAI_PROTOCOL_QUEUE_CHANGE;
      break;
    case 8U:
      m.header.kind = CAI_PROTOCOL_QUEUE_CHANGE;
      m.observation.change.previous_view_revision = 1U;
      break;
    case 9U:
      m.header.kind = CAI_PROTOCOL_QUEUE_CHANGE;
      m.observation.change.previous_view_revision = 5U;
      m.header.sequence = 6U;
      m.observation.producer = 8U;
      break;
    case 10U:
      m.header.kind = CAI_PROTOCOL_QUEUE_BEGIN;
      m.session = "different session";
      break;
    case 11U:
      message(&m, CAI_PROTOCOL_RESULT);
      m.category = CAI_CLI_INPUT_BIND_REJECTED;
      m.header.request = 91U;
      break;
    case 12U:
      m.header.request = 91U;
      break;
    case 13U:
    case 14U:
    case 15U:
      message(&m, CAI_PROTOCOL_QUEUE_CHANGE);
      m.header.sequence = 6U;
      m.observation.change.previous_view_revision = 5U;
      m.observation.change.type = CAI_INPUT_STATE;
      if (scenario == 13U)
        m.observation.state.count = 3U;
      if (scenario == 14U)
        m.observation.state.revision = 1U;
      if (scenario == 15U)
        m.header.sequence = 7U;
      break;
    case 16U:
      message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
      m.header.sequence = 4U;
      break;
    }
    send(f, &m);
    assert(exchange(f) == CAI_ERR_PROTOCOL);
    assert(state(f).transport.phase == CAI_WORKSPACE_FAILED && !state(f).ready);
    close_fixture(f);
    free(f);
  }
}
static void rejection_and_eof(const char *runtime, const char *workspace) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_input_wire_message m;
  cai_app_client_state s;
  assert(f != NULL);
  open_fixture(f, runtime, workspace);
  bind(f);
  message(&m, CAI_PROTOCOL_RESULT);
  m.category = CAI_CLI_INPUT_BIND_REJECTED;
  m.observation.result.outcome = CAI_INPUT_REJECTED;
  m.observation.result.code = CAI_ERR_INVALID;
  strcpy(m.observation.result.reason, "producer is unavailable");
  send(f, &m);
  assert(exchange(f) == CAI_OK && f->rejected == 1U && !state(f).bound);
  assert(cai_app_client_bind(f->client, 91U, 0U, 0U, NULL, NULL) == CAI_OK);
  assert(exchange(f) == CAI_OK && f->binds == 2U);
  message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
  m.header.request = 91U;
  send(f, &m);
  assert(exchange(f) == CAI_OK && !state(f).ready);
  cai_workspace_server_close(f->server);
  f->server = NULL;
  assert(cai_app_client_pump(f->client, 2U, NULL) == CAI_OK);
  s = state(f);
  assert(!s.binding && !s.ready && s.producer == 0U &&
         s.session[0] == '\0'); /* Partial identity never committed. */
  close_fixture(f);
  open_fixture(f, runtime, workspace);
  f->cancel = 1;
  assert(exchange(f) == CAI_ERR_CANCELLED);
  assert(state(f).transport.phase == CAI_WORKSPACE_FAILED);
  close_fixture(f);
  open_fixture(f, runtime, workspace);
  bind(f);
  f->cancel = 1;
  message(&m, CAI_PROTOCOL_QUEUE_BEGIN);
  send(f, &m);
  assert(exchange(f) == CAI_ERR_CANCELLED && !state(f).ready);
  close_fixture(f);
  open_fixture(f, runtime, workspace);
  assert(exchange(f) == CAI_OK);
  s = state(f);
  assert(cai_app_client_shutdown(f->client, 1U, s.transport.generation + 1U,
                                 NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_shutdown(f->client, 2U, s.transport.generation, NULL) ==
         CAI_OK);
  assert(exchange(f) == CAI_OK);
  {
    cai_attachment_state stopped;
    assert(cai_workspace_server_get_state(f->server, &stopped, NULL) ==
               CAI_OK &&
           stopped.phase == CAI_ATTACHMENT_STOPPING);
  }
  close_fixture(f);
  free(f);
}
static void execution_tests(const char *runtime, const char *workspace) {
  static const char json[] =
      "{\"kind\":\"tool.output\",\"session_id\":\"child\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"call_id\":\"call\","
      "\"subagent\":\"review\",\"parent_session_id\":\"parent\","
      "\"parent_call_id\":\"parent-call\",\"source_sequence\":99,"
      "\"data\":\"first\\nλ\"}";
  static const char missing_identity[] =
      "{\"kind\":\"tool.output\",\"session_id\":\"child\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"call_id\":\"call\","
      "\"data\":\"first\\nλ\"}";
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_transcript_message m;
  size_t scenario, offset, length;
  const char *text;
  assert(f != NULL);
  for (scenario = 0U; scenario < 13U; ++scenario) {
    text = scenario == 11U   ? missing_identity
           : scenario == 12U ? "{garbage"
                             : json;
    length = strlen(text);
    open_fixture(f, runtime, workspace);
    subscribe(f);
    transcript_message(&m, CAI_PROTOCOL_REPLAY_BEGIN);
    transcript_send(f, &m);
    assert(exchange(f) == CAI_OK);
    if (scenario == 9U) {
      transcript_message(&m, CAI_PROTOCOL_REPLAY_END);
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK);
      f->live_execution = 1;
    }
    f->child_execution = scenario == 1U || scenario == 11U;
    transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
    m.event.type = f->child_execution ? "subagent" : "execution";
    if (scenario == 8U)
      m.event.has_data = 0;
    transcript_send(f, &m);
    if (scenario == 8U) {
      assert(exchange(f) == CAI_OK && f->execution_warnings == 0U);
      transcript_message(&m, CAI_PROTOCOL_STREAM_END);
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK && f->execution_warnings == 1U &&
             f->executions == 0U && state(f).transcript.position == 9U &&
             state(f).transcript.active);
      close_fixture(f);
      continue;
    }
    assert(exchange(f) == CAI_OK && f->executions == 0U);
    for (offset = 0U; offset < length; ++offset) {
      if ((scenario == 4U || scenario == 5U || scenario == 7U) &&
          offset == length / 2U)
        break;
      transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
      m.event.offset = offset;
      m.event.data = text + offset;
      m.event.length = 1U;
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK && f->executions == 0U &&
             f->execution_warnings == 0U);
    }
    if (scenario == 4U) {
      cai_workspace_server_close(f->server);
      f->server = NULL;
      assert(cai_app_client_pump(f->client, 2U, NULL) == CAI_OK);
      assert(f->executions == 0U && state(f).transcript.position == 0U);
    } else if (scenario == 5U || scenario == 6U || scenario == 10U) {
      f->replace_execution = scenario == 6U;
      f->replace_transcript = scenario == 10U;
      if (scenario == 5U)
        assert(cai_app_client_subscribe(f->client, 2U, 11U, "session", NULL, 0U,
                                        0U, -1LL, NULL) == CAI_OK);
      else {
        transcript_message(&m, CAI_PROTOCOL_STREAM_END);
        m.event.offset = offset;
        transcript_send(f, &m);
      }
      assert(exchange(f) == CAI_OK);
      assert(state(f).transcript.subscribing &&
             state(f).transcript.position == 0U &&
             f->executions == (scenario == 5U ? 0U : 1U));
      transcript_message(&m, CAI_PROTOCOL_REPLAY_BEGIN);
      m.header.request = m.header.stream = 2U;
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK && state(f).transcript.active &&
             !state(f).transcript.subscribing);
      f->replace_execution = 0;
      f->replace_transcript = 0;
      transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
      m.header.request = m.header.stream = 2U;
      m.event.type = "execution";
      transcript_send(f, &m);
      transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
      m.header.request = m.header.stream = 2U;
      m.event.data = text;
      m.event.length = length;
      transcript_send(f, &m);
      transcript_message(&m, CAI_PROTOCOL_STREAM_END);
      m.header.request = m.header.stream = 2U;
      m.event.offset = length;
      transcript_send(f, &m);
      assert(exchange(f) == CAI_OK && state(f).transcript.position == 9U &&
             f->executions == (scenario == 5U ? 1U : 2U));
    } else {
      f->reject_execution = scenario == 2U || scenario == 12U;
      f->reject_transcript = scenario == 3U;
      f->reject_kind = CAI_TRANSCRIPT_EVENT_END;
      transcript_message(&m, CAI_PROTOCOL_STREAM_END);
      if (scenario == 9U)
        m.event.position = 11U;
      m.event.offset = offset;
      transcript_send(f, &m);
      if (scenario == 2U || scenario == 3U)
        assert(exchange(f) == CAI_ERR_CANCELLED && f->executions == 1U &&
               state(f).transcript.position == 0U);
      else if (scenario == 12U)
        assert(exchange(f) == CAI_ERR_CANCELLED && f->executions == 0U &&
               f->execution_warnings == 1U &&
               state(f).transcript.position == 0U);
      else if (scenario == 7U || scenario == 11U)
        assert(exchange(f) == CAI_OK && f->executions == 0U &&
               f->execution_warnings == 1U && state(f).transcript.active &&
               state(f).transcript.position == 9U);
      else
        assert(exchange(f) == CAI_OK && f->executions == 1U &&
               state(f).transcript.position == (scenario == 9U ? 11U : 9U));
      if (scenario == 7U || scenario == 11U) {
        transcript_message(&m, CAI_PROTOCOL_STREAM_BEGIN);
        m.header.sequence = 9U;
        m.event.type = f->child_execution ? "subagent" : "execution";
        transcript_send(f, &m);
        transcript_message(&m, CAI_PROTOCOL_STREAM_CHUNK);
        m.header.sequence = 9U;
        m.event.data = json;
        m.event.length = strlen(json);
        transcript_send(f, &m);
        transcript_message(&m, CAI_PROTOCOL_STREAM_END);
        m.header.sequence = 9U;
        m.event.position = 10U;
        m.event.offset = strlen(json);
        transcript_send(f, &m);
        assert(exchange(f) == CAI_OK && f->executions == 1U &&
               f->execution_warnings == 1U &&
               state(f).transcript.position == 10U);
      }
    }
    close_fixture(f);
  }
  free(f);
}
static void status_tests(const char *runtime, const char *workspace) {
  fixture f;
  cai_cli_input_wire_output output;
  cai_app_view source;
  const cai_app_view *view;
  cai_protocol_header h;
  cai_cbor_token t;
  size_t i;
  for (i = 0U; i < 3U; ++i) {
    open_fixture(&f, runtime, workspace);
    assert(exchange(&f) == CAI_OK);
    assert(cai_app_client_get_view(f.client, &view, NULL) == CAI_OK &&
           view == NULL && !state(&f).bound && state(&f).producer == 0U);
    memset(&source, 0, sizeof(source));
    source.available = 1;
    source.revision = 1U;
    source.conversation = 89U;
    source.session_id = "status-without-producer";
    source.provider = "custom";
    source.effective.model = "literal\033[31m**model**";
    source.quota.has_credit_balance = 1;
    source.quota.credit_balance = 58816.38;
    source.goal.has_goal = 1;
    source.goal.status = "paused";
    output.context = &f;
    output.begin = begin;
    output.write = write_token;
    output.end = end;
    assert(cai_cli_status_write(&output, &source, NULL) == CAI_OK);
    assert(exchange(&f) == CAI_OK && f.statuses == 1U && f.binds == 0U);
    assert(cai_app_client_get_view(f.client, &view, NULL) == CAI_OK &&
           view != NULL && view->available && !view->stale &&
           view->revision == 1U && view->conversation == 89U &&
           view->quota.credit_balance == 58816.38 &&
           strcmp(view->effective.model, source.effective.model) == 0);
    if (i == 0U) {
      /* An actual partially flushed frame followed by EOF must not replace
       * the accepted view or create a second status notification. */
      memset(&h, 0, sizeof(h));
      h.kind = CAI_PROTOCOL_STATUS;
      h.sequence = 2U;
      assert(begin(&f, &h, 150U, NULL) == CAI_OK);
      memset(&t, 0, sizeof(t));
      t.type = CAI_CBOR_ARRAY;
      t.argument = 2U;
      assert(write_token(&f, &t, NULL) == CAI_OK);
      assert(cai_workspace_server_pump(f.server, 2U, NULL) == CAI_OK);
      assert(cai_app_client_pump(f.client, 2U, NULL) == CAI_OK);
      cai_workspace_server_close(f.server);
      f.server = NULL;
      assert(cai_app_client_pump(f.client, 3U, NULL) == CAI_ERR_PROTOCOL);
      assert(cai_app_client_get_view(f.client, &view, NULL) == CAI_OK &&
             view->stale && view->revision == 1U && f.statuses == 1U);
    } else {
      if (i == 1U) {
        ++source.revision;
        f.cancel = 1;
      }
      /* Replayed revisions fail; consumer refusal is also terminal. */
      assert(cai_cli_status_write(&output, &source, NULL) == CAI_OK);
      assert(exchange(&f) == (i == 1U ? CAI_ERR_CANCELLED : CAI_ERR_PROTOCOL));
      assert(state(&f).transport.phase == CAI_WORKSPACE_FAILED);
      assert(cai_app_client_get_view(f.client, &view, NULL) == CAI_OK &&
             view->stale);
    }
    close_fixture(&f);
  }
}
static void catalog_send(fixture *f, const cai_app_catalog_event *row) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = write_token;
  out.end = end;
  assert(cai_cli_catalog_write(&out, row, NULL) == CAI_OK);
}
static void catalog_tests(const char *runtime, const char *workspace) {
  fixture *f = calloc(1U, sizeof(*f));
  unsigned int scenario;
  assert(f != NULL);
  for (scenario = 0U; scenario < 11U; ++scenario) {
    cai_app_catalog_event row;
    size_t i;
    open_fixture(f, runtime, workspace);
    assert(exchange(f) == CAI_OK && !state(f).bound);
    memset(&row, 0, sizeof(row));
    row.revision = 5U;
    row.conversation = 11U;
    row.text = "";
    if (scenario != 1U) {
      if (scenario == 8U)
        f->reject_catalog = 1;
      catalog_send(f, &row);
      if (scenario == 8U) {
        assert(exchange(f) == CAI_ERR_CANCELLED && f->catalogs == 0U);
        close_fixture(f);
        continue;
      }
      assert(exchange(f) == CAI_OK && f->catalogs == 1U &&
             f->catalog_items == 0U);
    }
    row.phase = CAI_APP_CATALOG_ITEM;
    row.text = "example-skill";
    if (scenario == 2U) {
      row.phase = CAI_APP_CATALOG_BEGIN;
      row.text = "";
    }
    if (scenario == 3U)
      row.index = 1U;
    if (scenario == 4U)
      row.conversation = 12U;
    if (scenario == 5U)
      row.revision = 6U;
    if (scenario == 6U) {
      row.phase = CAI_APP_CATALOG_END;
      row.index = 1U;
      row.text = "";
    }
    if (scenario == 7U || scenario == 9U) {
      row.phase = CAI_APP_CATALOG_END;
      row.text = "";
      catalog_send(f, &row);
      assert(exchange(f) == CAI_OK);
      row.phase = scenario == 7U ? CAI_APP_CATALOG_ITEM : CAI_APP_CATALOG_ERROR;
      row.text = "example-skill";
      if (scenario == 9U) {
        row.code = CAI_ERR_CANCELLED;
        catalog_send(f, &row);
        assert(exchange(f) == CAI_OK);
      }
    }
    if (scenario == 10U) {
      for (i = 0U; i < CAI_SKILLS_MAX_PACKAGES; ++i) {
        row.index = i;
        catalog_send(f, &row);
        assert(exchange(f) == CAI_OK);
      }
      assert(f->catalog_items == CAI_SKILLS_MAX_PACKAGES);
      row.index = CAI_SKILLS_MAX_PACKAGES;
    }
    catalog_send(f, &row);
    if (scenario == 0U) {
      assert(exchange(f) == CAI_OK && f->catalog_items == 1U);
      row.phase = CAI_APP_CATALOG_END;
      row.index = 1U;
      row.text = "";
      catalog_send(f, &row);
      assert(exchange(f) == CAI_OK);
      row.phase = CAI_APP_CATALOG_ERROR;
      row.code = CAI_ERR_CANCELLED;
      row.text = "Conversation closed";
      catalog_send(f, &row);
      assert(exchange(f) == CAI_OK);
      row.phase = CAI_APP_CATALOG_BEGIN;
      row.revision = 6U;
      row.conversation = 12U;
      row.index = 0U;
      row.code = CAI_OK;
      row.text = "";
      catalog_send(f, &row);
      assert(exchange(f) == CAI_OK);
      row.phase = CAI_APP_CATALOG_END;
      catalog_send(f, &row);
      assert(exchange(f) == CAI_OK && f->catalogs == 6U);
    } else
      assert(exchange(f) == CAI_ERR_PROTOCOL &&
             state(f).transport.phase == CAI_WORKSPACE_FAILED);
    close_fixture(f);
  }
  free(f);
}
static void *run(void *context) {
  char path[] = "./ac.XXXXXX";
  char *runtime, *workspace;
  DIR *directory;
  struct dirent *entry;
  (void)context;
  assert(mkdtemp(path) != NULL);
  runtime = realpath(path, NULL);
  workspace = realpath(".", NULL);
  assert(runtime != NULL && workspace != NULL);
  roundtrip(runtime, workspace);
  malformed(runtime, workspace);
  rejection_and_eof(runtime, workspace);
  transcript_tests(runtime, workspace);
  status_tests(runtime, workspace);
  execution_tests(runtime, workspace);
  catalog_tests(runtime, workspace);
  directory = opendir(runtime);
  assert(directory != NULL);
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.') {
      char file[512];
      assert(snprintf(file, sizeof(file), "%s/%s", runtime, entry->d_name) > 0);
      assert(unlink(file) == 0);
    }
  assert(closedir(directory) == 0 && rmdir(runtime) == 0);
  free(workspace);
  free(runtime);
  return NULL;
}
int main(void) {
  pthread_t worker;
  cai_app_client *absent;
  cai_app_client_config config;
  cai_app_client_config_init(&config);
  cai_app_client_config_init(NULL);
  assert(config.handshake_timeout_ms == 0U && config.event == NULL);
  assert(cai_app_client_open(NULL, 0U, &absent, NULL) == CAI_ERR_INVALID &&
         absent == NULL);
  assert(cai_app_client_open(&config, 0U, &absent, NULL) == CAI_ERR_INVALID &&
         absent == NULL);
  assert(cai_app_client_open(&config, 0U, NULL, NULL) == CAI_ERR_INVALID);
  cai_app_client_close(NULL);
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
