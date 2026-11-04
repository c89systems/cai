#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/input_wire.h"
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
  uint64_t attachment;
  size_t binds, requests, begins, rows, ends, results, resets, rejected, roles;
  cai_input_request request;
  cai_protocol_header header;
  char text[80];
  int cancel;
} fixture;
static int received(void *context, const cai_workspace_server_event *event,
                    cai_error *error) {
  fixture *f = context;
  const cai_cli_input_wire_message *m = NULL;
  int rc;
  if (event->type == CAI_WORKSPACE_ATTACHED)
    f->attachment = event->attachment;
  if (event->type != CAI_WORKSPACE_MESSAGE)
    return CAI_OK;
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
static void open_fixture(fixture *f, const char *runtime,
                         const char *workspace) {
  cai_workspace_server_config server;
  cai_app_client_config client;
  memset(f, 0, sizeof(*f));
  assert(cai_cli_input_wire_decoder_open(&f->decoder, NULL) == CAI_OK);
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
  assert(cai_app_client_detach(f->client, NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_directory(f->client) == NULL);
  cai_app_client_close(f->client);
  return NULL;
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
