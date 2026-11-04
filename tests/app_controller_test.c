#define _POSIX_C_SOURCE 200809L
#include "../cli/input_wire.h"
#include <assert.h>
#include <cai/app.h>
#include <cai/app_client.h>
#include <cai/workspace_client.h>
#include <libmdf/mdf.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* A controller must not create presentation objects, even with a terminal. */
mdf_status __wrap_mdf_create(mdf_format, const mdf_options *, mdf **);
mdf_status __wrap_mdf_create(mdf_format format, const mdf_options *options,
                             mdf **out) {
  (void)format;
  (void)options;
  (void)out;
  abort();
}
sl_t *__wrap_sl_create_with_config(const sl_config_t *);
sl_t *__wrap_sl_create_with_config(const sl_config_t *config) {
  (void)config;
  abort();
}
typedef struct host {
  cai_app *app;
  pthread_t caller, owner;
  const char *mode;
  pthread_mutex_t lock;
  cai_input_connection *input;
  unsigned long long conversation;
  int starts, completed, text, replay, acks, rejected, steered, has_owner;
} host;
unsigned int cai_test_storage_open_count(void);
static void control(host *h, cai_app_control_type type, const char *text) {
  unsigned long long id;
  assert(cai_app_control(h->app, type, text, &id, NULL) == CAI_OK && id != 0U);
}
static void owner(host *h) {
  assert(!pthread_equal(h->caller, pthread_self()));
  if (!h->has_owner) {
    h->owner = pthread_self();
    h->has_owner = 1;
  }
  assert(pthread_equal(h->owner, pthread_self()));
  assert(getenv("CAI_CONTROLLER_PRIVATE") == NULL);
}
static int authorize(void *context, uint64_t attachment, uint64_t generation,
                     cai_error *error) {
  host *h = context;
  owner(h);
  if (attachment == 42U && generation == 7U)
    return CAI_OK;
  if (error != NULL) {
    error->code = CAI_ERR_INVALID;
    error->message = strdup("frontend does not control this workspace");
    assert(error->message != NULL);
  }
  return CAI_ERR_INVALID;
}
static void input_offer(host *h, unsigned long long id, const char *text,
                        cai_input_intent intent) {
  cai_input_scope scope;
  cai_input_request request;
  scope.conversation = h->conversation;
  scope.generation = 7U;
  memset(&request, 0, sizeof(request));
  request.request_id = id;
  request.operation = CAI_INPUT_OFFER;
  request.intent = intent;
  request.text = text;
  assert(cai_input_connection_request(h->input, &scope, &request, NULL) ==
         CAI_OK);
}
static int event(void *context, const cai_agent_runtime_event *e,
                 cai_error *error) {
  host *h = context;
  owner(h);
  if (e->type == CAI_AGENT_EVENT_RUN_STARTED) {
    int code;
    pthread_mutex_lock(&h->lock);
    ++h->starts;
    pthread_mutex_unlock(&h->lock);
    assert(cai_app_join(h->app, &code, NULL) == CAI_ERR_INVALID);
    assert(cai_app_start(h->app, NULL, 0, NULL, NULL) == CAI_ERR_INVALID);
    if (strcmp(h->mode, "queue") == 0 && h->starts == 1) {
      control(h, CAI_APP_QUEUE_PROMPT, "CONTROLLER QUEUED ONE");
      control(h, CAI_APP_QUEUE_PROMPT, "CONTROLLER QUEUED TWO");
    }
    if (strcmp(h->mode, "cancel") == 0 && h->starts == 1)
      control(h, CAI_APP_CANCEL_TURN, NULL);
    if (strcmp(h->mode, "stop_active") == 0)
      control(h, CAI_APP_STOP, NULL);
  }
  if (e->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    pthread_mutex_lock(&h->lock);
    ++h->text;
    pthread_mutex_unlock(&h->lock);
    if (strcmp(h->mode, "observer_error") == 0) {
      error->code = CAI_ERR_PROTOCOL;
      error->message = strdup("CONTROLLER OBSERVER FAILED");
      assert(error->message != NULL);
      return CAI_ERR_PROTOCOL;
    }
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
      strcmp(h->mode, "steer") == 0 && !h->steered) {
    h->steered = 1;
    control(h, CAI_APP_STEER, "CONTROLLER STEERING");
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
      strcmp(h->mode, "input_steer") == 0 && !h->steered) {
    h->steered = 1;
    input_offer(h, 2U, "FRONTEND STEERING", CAI_INPUT_STEERING);
  }
  if (e->type == CAI_AGENT_EVENT_RUN_COMPLETED) {
    pthread_mutex_lock(&h->lock);
    ++h->completed;
    pthread_mutex_unlock(&h->lock);
  }
  if (e->type == CAI_AGENT_EVENT_RUN_CANCELLED &&
      strcmp(h->mode, "cancel") == 0)
    control(h, CAI_APP_QUEUE_PROMPT, "AFTER CONTROLLER CANCEL");
  if (e->type == CAI_AGENT_EVENT_RUN_FAILED &&
      strcmp(h->mode, "provider_error") == 0 && !h->steered) {
    h->steered = 1;
    control(h, CAI_APP_QUEUE_PROMPT, "AFTER PROVIDER FAILURE");
  }
  return CAI_OK;
}
static int replay(void *context, const cai_agent_session_event *e,
                  cai_error *error) {
  host *h = context;
  (void)e;
  (void)error;
  owner(h);
  if (++h->replay == 1 && strcmp(h->mode, "replay_stop") == 0)
    control(h, CAI_APP_STOP, NULL);
  return CAI_OK;
}
static void ack(void *context, unsigned long long id, int status,
                const cai_error *error) {
  host *h = context;
  (void)error;
  owner(h);
  assert(id != 0U);
  pthread_mutex_lock(&h->lock);
  ++h->acks;
  if (status != CAI_OK)
    ++h->rejected;
  pthread_mutex_unlock(&h->lock);
}
static int register_tools(void *context, cai_agent *agent, cai_error *error) {
  host *h = context;
  (void)agent;
  (void)error;
  owner(h);
  if (strcmp(h->mode, "startup_stop") == 0)
    control(h, CAI_APP_STOP, NULL);
  return CAI_OK;
}
static void pause_tick(void) {
  struct timespec delay = {0, 1000000L};
  nanosleep(&delay, NULL);
}
static void idle_cpu(host *h) {
  clockid_t clock;
  struct timespec before_cpu, after_cpu, idle = {0, 250000000L};
  assert(pthread_getcpuclockid(h->owner, &clock) == 0);
  assert(clock_gettime(clock, &before_cpu) == 0);
  nanosleep(&idle, NULL);
  assert(clock_gettime(clock, &after_cpu) == 0);
  assert((after_cpu.tv_sec - before_cpu.tv_sec) * 1000000000LL +
             after_cpu.tv_nsec - before_cpu.tv_nsec <
         50000000LL);
}
static void wait_ready(host *h) {
  cai_app_status s;
  struct pollfd fd;
  size_t ticks;
  cai_app_status_init(&s);
  for (ticks = 0U; ticks < 10000U; ++ticks) {
    assert(cai_app_observe_status(h->app, &s, NULL) == CAI_OK);
    if (s.runtime_ready)
      break;
    if (!s.running)
      fprintf(stderr, "controller startup failed: code=%d %s (%s)\n",
              s.exit_code,
              s.error.message != NULL ? s.error.message : "no message",
              s.error.detail != NULL ? s.error.detail : "no detail");
    assert(s.running);
    fd.fd = cai_app_status_fd(h->app);
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 10000) == 1 && (fd.revents & POLLIN) != 0);
  }
  assert(s.running && s.runtime_ready && s.session_id != NULL);
  cai_app_status_cleanup(&s);
}
static void wait_count(host *h, int turns, int rejected) {
  size_t ticks;
  int done = 0;
  for (ticks = 0U; ticks < 10000U; ++ticks) {
    pthread_mutex_lock(&h->lock);
    done = h->completed >= turns && h->rejected >= rejected;
    pthread_mutex_unlock(&h->lock);
    if (done)
      break;
    pause_tick();
  }
  assert(done);
}
static void open_host(host *h, const char *mode) {
  cai_app_config config;
  static const cai_app_setting settings[] = {
      {"env", "CAI_CONTROLLER_PRIVATE", "never-installed"}};
  memset(h, 0, sizeof(*h));
  h->mode = mode;
  h->caller = pthread_self();
  assert(pthread_mutex_init(&h->lock, NULL) == 0);
  cai_app_config_init(&config);
  config.settings = settings;
  config.setting_count = 1U;
  config.event_callback = event;
  config.event_context = h;
  config.replay_callback = replay;
  config.replay_context = h;
  config.control_callback = ack;
  config.control_context = h;
  config.register_agent_tools = register_tools;
  config.register_agent_tools_context = h;
  if (strncmp(mode, "server_", 7U) == 0)
    config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
  if (strncmp(mode, "server_", 7U) != 0) {
    config.input.authorize = authorize;
    config.input.authorize_context = h;
  }
  assert(cai_app_open(&config, &h->app, NULL) == CAI_OK);
}
static cai_input_observation *input_wait(host *h,
                                         cai_input_observation_type type,
                                         unsigned long long request) {
  struct pollfd fd;
  cai_input_observation *value;
  size_t ticks;
  fd.fd = cai_input_connection_fd(h->input);
  fd.events = POLLIN;
  for (ticks = 0U; ticks < 1000U; ++ticks) {
    assert(cai_input_connection_poll(h->input, &value, NULL) == CAI_OK);
    if (value == NULL) {
      fd.revents = 0;
      assert(poll(&fd, 1U, 10000) == 1 && (fd.revents & POLLIN) != 0);
      continue;
    }
    if (value->type == type && (type != CAI_INPUT_OBSERVATION_RESULT ||
                                value->result.request_id == request))
      return value;
    cai_input_observation_close(value);
  }
  abort();
}
static void input_start(host *h) {
  cai_input_observation *value;
  assert(cai_app_connect_input(h->app, 42U, &h->input, NULL) == CAI_OK);
  value = input_wait(h, CAI_INPUT_OBSERVATION_SNAPSHOT_END, 0U);
  h->conversation = value->conversation;
  assert(h->conversation != 0U && value->producer != 0U &&
         value->last_request == 0U);
  cai_input_observation_close(value);
  input_offer(h, 1U, "CONTROLLER FIRST PROMPT", CAI_INPUT_QUEUED);
}
static void input_reconnect(host *h) {
  cai_input_observation *value;
  unsigned long long producer;
  value = input_wait(h, CAI_INPUT_OBSERVATION_CHANGE, 0U);
  producer = value->producer;
  assert(producer != 0U);
  cai_input_observation_close(value);
  /* Deliberately drop the acceptance acknowledgement. A new observer
   * connection must recover it without admitting a second prompt. */
  cai_input_connection_close(h->input);
  h->input = NULL;
  assert(cai_app_resume_input(h->app, 84U, h->conversation, producer, &h->input,
                              NULL) == CAI_OK);
  value = input_wait(h, CAI_INPUT_OBSERVATION_SNAPSHOT_END, 0U);
  assert(value->producer == producer && value->last_request >= 1U);
  cai_input_observation_close(value);
  input_offer(h, 1U, "THIS DUPLICATE MUST NEVER RUN", CAI_INPUT_QUEUED);
  value = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 1U);
  assert(value->result.outcome == CAI_INPUT_ACCEPTED);
  cai_input_observation_close(value);
  input_offer(h, 3U, "THIS OBSERVER MUST NEVER RUN", CAI_INPUT_QUEUED);
  value = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 3U);
  assert(value->result.outcome == CAI_INPUT_REJECTED &&
         strstr(value->result.reason, "does not control") != NULL);
  cai_input_observation_close(value);
  cai_input_connection_close(h->input);
  h->input = NULL;
  assert(cai_app_resume_input(h->app, 42U, h->conversation, producer, &h->input,
                              NULL) == CAI_OK);
  value = input_wait(h, CAI_INPUT_OBSERVATION_SNAPSHOT_END, 0U);
  assert(value->last_request == 3U);
  cai_input_observation_close(value);
  input_offer(h, 4U, "/status", CAI_INPUT_STEERING);
  value = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 4U);
  assert(value->result.outcome == CAI_INPUT_REJECTED &&
         strstr(value->result.reason, "presentation") != NULL);
  cai_input_observation_close(value);
  input_offer(h, 5U, "!printf command", CAI_INPUT_QUEUED);
  value = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 5U);
  assert(value->result.outcome == CAI_INPUT_REJECTED);
  cai_input_observation_close(value);
}
static void independent_controllers(int argc, char **argv) {
  host a, b;
  cai_config_context context;
  cai_environment *base = NULL, *first = NULL, *second = NULL;
  cai_environment_change change;
  cai_app_status sa, sb;
  const char **arguments = calloc((size_t)argc + 3U, sizeof(*arguments));
  const char *work = getenv("CAI_TEST_CONTROLLER_WORK");
  int i, count = 0, code;
  unsigned int opens = cai_test_storage_open_count();
  assert(arguments != NULL && work != NULL);
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0 ||
        strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--directory") == 0) {
      ++i;
      assert(i < argc);
    } else
      arguments[count++] = argv[i];
  }
  arguments[count++] = "--no-config";
  arguments[count++] = "-C";
  arguments[count++] = ".";
  assert(cai_environment_open(NULL, &base, NULL) == CAI_OK);
  change.name = "CAI_MODEL";
  change.value = "gpt-6-luna";
  assert(cai_environment_derive(base, &change, 1U, &first, NULL) == CAI_OK);
  change.value = "gpt-6.1-sol";
  assert(cai_environment_derive(base, &change, 1U, &second, NULL) == CAI_OK);
  cai_environment_close(base);
  open_host(&a, "contexts");
  open_host(&b, "contexts");
  context.directory = work;
  context.environment = first;
  assert(cai_app_start(a.app, &context, count, arguments, NULL) == CAI_OK);
  arguments[count - 1] = "child";
  context.environment = second;
  assert(cai_app_start(b.app, &context, count, arguments, NULL) == CAI_OK);
  free(arguments);
  cai_environment_close(first);
  cai_environment_close(second);
  wait_ready(&a);
  wait_ready(&b);
  cai_app_status_init(&sa);
  cai_app_status_init(&sb);
  assert(cai_app_get_status(a.app, &sa, NULL) == CAI_OK);
  assert(cai_app_get_status(b.app, &sb, NULL) == CAI_OK);
  assert(strcmp(sa.effective.model, "gpt-6-luna") == 0);
  assert(strcmp(sb.effective.model, "gpt-6.1-sol") == 0);
  assert(strcmp(sa.session_id, sb.session_id) != 0);
  assert(cai_test_storage_open_count() == opens + 1U);
  control(&a, CAI_APP_STOP, NULL);
  assert(cai_app_join(a.app, &code, NULL) == CAI_OK && code == 0);
  /* Stopping A must not close B's shared-root SDK client or its context. */
  assert(cai_app_get_status(b.app, &sb, NULL) == CAI_OK && sb.runtime_ready);
  control(&b, CAI_APP_STOP, NULL);
  assert(cai_app_join(b.app, &code, NULL) == CAI_OK && code == 0);
  cai_app_close(a.app);
  cai_app_close(b.app);
  cai_app_status_cleanup(&sa);
  cai_app_status_cleanup(&sb);
  pthread_mutex_destroy(&a.lock);
  pthread_mutex_destroy(&b.lock);
}
static uint64_t monotonic_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static void socket_wait(cai_workspace_client *client,
                        cai_workspace_client_phase phase) {
  cai_workspace_client_state state;
  uint64_t deadline, until = monotonic_ms() + 10000U;
  struct pollfd fd;
  do {
    assert(cai_workspace_client_pump(client, monotonic_ms(), NULL) == CAI_OK);
    assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
    if (state.phase == phase)
      return;
    assert(monotonic_ms() < until);
    assert(cai_workspace_client_prepare(client, &fd, &deadline, NULL) ==
           CAI_OK);
    assert(fd.fd >= 0 && poll(&fd, 1U, 1000) >= 0);
  } while (1);
}
static void server_state(host *h, size_t count, uint64_t controller) {
  cai_app_status status;
  struct pollfd fd;
  uint64_t until = monotonic_ms() + 10000U;
  cai_app_status_init(&status);
  fd.fd = cai_app_status_fd(h->app);
  fd.events = POLLIN;
  for (;;) {
    assert(cai_app_observe_status(h->app, &status, NULL) == CAI_OK &&
           status.running && status.has_server);
    if (status.server.count == count &&
        (controller == ~(uint64_t)0 ||
         status.server.controller == controller)) {
      assert(status.server.phase == CAI_ATTACHMENT_READY);
      break;
    }
    assert(monotonic_ms() < until && poll(&fd, 1U, 1000) >= 0);
  }
  cai_app_status_cleanup(&status);
}
typedef struct wire_view {
  cai_workspace_client *client;
  cai_cli_input_wire_decoder *decoder;
  unsigned long long conversation, producer, high, view, snapshot_request;
  cai_input_result result;
  cai_input_entry entry;
  char session[129];
  size_t begins, snapshots, rows, expected, results, binding_errors, resets,
      large_rows;
  int active;
} wire_view;
static int wire_event(void *context, const cai_protocol_event *event,
                      cai_error *error) {
  wire_view *v = context;
  const cai_cli_input_wire_message *message;
  const cai_input_observation *o;
  int rc;
  if (event->header->kind == CAI_PROTOCOL_WELCOME ||
      event->header->kind == CAI_PROTOCOL_ROLE)
    return CAI_OK;
  rc = cai_cli_input_wire_decode(v->decoder, event, &message, error);
  if (rc != CAI_OK || message == NULL)
    return rc;
  o = &message->observation;
  switch (o->type) {
  case CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN:
    assert(!v->active && message->session != NULL);
    v->active = 1;
    ++v->begins;
    v->rows = 0U;
    v->expected = o->state.count;
    v->conversation = o->conversation;
    v->view = o->state.view_revision;
    v->snapshot_request = message->header.request;
    v->producer = o->producer;
    v->high = o->last_request;
    assert(strlen(message->session) < sizeof(v->session));
    strcpy(v->session, message->session);
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM:
    assert(v->active && v->rows == o->index &&
           v->conversation == o->conversation &&
           v->view == o->state.view_revision &&
           v->snapshot_request == message->header.request);
    ++v->rows;
    if (strlen(o->entry.text) == CAI_CLI_INPUT_TEXT_MAX)
      ++v->large_rows;
    v->entry = o->entry;
    v->entry.text = NULL;
    break;
  case CAI_INPUT_OBSERVATION_SNAPSHOT_END:
    assert(v->active && v->rows == v->expected &&
           o->state.count == v->expected && v->view == o->state.view_revision &&
           v->snapshot_request == message->header.request);
    v->active = 0;
    ++v->snapshots;
    break;
  case CAI_INPUT_OBSERVATION_CHANGE:
    assert(!v->active && o->conversation == v->conversation &&
           o->change.previous_view_revision == v->view);
    v->view = o->state.view_revision;
    if (o->change.type == CAI_INPUT_UPSERT) {
      v->entry = o->change.entry;
      v->entry.text = NULL;
    }
    break;
  case CAI_INPUT_OBSERVATION_RESULT:
    v->result = o->result;
    v->high = o->last_request;
    ++v->results;
    break;
  case CAI_INPUT_OBSERVATION_DISCONNECTED:
    v->active = 0;
    if (message->category == CAI_CLI_INPUT_BIND_REJECTED)
      ++v->binding_errors;
    else
      ++v->resets;
    v->result = o->result;
    break;
  }
  return CAI_OK;
}
static int wire_begin(void *context, const cai_protocol_header *header,
                      uint32_t size, cai_error *error) {
  wire_view *v = context;
  /* Deliberately bypass the optional local authority guard: the server must
   * reject an observer/stale generation independently at owner admission. */
  return cai_workspace_client_begin(v->client, header, size, 0, error);
}
static int wire_write(void *context, const cai_cbor_token *token,
                      cai_error *error) {
  wire_view *v = context;
  return cai_workspace_client_write(v->client, token, error);
}
static int wire_end(void *context, cai_error *error) {
  wire_view *v = context;
  return cai_workspace_client_end(v->client, error);
}
static void wire_send(wire_view *v, cai_cli_input_wire_message *message) {
  cai_cli_input_wire_output output;
  output.context = v;
  output.begin = wire_begin;
  output.write = wire_write;
  output.end = wire_end;
  assert(cai_cli_input_wire_write(&output, message, NULL) == CAI_OK);
}
static void wire_wait(wire_view *v, const size_t *counter, size_t expected) {
  uint64_t deadline, until = monotonic_ms() + 10000U;
  struct pollfd fd;
  while (*counter < expected) {
    cai_error error;
    int rc;
    cai_error_init(&error);
    rc = cai_workspace_client_pump(v->client, monotonic_ms(), &error);
    if (rc != CAI_OK)
      fprintf(stderr, "wire client failed: %d %s\n", rc,
              error.message != NULL ? error.message : "no message");
    assert(rc == CAI_OK);
    cai_error_cleanup(&error);
    if (*counter >= expected)
      break;
    assert(monotonic_ms() < until);
    assert(cai_workspace_client_prepare(v->client, &fd, &deadline, NULL) ==
               CAI_OK &&
           fd.fd >= 0 && poll(&fd, 1U, 1000) >= 0);
  }
}
static void wire_bind(wire_view *v, unsigned long long producer,
                      const char *session) {
  cai_cli_input_wire_message message;
  cai_workspace_client_state state;
  assert(cai_workspace_client_get_state(v->client, &state, NULL) == CAI_OK);
  memset(&message, 0, sizeof(message));
  message.header.kind = CAI_PROTOCOL_SYNCHRONIZE;
  message.header.request = 1U;
  message.header.generation = state.generation;
  message.header.conversation = producer == 0U ? 0U : v->conversation;
  message.producer = producer;
  message.session = session;
  wire_send(v, &message);
}
static uint64_t wire_generation(wire_view *v) {
  cai_workspace_client_state state;
  assert(cai_workspace_client_get_state(v->client, &state, NULL) == CAI_OK);
  return state.generation;
}
static void wire_request(wire_view *v, unsigned long long id,
                         cai_input_operation operation, const char *text,
                         int held, uint64_t generation) {
  cai_cli_input_wire_message message;
  memset(&message, 0, sizeof(message));
  message.header.kind = CAI_PROTOCOL_REQUEST;
  message.header.request = id;
  message.header.conversation = v->conversation;
  message.header.generation = generation;
  message.request.request_id = id;
  message.request.operation = operation;
  message.request.text = text;
  message.request.enabled = held;
  message.request.item_id = v->entry.id;
  message.request.item_revision = v->entry.revision;
  message.request.edit_token = v->entry.edit_token;
  wire_send(v, &message);
}
static void wire_reopen(wire_view *v,
                        const cai_workspace_client_config *source) {
  cai_workspace_client_config config = *source;
  cai_workspace_client_close(v->client);
  v->client = NULL;
  cai_cli_input_wire_decoder_close(v->decoder);
  v->decoder = NULL;
  assert(cai_cli_input_wire_decoder_open(&v->decoder, NULL) == CAI_OK);
  config.event = wire_event;
  config.context = v;
  assert(cai_workspace_client_open(&config, monotonic_ms(), &v->client, NULL) ==
         CAI_OK);
  socket_wait(v->client, CAI_WORKSPACE_CONNECTED);
}
static void wire_scenario(host *h, wire_view *a, wire_view *b,
                          const cai_workspace_client_config *config) {
  char *text = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
  unsigned long long producer;
  uint64_t old_generation;
  size_t results, i;
  unsigned int storage_opens = cai_test_storage_open_count();
  wire_view extra;
  assert(text != NULL);
  memset(text, 'x', CAI_CLI_INPUT_TEXT_MAX);
  text[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  wire_bind(a, 0U, NULL);
  wire_bind(b, 0U, NULL);
  wire_wait(a, &a->snapshots, 1U);
  wire_wait(b, &b->snapshots, 1U);
  assert(a->producer != 0U && b->producer != 0U && a->producer != b->producer &&
         a->conversation == b->conversation &&
         strcmp(a->session, b->session) == 0);
  old_generation = wire_generation(a);
  wire_request(b, 1U, CAI_INPUT_OFFER, "OBSERVER MUST NEVER RUN", 0,
               wire_generation(b));
  wire_wait(b, &b->results, 1U);
  assert(b->result.outcome == CAI_INPUT_REJECTED &&
         b->result.code == CAI_ERR_INVALID);
  wire_request(a, 1U, CAI_INPUT_OFFER, text, 1, old_generation);
  wire_wait(a, &a->results, 1U);
  assert(a->result.outcome == CAI_INPUT_ACCEPTED && a->entry.held);
  wire_request(a, 2U, CAI_INPUT_BEGIN_EDIT, NULL, 0, old_generation);
  wire_wait(a, &a->results, 2U);
  assert(a->result.outcome == CAI_INPUT_ACCEPTED && a->entry.edit_held &&
         a->entry.edit_token != 0U);
  producer = a->producer;
  cai_workspace_client_close(a->client);
  a->client = NULL;
  server_state(h, 1U, 0U);
  wire_reopen(a, config);
  wire_bind(a, producer, a->session);
  wire_wait(a, &a->snapshots, 2U);
  assert(a->producer == producer && a->high == 2U && a->rows == 1U &&
         a->large_rows == 1U && a->entry.held && !a->entry.edit_held &&
         a->entry.edit_token == 0U);
  wire_request(a, 1U, CAI_INPUT_OFFER, "DUPLICATE MUST NEVER RUN", 0,
               wire_generation(a));
  wire_wait(a, &a->results, 3U);
  assert(a->result.outcome == CAI_INPUT_ACCEPTED && a->high == 2U);
  wire_request(a, 3U, CAI_INPUT_APPROVE, NULL, 0, old_generation);
  wire_wait(a, &a->results, 4U);
  assert(a->result.outcome == CAI_INPUT_REJECTED && a->entry.held);
  wire_request(a, 3U, CAI_INPUT_APPROVE, NULL, 0, wire_generation(a));
  wire_wait(a, &a->results, 5U);
  assert(a->result.outcome == CAI_INPUT_REJECTED && a->entry.held);
  wire_request(a, 4U, CAI_INPUT_APPROVE, NULL, 0, wire_generation(a));
  wire_wait(a, &a->results, 6U);
  assert(a->result.outcome == CAI_INPUT_ACCEPTED);
  wait_count(h, 1, 0);
  /* A nonreading observer must not block input or another snapshot. */
  results = a->results;
  for (i = 0U; i < 12U; ++i) {
    wire_request(a, 5U + i, CAI_INPUT_OFFER, text, 1, wire_generation(a));
    wire_wait(a, &a->results, ++results);
    assert(a->result.outcome == CAI_INPUT_ACCEPTED);
  }
  memset(&extra, 0, sizeof(extra));
  wire_reopen(&extra, config);
  wire_bind(&extra, 999999U, a->session);
  wire_wait(&extra, &extra.binding_errors, 1U);
  assert(extra.result.code == CAI_ERR_INVALID && extra.snapshots == 0U);
  wire_bind(&extra, 0U, NULL);
  wire_wait(&extra, &extra.begins, 1U);
  /* The socket cannot have finished twelve large rows in its 64 KiB read
   * budget. Mutate through the controller while this initial bind is partial.
   * It must reset/restart, rather than rejecting or reserving a new producer.
   */
  wire_request(a, 17U, CAI_INPUT_OFFER, text, 1, wire_generation(a));
  wire_wait(a, &a->results, ++results);
  wire_wait(&extra, &extra.snapshots, 1U);
  assert(extra.producer == b->producer + 1U && extra.rows == 13U &&
         extra.large_rows >= 13U && extra.resets != 0U &&
         extra.binding_errors == 1U);
  /* Close after sending only part of a structurally valid request. Neither
   * its provisional header nor fields may admit text to the queue. */
  {
    cai_protocol_header header;
    cai_cbor_token token;
    memset(&header, 0, sizeof(header));
    header.kind = CAI_PROTOCOL_REQUEST;
    header.request = 1U;
    header.conversation = extra.conversation;
    header.generation = wire_generation(&extra);
    assert(cai_workspace_client_begin(extra.client, &header, 20U, 0, NULL) ==
           CAI_OK);
    memset(&token, 0, sizeof(token));
    token.type = CAI_CBOR_ARRAY;
    token.argument = 9U;
    assert(cai_workspace_client_write(extra.client, &token, NULL) == CAI_OK);
    assert(cai_workspace_client_pump(extra.client, monotonic_ms(), NULL) ==
           CAI_OK);
  }
  cai_workspace_client_close(extra.client);
  cai_cli_input_wire_decoder_close(extra.decoder);
  server_state(h, 2U, ~(uint64_t)0);
  i = a->begins;
  wire_request(a, 18U, CAI_INPUT_SYNCHRONIZE, NULL, 0, wire_generation(a));
  wire_wait(a, &a->results, results + 1U);
  wire_wait(a, &a->begins, i + 1U);
  /* A second resync cannot relabel rows of the still-open first baseline. */
  wire_request(a, 19U, CAI_INPUT_SYNCHRONIZE, NULL, 0, wire_generation(a));
  wire_wait(a, &a->results, results + 2U);
  wire_wait(a, &a->snapshots, 3U);
  assert(a->rows == 13U && a->result.outcome == CAI_INPUT_ACCEPTED);
  assert(cai_test_storage_open_count() == storage_opens);
  free(text);
}
static void socket_control(cai_workspace_client *client, uint64_t controller,
                           uint64_t minimum_generation) {
  cai_workspace_client_state state;
  struct pollfd fd;
  uint64_t deadline, until = monotonic_ms() + 10000U;
  for (;;) {
    assert(cai_workspace_client_pump(client, monotonic_ms(), NULL) == CAI_OK);
    assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
    if (state.controller == controller &&
        state.generation >= minimum_generation)
      break;
    assert(monotonic_ms() < until);
    assert(cai_workspace_client_prepare(client, &fd, &deadline, NULL) ==
               CAI_OK &&
           fd.fd >= 0 && poll(&fd, 1U, 1000) >= 0);
  }
}
static void server_input(host *h, cai_workspace_client *a,
                         cai_workspace_client *b) {
  cai_input_request request;
  cai_input_scope scope;
  cai_workspace_client_state state;
  cai_input_observation *value;
  int i;
  for (i = 0; i < 2; ++i) {
    assert(cai_workspace_client_get_state(i == 0 ? b : a, &state, NULL) ==
           CAI_OK);
    assert(cai_app_connect_input(h->app, state.attachment, &h->input, NULL) ==
           CAI_OK);
    value = input_wait(h, CAI_INPUT_OBSERVATION_SNAPSHOT_END, 0U);
    scope.conversation = value->conversation;
    scope.generation = state.generation;
    cai_input_observation_close(value);
    memset(&request, 0, sizeof(request));
    request.request_id = 1U;
    request.operation = CAI_INPUT_OFFER;
    request.text = i == 0 ? "OBSERVER MUST NEVER RUN" : "CONTROLLING INPUT";
    assert(cai_input_connection_request(h->input, &scope, &request, NULL) ==
           CAI_OK);
    value = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 1U);
    assert(value->result.outcome ==
           (i == 0 ? CAI_INPUT_REJECTED : CAI_INPUT_ACCEPTED));
    cai_input_observation_close(value);
    cai_input_connection_close(h->input);
    h->input = NULL;
  }
  wait_count(h, 1, 0);
}
static void server_fence(host *h, cai_workspace_client *a,
                         cai_workspace_client *b) {
  cai_workspace_client_state ca, cb;
  cai_input_scope scope;
  cai_input_request request;
  cai_input_observation *event;
  assert(cai_workspace_client_get_state(a, &ca, NULL) == CAI_OK);
  assert(cai_workspace_client_get_state(b, &cb, NULL) == CAI_OK);
  assert(cai_app_connect_input(h->app, ca.attachment, &h->input, NULL) ==
         CAI_OK);
  event = input_wait(h, CAI_INPUT_OBSERVATION_SNAPSHOT_END, 0U);
  scope.conversation = event->conversation;
  scope.generation = ca.generation;
  cai_input_observation_close(event);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = 1U;
  request.text = "keep accepted original";
  request.enabled = 1;
  assert(cai_input_connection_request(h->input, &scope, &request, NULL) ==
         CAI_OK);
  event = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 1U);
  assert(event->result.outcome == CAI_INPUT_ACCEPTED);
  request.item_id = event->result.item_id;
  request.item_revision = event->result.item_revision;
  cai_input_observation_close(event);
  request.request_id = 2U;
  request.operation = CAI_INPUT_BEGIN_EDIT;
  request.text = NULL;
  request.enabled = 0;
  assert(cai_input_connection_request(h->input, &scope, &request, NULL) ==
         CAI_OK);
  event = input_wait(h, CAI_INPUT_OBSERVATION_RESULT, 2U);
  assert(event->result.outcome == CAI_INPUT_ACCEPTED &&
         event->result.edit_token != 0U);
  cai_input_observation_close(event);
  assert(cai_workspace_client_takeover(b, 10U, cb.generation, NULL) == CAI_OK);
  socket_control(b, cb.attachment, cb.generation + 1U);
  event = input_wait(h, CAI_INPUT_OBSERVATION_CHANGE, 0U);
  assert(event->change.type == CAI_INPUT_UPSERT &&
         !event->change.entry.edit_held &&
         event->change.entry.edit_token == 0U && event->change.entry.held &&
         event->change.request_id == 0U &&
         strcmp(event->change.entry.text, "keep accepted original") == 0 &&
         event->last_request == 2U);
  cai_input_observation_close(event);
  /* Restore control, without reviving an old edit token or submitting a draft.
   */
  assert(cai_workspace_client_get_state(b, &cb, NULL) == CAI_OK);
  socket_control(a, cb.attachment, cb.generation);
  assert(cai_workspace_client_takeover(a, 11U, cb.generation, NULL) == CAI_OK);
  socket_control(a, ca.attachment, cb.generation + 1U);
  cai_input_connection_close(h->input);
  h->input = NULL;
}
typedef struct typed_view {
  cai_app_client *client;
  cai_app_client_state state;
  cai_input_entry entry;
  cai_input_result result;
  size_t roles, snapshots, results, disconnected;
} typed_view;
static int typed_event(void *context, const cai_app_client_event *event,
                       cai_error *error) {
  typed_view *v = context;
  const cai_input_observation *o = event->input;
  (void)error;
  v->state = *event->state;
  assert(event->type != CAI_APP_CLIENT_BIND_REJECTED);
  if (event->type == CAI_APP_CLIENT_CONNECTION) {
    if (v->state.transport.phase == CAI_WORKSPACE_CONNECTED)
      ++v->roles;
    if (v->state.transport.phase == CAI_WORKSPACE_DISCONNECTED)
      ++v->disconnected;
  } else if (o->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END)
    ++v->snapshots;
  else if (o->type == CAI_INPUT_OBSERVATION_RESULT) {
    ++v->results;
    v->result = o->result;
  } else if (o->type == CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM) {
    v->entry = o->entry;
    v->entry.text = NULL;
  } else if (o->type == CAI_INPUT_OBSERVATION_CHANGE &&
             o->change.type == CAI_INPUT_UPSERT) {
    v->entry = o->change.entry;
    v->entry.text = NULL;
  }
  return CAI_OK;
}
static void typed_wait(typed_view *v, const size_t *counter, size_t expected) {
  uint64_t deadline, until = monotonic_ms() + 10000U;
  struct pollfd fd;
  while (*counter < expected) {
    cai_error error;
    int rc;
    cai_error_init(&error);
    rc = cai_app_client_pump(v->client, monotonic_ms(), &error);
    if (rc != CAI_OK)
      fprintf(stderr, "typed client failed: %d %s\n", rc,
              error.message != NULL ? error.message : "no message");
    assert(rc == CAI_OK);
    cai_error_cleanup(&error);
    if (*counter >= expected)
      break;
    assert(monotonic_ms() < until);
    assert(cai_app_client_prepare(v->client, &fd, &deadline, NULL) == CAI_OK &&
           fd.fd >= 0 && poll(&fd, 1U, 1000) >= 0);
  }
  assert(cai_app_client_get_state(v->client, &v->state, NULL) == CAI_OK);
}
static void typed_open(typed_view *v, unsigned long long producer,
                       const char *session) {
  cai_app_client_config config;
  memset(v, 0, sizeof(*v));
  cai_app_client_config_init(&config);
  config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
  config.workspace = getenv("CAI_TEST_CONTROLLER_WORK");
  config.event = typed_event;
  config.context = v;
  assert(cai_app_client_open(&config, monotonic_ms(), &v->client, NULL) ==
         CAI_OK);
  typed_wait(v, &v->roles, 1U);
  assert(cai_app_client_bind(v->client, 1U, 0U, producer, session, NULL) ==
         CAI_OK);
  typed_wait(v, &v->snapshots, 1U);
}
static void typed_request(typed_view *v, unsigned long long id,
                          cai_input_operation operation, const char *text,
                          uint64_t generation) {
  cai_input_scope scope;
  cai_input_request r;
  size_t results = v->results;
  memset(&r, 0, sizeof(r));
  scope.conversation = v->state.conversation;
  scope.generation = generation;
  r.request_id = id;
  r.operation = operation;
  r.text = text;
  r.enabled = operation == CAI_INPUT_OFFER;
  r.item_id = v->entry.id;
  r.item_revision = v->entry.revision;
  r.edit_token = v->entry.edit_token;
  assert(cai_app_client_request(v->client, &scope, &r, NULL) == CAI_OK);
  typed_wait(v, &v->results, results + 1U);
}
static void typed_server(int argc, char **argv) {
  host h;
  typed_view a, b;
  cai_app_client_state original;
  cai_app_status final;
  unsigned int opens;
  size_t roles;
  int code;
  open_host(&h, "server_typed");
  assert(cai_app_start_server(h.app, NULL, argc - 1,
                              (const char *const *)(argv + 1), NULL,
                              NULL) == CAI_OK);
  wait_ready(&h);
  opens = cai_test_storage_open_count();
  typed_open(&a, 0U, NULL);
  typed_open(&b, 0U, NULL);
  original = a.state;
  assert(a.state.transport.controller == a.state.transport.attachment &&
         b.state.transport.controller != b.state.transport.attachment);
  typed_request(&b, 1U, CAI_INPUT_OFFER, "OBSERVER MUST NEVER RUN",
                b.state.transport.generation);
  assert(b.result.outcome == CAI_INPUT_REJECTED);
  typed_request(&a, 1U, CAI_INPUT_OFFER, "Background work finished",
                a.state.transport.generation);
  assert(a.result.outcome == CAI_INPUT_ACCEPTED && a.entry.held);
  typed_request(&a, 2U, CAI_INPUT_BEGIN_EDIT, NULL,
                a.state.transport.generation);
  assert(a.result.outcome == CAI_INPUT_ACCEPTED && a.entry.edit_held);
  cai_app_client_close(a.client);
  server_state(&h, 1U, 0U);
  original = a.state;
  typed_open(&a, original.producer, original.session);
  assert(a.state.producer == original.producer && a.state.last_request == 2U &&
         a.state.queue.count == 1U && a.entry.held && !a.entry.edit_held);
  roles = a.roles;
  assert(cai_app_client_takeover(a.client, 80U, a.state.transport.generation,
                                 NULL) == CAI_OK);
  typed_wait(&a, &a.roles, roles + 1U);
  assert(a.state.transport.controller == a.state.transport.attachment);
  typed_request(&a, 1U, CAI_INPUT_OFFER, "Background work finished",
                original.transport.generation);
  assert(a.result.outcome == CAI_INPUT_ACCEPTED && a.state.last_request == 2U);
  typed_request(&a, 3U, CAI_INPUT_APPROVE, NULL, a.state.transport.generation);
  assert(a.result.outcome == CAI_INPUT_ACCEPTED);
  wait_count(&h, 1, 0);
  cai_app_client_close(b.client);
  server_state(&h, 1U, a.state.transport.attachment);
  assert(cai_app_client_detach(a.client, NULL) == CAI_OK);
  typed_wait(&a, &a.disconnected, 1U);
  assert(cai_app_join(h.app, &code, NULL) == CAI_OK && code == 0);
  cai_app_status_init(&final);
  assert(cai_app_get_status(h.app, &final, NULL) == CAI_OK &&
         final.server.phase == CAI_ATTACHMENT_STOPPING &&
         !final.server.persist && cai_test_storage_open_count() == opens);
  cai_app_status_cleanup(&final);
  cai_app_client_close(a.client);
  cai_app_close(h.app);
  pthread_mutex_destroy(&h.lock);
}
static void workspace_server(int argc, char **argv, const char *mode) {
  cai_attachment_config lifetime;
  cai_workspace_client_config client_config;
  cai_workspace_client *a = NULL, *b = NULL;
  cai_workspace_client_state connection;
  cai_app_status status;
  host h;
  wire_view wa, wb;
  int wire = strcmp(mode, "server_wire") == 0;
  int code, persistent = strcmp(mode, "server_persist") == 0;
  if (strcmp(mode, "server_typed") == 0) {
    typed_server(argc, argv);
    return;
  }
  open_host(&h, mode);
  memset(&wa, 0, sizeof(wa));
  memset(&wb, 0, sizeof(wb));
  if (wire) {
    assert(cai_cli_input_wire_decoder_open(&wa.decoder, NULL) == CAI_OK);
    assert(cai_cli_input_wire_decoder_open(&wb.decoder, NULL) == CAI_OK);
  }
  cai_app_status_init(&status);
  cai_attachment_config_init(&lifetime);
  lifetime.persist = persistent;
  if (strcmp(mode, "server_grace") == 0)
    lifetime.first_attachment_timeout_ms = 50U;
  assert(cai_app_start_server(
             h.app, NULL, argc - 1, (const char *const *)(argv + 1),
             strcmp(mode, "server_lifetime") == 0 ? NULL : &lifetime,
             NULL) == CAI_OK);
  if (strcmp(mode, "server_grace") != 0) {
    wait_ready(&h);
    assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK &&
           status.has_server && status.server.phase == CAI_ATTACHMENT_READY &&
           status.server.persist == persistent);
    cai_workspace_client_config_init(&client_config);
    client_config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
    client_config.workspace = getenv("CAI_TEST_CONTROLLER_WORK");
    if (wire) {
      client_config.event = wire_event;
      client_config.context = &wa;
    }
    assert(cai_workspace_client_open(&client_config, monotonic_ms(), &a,
                                     NULL) == CAI_OK);
    socket_wait(a, CAI_WORKSPACE_CONNECTED);
    wa.client = a;
    assert(cai_workspace_client_get_state(a, &connection, NULL) == CAI_OK &&
           connection.persist == persistent &&
           connection.controller == connection.attachment);
    if (wire)
      client_config.context = &wb;
    assert(cai_workspace_client_open(&client_config, monotonic_ms(), &b,
                                     NULL) == CAI_OK);
    socket_wait(b, CAI_WORKSPACE_CONNECTED);
    wb.client = b;
    assert(cai_workspace_client_get_state(b, &connection, NULL) == CAI_OK &&
           connection.controller != connection.attachment);
    server_state(&h, 2U, ~(uint64_t)0);
    if (strcmp(mode, "server_input") == 0)
      server_input(&h, a, b);
    if (strcmp(mode, "server_fence") == 0)
      server_fence(&h, a, b);
    if (wire) {
      wire_scenario(&h, &wa, &wb, &client_config);
      a = wa.client;
      b = wb.client;
    }
    if (strcmp(mode, "server_busy") == 0) {
      host other;
      unsigned int before = cai_test_storage_open_count();
      open_host(&other, mode);
      assert(cai_app_start_server(other.app, NULL, argc - 1,
                                  (const char *const *)(argv + 1), NULL,
                                  NULL) == CAI_OK);
      assert(cai_app_join(other.app, &code, NULL) == CAI_OK && code == 1);
      assert(cai_app_get_status(other.app, &status, NULL) == CAI_OK &&
             status.outcome == CAI_APP_FAILED &&
             status.error.code == CAI_ERR_LIMIT &&
             cai_test_storage_open_count() == before);
      cai_app_close(other.app);
      pthread_mutex_destroy(&other.lock);
    }
    if (strcmp(mode, "server_active") == 0) {
      int started = 0;
      size_t ticks;
      control(&h, CAI_APP_QUEUE_PROMPT, "WORK CANCELLED ON LAST DETACH");
      for (ticks = 0U; ticks < 10000U; ++ticks) {
        pthread_mutex_lock(&h.lock);
        started = h.starts != 0;
        pthread_mutex_unlock(&h.lock);
        if (started)
          break;
        pause_tick();
      }
      assert(started);
    }
    if (strcmp(mode, "server_abrupt") == 0) {
      cai_workspace_client_close(a);
      a = NULL;
    } else {
      assert(cai_workspace_client_detach(a, NULL) == CAI_OK);
      socket_wait(a, CAI_WORKSPACE_DISCONNECTED);
    }
    /* An observer keeps a nonpersistent owner alive without taking control. */
    server_state(&h, 1U, 0U);
    assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK);
    socket_control(b, 0, status.server.generation);
    assert(cai_workspace_client_get_state(b, &connection, NULL) == CAI_OK);
    assert(cai_workspace_client_takeover(b, 1U, connection.generation, NULL) ==
           CAI_OK);
    socket_control(b, connection.attachment, connection.generation + 1U);
    server_state(&h, 1U, connection.attachment);
    if (strcmp(mode, "server_abrupt") == 0) {
      cai_workspace_client_close(b);
      b = NULL;
    } else {
      assert(cai_workspace_client_detach(b, NULL) == CAI_OK);
      socket_wait(b, CAI_WORKSPACE_DISCONNECTED);
    }
    if (persistent) {
      server_state(&h, 0U, 0U);
      idle_cpu(&h);
      control(&h, CAI_APP_QUEUE_PROMPT, "WORK WITHOUT AN ATTACHMENT");
      wait_count(&h, 1, 0);
      cai_workspace_client_close(a);
      a = NULL;
      assert(cai_workspace_client_open(&client_config, monotonic_ms(), &a,
                                       NULL) == CAI_OK);
      socket_wait(a, CAI_WORKSPACE_CONNECTED);
      assert(cai_workspace_client_get_state(a, &connection, NULL) == CAI_OK);
      assert(cai_workspace_client_shutdown(a, 1U, connection.generation,
                                           NULL) == CAI_OK);
      socket_wait(a, CAI_WORKSPACE_DISCONNECTED);
    }
  }
  assert(cai_app_join(h.app, &code, NULL) == CAI_OK && code == 0);
  assert(
      cai_app_get_status(h.app, &status, NULL) == CAI_OK && !status.running &&
      status.has_server && status.outcome == CAI_APP_STOPPED &&
      status.server.phase == CAI_ATTACHMENT_STOPPING &&
      status.server.stop_reason == (persistent ? CAI_ATTACHMENT_STOP_REQUESTED
                                    : strcmp(mode, "server_grace") == 0
                                        ? CAI_ATTACHMENT_STARTUP_ABANDONED
                                        : CAI_ATTACHMENT_LAST_DETACHED));
  if (persistent)
    assert(h.completed == 1 && h.starts == 1);
  cai_workspace_client_close(a);
  cai_workspace_client_close(b);
  cai_cli_input_wire_decoder_close(wa.decoder);
  cai_cli_input_wire_decoder_close(wb.decoder);
  /* Reuse the endpoint only after the runtime/store owner has been joined. */
  h.mode = "startup_stop";
  h.has_owner = h.acks = h.starts = 0;
  assert(cai_app_start_server(h.app, NULL, argc - 1,
                              (const char *const *)(argv + 1), NULL,
                              NULL) == CAI_OK);
  assert(cai_app_join(h.app, &code, NULL) == CAI_OK && code == 0 &&
         h.acks == 1);
  cai_app_close(h.app);
  cai_app_status_cleanup(&status);
  pthread_mutex_destroy(&h.lock);
}
int main(int argc, char **argv) {
  host h;
  cai_app_status status;
  struct sigaction before, after;
  char cwd[4096], current[4096];
  char *locale = strdup(setlocale(LC_CTYPE, NULL));
  const char *mode = getenv("CAI_TEST_CONTROLLER_MODE");
  int code;
  assert(locale != NULL && mode != NULL &&
         getenv("CAI_CONTROLLER_PRIVATE") == NULL);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(sigaction(SIGINT, NULL, &before) == 0);
  if (strcmp(mode, "contexts") == 0 || strncmp(mode, "server_", 7U) == 0) {
    if (strcmp(mode, "contexts") == 0)
      independent_controllers(argc, argv);
    else
      workspace_server(argc, argv, mode);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, cwd) == 0);
    assert(strcmp(locale, setlocale(LC_CTYPE, NULL)) == 0);
    assert(sigaction(SIGINT, NULL, &after) == 0 &&
           before.sa_handler == after.sa_handler);
    free(locale);
    return 0;
  }
  cai_app_status_init(&status);
  open_host(&h, mode);
  assert(cai_app_connect_input(h.app, 42U, &h.input, NULL) == CAI_ERR_INVALID &&
         h.input == NULL);
  assert(cai_app_join(h.app, &code, NULL) == CAI_ERR_INVALID);
  assert(cai_app_start(h.app, NULL, argc - 1, (const char *const *)(argv + 1),
                       NULL) == CAI_OK);
  assert(cai_app_start(h.app, NULL, 0, NULL, NULL) == CAI_ERR_INVALID);
  if (strcmp(mode, "startup_stop") != 0 && strcmp(mode, "replay_stop") != 0 &&
      strncmp(mode, "invalid", 7U) != 0) {
    wait_ready(&h);
    if (strncmp(mode, "input", 5U) == 0)
      input_start(&h);
    else
      control(&h,
              strcmp(mode, "goal") == 0 ? CAI_APP_REPLACE_GOAL
                                        : CAI_APP_QUEUE_PROMPT,
              "CONTROLLER FIRST PROMPT");
    if (strcmp(mode, "observer_error") != 0 &&
        strcmp(mode, "stop_active") != 0) {
      wait_count(&h, strcmp(mode, "queue") == 0 ? 3 : 1, 0);
      if (strncmp(mode, "input", 5U) == 0)
        input_reconnect(&h);
      assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK &&
             status.running);
      if (strcmp(mode, "run") == 0) {
        idle_cpu(&h);
        control(&h, CAI_APP_QUEUE_PROMPT, "CONTROLLER NEXT PROMPT");
        wait_count(&h, 2, 0);
        control(&h, CAI_APP_QUEUE_PROMPT, "/status");
        wait_count(&h, 2, 1);
      }
      control(&h, CAI_APP_STOP, NULL);
    }
  }
  assert(cai_app_join(h.app, &code, NULL) == CAI_OK);
  {
    struct pollfd fd;
    fd.fd = cai_app_status_fd(h.app);
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 0) == 1);
    assert(cai_app_observe_status(h.app, &status, NULL) == CAI_OK &&
           !status.running);
    assert(poll(&fd, 1U, 0) == 0);
  }
  assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK &&
         !status.running && !status.runtime_ready);
  if (strncmp(mode, "invalid", 7U) == 0)
    assert(code == 2 && status.outcome == CAI_APP_FAILED &&
           status.error.code != CAI_OK && status.error.message != NULL);
  else if (strcmp(mode, "observer_error") == 0)
    assert(code == 1 && status.outcome == CAI_APP_FAILED &&
           status.error.code == CAI_ERR_PROTOCOL);
  else {
    assert(code == 0 && status.outcome == CAI_APP_STOPPED);
    if (strcmp(mode, "replay_stop") == 0)
      assert(h.replay == 1 && h.starts == 0 && status.error.code == CAI_OK);
    else if (strcmp(mode, "startup_stop") == 0)
      assert(h.starts == 0 && h.acks == 1 && status.error.code == CAI_OK);
    else if (strcmp(mode, "stop_active") == 0)
      assert(h.starts == 1);
    else
      assert(h.starts ==
                 (strcmp(mode, "run") == 0 || strcmp(mode, "cancel") == 0 ||
                          strcmp(mode, "provider_error") == 0
                      ? 2
                  : strcmp(mode, "queue") == 0 ? 3
                                               : 1) &&
             h.text > 0);
    if (strcmp(mode, "goal") == 0)
      assert(status.goal.has_goal &&
             strcmp(status.goal.status, "complete") == 0);
    if (strcmp(mode, "provider_error") == 0)
      assert(status.error.code == CAI_OK);
  }
  assert(getenv("CAI_CONTROLLER_PRIVATE") == NULL);
  assert(getcwd(current, sizeof(current)) != NULL && strcmp(current, cwd) == 0);
  assert(strcmp(locale, setlocale(LC_CTYPE, NULL)) == 0);
  assert(sigaction(SIGINT, NULL, &after) == 0 &&
         before.sa_handler == after.sa_handler);
  free(locale);
  if (strcmp(mode, "run") == 0) {
    const char *invalid[] = {NULL};
    cai_config_context context;
    memset(&context, 0, sizeof(context));
    context.directory = "relative";
    assert(cai_app_start(h.app, &context, 0, NULL, NULL) == CAI_ERR_INVALID);
    assert(cai_app_start(h.app, NULL, 1, invalid, NULL) == CAI_ERR_INVALID);
    h.mode = "startup_stop";
    h.has_owner = h.starts = h.acks = 0;
    assert(cai_app_start(h.app, NULL, argc - 1, (const char *const *)(argv + 1),
                         NULL) == CAI_OK);
    assert(cai_app_join(h.app, &code, NULL) == CAI_OK && code == 0);
    assert(h.starts == 0 && h.acks == 1);
  }
  cai_app_close(h.app);
  if (h.input != NULL) {
    cai_input_observation *value;
    do {
      assert(cai_input_connection_poll(h.input, &value, NULL) == CAI_OK);
      cai_input_observation_close(value);
    } while (value != NULL);
    cai_input_connection_close(h.input);
  }
  cai_app_status_cleanup(&status);
  pthread_mutex_destroy(&h.lock);
  return 0;
}
