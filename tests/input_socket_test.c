#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/input_socket.h"
#include "../cli/workspace_host.h"
#include <assert.h>
#include <cai/workspace_registry.h>
#include <dirent.h>
#include <fcntl.h>
#include <lc/lc.h>
#include <pthread.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort(); /* A socket frontend/core queue host must not open SDK storage. */
}
typedef struct frontend {
  cai_app_client *client;
  cai_cli_input_socket *queue;
  cai_app_client_state state;
  sl_t *editor;
  int descriptors[2];
  size_t results;
  size_t control_results;
  int control_code;
  size_t binding_rejections;
  int binding_code;
  int paused;
  uint64_t takeover_request;
  cai_input_result result;
} frontend;
typedef struct fixture {
  cai_cli_workspace_host *host;
  cai_input_controller *controller;
  cai_input_queue *queue;
  char *runtime, *workspace;
  unsigned long long clock;
  size_t cancellations;
  size_t selections;
} fixture;
static int observed(void *context, const cai_app_client_event *event,
                    cai_error *error) {
  frontend *f = context;
  f->state = *event->state;
  if (event->type == CAI_APP_CLIENT_BIND_REJECTED) {
    ++f->binding_rejections;
    f->binding_code = event->input->result.code;
  }
  if (event->type == CAI_APP_CLIENT_CONTROL_RESULT) {
    ++f->control_results;
    f->control_code = event->input->result.code;
  } else if (event->input != NULL &&
             event->input->type == CAI_INPUT_OBSERVATION_RESULT) {
    ++f->results;
    f->result = event->input->result;
  }
  return f->queue != NULL && !f->paused
             ? cai_cli_input_socket_observe(f->queue, event, error)
             : CAI_OK;
}
static void server(fixture *f) {
  int stopping;
  assert(cai_input_controller_pump(f->controller, 64U, NULL) == CAI_OK);
  assert(cai_cli_workspace_host_pump(f->host, &stopping, NULL) == CAI_OK &&
         !stopping);
}
static int drive(fixture *f, frontend *a, frontend *b) {
  unsigned int i;
  for (i = 0U; i < 32U; ++i) {
    int rc;
    server(f);
    if (a != NULL && a->client != NULL) {
      cai_error error;
      cai_error_init(&error);
      rc = cai_app_client_pump(a->client, ++f->clock, &error);
      if (rc != CAI_OK) {
        fprintf(stderr, "first client: %d %s\n", rc,
                error.message != NULL ? error.message : "");
        cai_error_cleanup(&error);
        return rc;
      }
      cai_error_cleanup(&error);
      assert(cai_app_client_get_state(a->client, &a->state, NULL) == CAI_OK);
    }
    if (b != NULL && b->client != NULL) {
      cai_error error;
      cai_error_init(&error);
      rc = cai_app_client_pump(b->client, ++f->clock, &error);
      if (rc != CAI_OK) {
        fprintf(stderr, "second client: %d %s\n", rc,
                error.message != NULL ? error.message : "");
        cai_error_cleanup(&error);
        return rc;
      }
      cai_error_cleanup(&error);
      assert(cai_app_client_get_state(b->client, &b->state, NULL) == CAI_OK);
    }
  }
  return CAI_OK;
}
static void connect(fixture *f, frontend *v) {
  cai_app_client_config config;
  cai_app_client_config_init(&config);
  config.runtime_directory = f->runtime;
  config.workspace = f->workspace;
  config.event = observed;
  config.context = v;
  assert(cai_app_client_open(&config, ++f->clock, &v->client, NULL) == CAI_OK);
}
static void reject_instance(fixture *f) {
  cai_app_client_config config;
  cai_app_client *client;
  cai_app_client_state state;
  cai_workspace_registry *registry;
  const cai_workspace_record *record;
  unsigned char expected[16];
  size_t i;
  int rc = CAI_OK;
  assert(cai_workspace_registry_open(f->runtime, &registry, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(registry, &record, NULL) == CAI_OK &&
         record);
  memcpy(expected, record->instance, sizeof(expected));
  expected[15] ^= 1U;
  cai_workspace_registry_close(registry);
  cai_app_client_config_init(&config);
  config.runtime_directory = f->runtime;
  config.workspace = f->workspace;
  config.expected_instance = expected;
  assert(cai_app_client_open(&config, ++f->clock, &client, NULL) == CAI_OK);
  for (i = 0U; i < 32U && rc == CAI_OK; ++i) {
    server(f);
    rc = cai_app_client_pump(client, ++f->clock, NULL);
  }
  assert(rc == CAI_ERR_TRANSPORT);
  assert(cai_app_client_get_state(client, &state, NULL) == CAI_OK);
  assert(state.transport.phase == CAI_WORKSPACE_FAILED &&
         !state.transport.has_welcome);
  cai_app_client_close(client);
}
static void open_frontend(fixture *f, frontend *v, int editor,
                          unsigned long long producer,
                          unsigned long long issued) {
  sl_config_t config;
  memset(v, 0, sizeof(*v));
  v->descriptors[0] = v->descriptors[1] = -1;
  connect(f, v);
  if (editor) {
    assert(cai_cli_input_socket_open(v->client, issued, &v->queue, NULL) ==
           CAI_OK);
    assert(openpty(&v->descriptors[0], &v->descriptors[1], NULL, NULL, NULL) ==
           0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = v->descriptors[1];
    config.line_max_len = 66048U;
    cai_cli_input_adapter_configure(cai_cli_input_socket_adapter(v->queue),
                                    &config);
    v->editor = sl_create_with_config(&config);
    assert(v->editor != NULL &&
           cai_cli_input_adapter_bind(cai_cli_input_socket_adapter(v->queue),
                                      v->editor, NULL) == CAI_OK);
  }
  assert(drive(f, v, NULL) == CAI_OK &&
         v->state.transport.phase == CAI_WORKSPACE_CONNECTED);
  assert(cai_app_client_bind(v->client, 1U, 0U, producer,
                             producer != 0U ? "session" : NULL,
                             NULL) == CAI_OK);
  assert(drive(f, v, NULL) == CAI_OK && v->state.ready);
}
static void close_frontend(frontend *v) {
  cai_cli_input_socket *queue = v->queue;
  v->queue = NULL; /* Stop routing before destroying its borrowed editor. */
  if (v->editor != NULL)
    sl_destroy(v->editor);
  cai_cli_input_socket_close(queue);
  cai_app_client_close(v->client);
  if (v->descriptors[0] >= 0)
    close(v->descriptors[0]);
  if (v->descriptors[1] >= 0)
    close(v->descriptors[1]);
  v->client = NULL;
  v->editor = NULL;
}
static size_t count(fixture *f) {
  cai_input_state state;
  assert(cai_input_queue_get_state(f->queue, &state, NULL) == CAI_OK);
  return state.count;
}
static sl_queue_request_id_t request(frontend *v,
                                     sl_queue_operation_t operation,
                                     const char *text, size_t index,
                                     sl_queue_request_id_t target) {
  sl_queue_request_t r;
  sl_queue_request_id_t id;
  sl_queue_entry_t entry;
  int rc;
  memset(&r, 0, sizeof(r));
  r.operation = operation;
  r.mode = SL_PROMPT_QUEUE_MODE_STEER;
  r.text = text;
  r.enabled = 1;
  r.target_request_id = target;
  if (operation == SL_QUEUE_BEGIN_EDIT || operation == SL_QUEUE_END_EDIT) {
    assert(sl_queue_backend_entry(v->editor, index, &entry) == SL_OK);
    r.item_id = entry.id;
    r.item_revision = entry.revision;
    r.edit_token = entry.edit_token;
  }
  rc = sl_queue_backend_request(v->editor, &r, &id);
  if (rc != SL_OK) {
    const cai_error *failed = cai_cli_input_socket_error(v->queue);
    fprintf(stderr, "editor operation %d failed: %d %s; socket: %d %s\n",
            (int)operation, rc, sl_last_error(v->editor), failed->code,
            failed->message != NULL ? failed->message : "no detail");
  }
  assert(rc == SL_OK);
  return id;
}
static void takeover(fixture *f, frontend *v) {
  assert(cai_app_client_takeover(v->client, 80U + ++v->takeover_request,
                                 v->state.transport.generation,
                                 NULL) == CAI_OK);
  assert(drive(f, v, NULL) == CAI_OK &&
         v->state.transport.controller == v->state.transport.attachment);
}
static int cancelled(void *context, cai_error *error) {
  fixture *f = context;
  (void)error;
  ++f->cancellations;
  return CAI_OK;
}
static void cancellation_cases(fixture *f) {
  frontend a, b;
  unsigned long long old_generation;
  size_t results;
  int wake[2];
  char byte;
  cai_cli_input_socket *startup = NULL;
  assert(cai_cli_input_socket_open(NULL, 0U, &startup, NULL) == CAI_OK);
  cai_cli_input_socket_close(startup);
  cai_cli_workspace_host_cancel(f->host, cancelled, f);
  open_frontend(f, &a, 1, 0U, 0U);
  assert(pipe(wake) == 0 && fcntl(wake[0], F_SETFL, O_NONBLOCK) == 0);
  cai_cli_input_socket_set_wakeup(a.queue, wake[1]);
  assert(request(&a, SL_QUEUE_RESUME, NULL, 0U, 0U) == 1U);
  assert(read(wake[0], &byte, 1U) == 1);
  cai_cli_input_socket_set_wakeup(a.queue, -1);
  close(wake[0]);
  close(wake[1]);
  assert(drive(f, &a, NULL) == CAI_OK);
  results = a.results;
  old_generation = a.state.transport.generation;
  assert(cai_app_client_cancel(a.client, 1U, old_generation, NULL) == CAI_OK);
  assert(drive(f, &a, NULL) == CAI_OK && a.control_results == 1U &&
         a.control_code == CAI_OK && f->cancellations == 1U);
  assert(a.state.last_request == 1U && a.results == results && count(f) == 0U);
  /* Exact repeats acknowledge the first effect; they cannot cancel a later
   * turn. Control identities never consume queue producer watermarks. */
  assert(cai_app_client_cancel(a.client, 1U, old_generation, NULL) == CAI_OK);
  assert(drive(f, &a, NULL) == CAI_OK && a.control_results == 2U &&
         a.control_code == CAI_OK && f->cancellations == 1U);
  open_frontend(f, &b, 1, 0U, 0U);
  assert(cai_app_client_cancel(b.client, 1U, b.state.transport.generation,
                               NULL) == CAI_ERR_INVALID);
  assert(drive(f, &a, &b) == CAI_OK && b.control_results == 0U &&
         f->cancellations == 1U);
  takeover(f, &b);
  assert(drive(f, &a, &b) == CAI_OK);
  assert(cai_app_client_cancel(a.client, 2U, old_generation, NULL) ==
         CAI_ERR_INVALID);
  assert(drive(f, &a, &b) == CAI_OK && a.control_results == 2U &&
         f->cancellations == 1U);
  assert(cai_app_client_cancel(b.client, 2U, b.state.transport.generation,
                               NULL) == CAI_OK);
  assert(drive(f, &a, &b) == CAI_OK && b.control_code == CAI_OK &&
         f->cancellations == 2U);
  cai_cli_workspace_host_cancel(f->host, NULL, NULL);
  assert(cai_app_client_cancel(b.client, 3U, b.state.transport.generation,
                               NULL) == CAI_OK);
  assert(drive(f, &a, &b) == CAI_OK && b.control_code != CAI_OK &&
         f->cancellations == 2U);
  close_frontend(&a);
  close_frontend(&b);
  assert(drive(f, NULL, NULL) == CAI_OK);
}
static void cases(fixture *f) {
  frontend *a = calloc(1U, sizeof(*a)), *b = calloc(1U, sizeof(*b));
  cai_input_request seed;
  cai_input_scope scope;
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  sl_queue_entry_t entry;
  sl_queue_request_id_t lost;
  unsigned long long producer;
  unsigned int i;
  assert(a != NULL && b != NULL);
  open_frontend(f, a, 0, 0U, 0U);
  producer = a->state.producer;
  memset(&seed, 0, sizeof(seed));
  seed.operation = CAI_INPUT_OFFER;
  seed.request_id = 37U;
  seed.text = "retained backend prompt";
  seed.enabled = 1;
  scope.conversation = a->state.conversation;
  scope.generation = a->state.transport.generation;
  assert(cai_app_client_request(a->client, &scope, &seed, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 37U &&
         count(f) == 1U);
  close_frontend(a);
  assert(drive(f, NULL, NULL) == CAI_OK);
  /* A recreated editor must preserve its own issued watermark too, even when
   * it is ahead of the last processed backend receipt. */
  open_frontend(f, a, 1, producer, 50U);
  takeover(f, a);
  {
    cai_app_client_event event;
    cai_app_client_state stale = a->state;
    sl_queue_backend_info_t before;
    char *taken = NULL;
    assert(sl_queue_backend_info(a->editor, &before) == SL_OK);
    memset(&event, 0, sizeof(event));
    event.type = CAI_APP_CLIENT_CONNECTION;
    stale.transport.attachment ^= 1U;
    event.state = &stale;
    assert(cai_cli_input_socket_observe(a->queue, &event, NULL) ==
           CAI_ERR_INVALID);
    assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
           info.synchronized && info.visible_count == before.visible_count);
    assert(sl_prompt_queue_take(a->editor, 0U, &taken) ==
               SL_ERROR_UNSUPPORTED &&
           taken == NULL);
  }
  assert(sl_set_buffer(a->editor, "independent local draft") == SL_OK);
  assert(request(a, SL_QUEUE_OFFER, "remote steer", 0U, 0U) == 1U);
  assert(count(f) == 1U); /* Callback hands off; never runs backend work. */
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         info.pending_count == 1U);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 51U &&
         count(f) == 2U);
  assert(cai_cli_input_socket_last_issued(a->queue) == 51U);
  assert(sl_queue_backend_entry(a->editor, 1U, &entry) == SL_OK &&
         entry.mode == SL_PROMPT_QUEUE_MODE_STEER && entry.held);
  /* Native approval/edit actions retain the oldest-held-item policy. */
  assert(request(a, SL_QUEUE_BEGIN_EDIT, NULL, 0U, 0U) == 2U);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 52U);
  assert(sl_queue_backend_entry(a->editor, 0U, &entry) == SL_OK &&
         entry.edit_held);
  assert(request(a, SL_QUEUE_END_EDIT, "edited remotely", 0U, 0U) == 3U);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 53U);
  assert(sl_queue_backend_entry(a->editor, 0U, &entry) == SL_OK &&
         !entry.edit_held && strcmp(entry.text, "edited remotely") == 0);
  /* A second real editor can observe, but cannot mutate without takeover. */
  open_frontend(f, b, 1, 0U, 0U);
  assert(request(b, SL_QUEUE_OFFER, "observer proposal", 0U, 0U) == 1U);
  assert(drive(f, a, b) == CAI_OK && b->result.outcome == CAI_INPUT_REJECTED &&
         count(f) == 2U);
  assert(sl_queue_backend_pending(b->editor, 0U, &pending) == SL_OK &&
         !pending.pending && pending.status == SL_QUEUE_REJECTED);
  assert(sl_queue_backend_recover(b->editor, 1U) == SL_OK &&
         strcmp(sl_buffer(b->editor), "observer proposal") == 0);
  close_frontend(b);
  assert(drive(f, a, NULL) == CAI_OK);
  /* The server accepts this, but this editor never reads the receipt. */
  lost = request(a, SL_QUEUE_OFFER, "accepted before lost receipt", 0U, 0U);
  assert(lost == 4U &&
         cai_app_client_pump(a->client, ++f->clock, NULL) == CAI_OK);
  for (i = 0U; i < 32U; ++i)
    server(f);
  assert(count(f) == 3U && a->state.last_request == 53U);
  assert(cai_cli_input_socket_disconnect(a->queue, NULL) == CAI_OK);
  cai_app_client_close(a->client);
  a->client = NULL;
  assert(drive(f, NULL, NULL) == CAI_OK);
  assert(sl_queue_backend_pending(a->editor, 0U, &pending) == SL_OK &&
         !pending.pending && pending.status == SL_QUEUE_UNKNOWN);
  assert(strcmp(sl_buffer(a->editor), "independent local draft") == 0);
  connect(f, a);
  assert(cai_cli_input_socket_reconnect(a->queue, a->client, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_app_client_bind(a->client, 1U, 0U, producer, "session", NULL) ==
         CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 54U);
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         info.synchronized && info.visible_count == 3U);
  assert(request(a, SL_QUEUE_RESOLVE, NULL, 0U, lost) == 5U);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 55U &&
         count(f) == 3U);
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         !info.pending_count);
  takeover(f, a);
  assert(request(a, SL_QUEUE_OFFER, "after reconnect", 0U, 0U) == 6U);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 56U &&
         count(f) == 4U);
  assert(strcmp(sl_buffer(a->editor), "independent local draft") == 0);
  /* Lose an edit acquisition's receipt, then reopen the same logical queue
   * under a new runtime epoch. Item identities remain stable for RESOLVE;
   * original request epochs are never silently upgraded or resubmitted. */
  {
    unsigned long long old_epoch = a->state.conversation;
    lost = request(a, SL_QUEUE_BEGIN_EDIT, NULL, 0U, 0U);
    assert(lost == 7U);
    assert(cai_app_client_pump(a->client, ++f->clock, NULL) == CAI_OK);
    for (i = 0U; i < 32U; ++i)
      server(f);
    assert(cai_cli_input_socket_disconnect(a->queue, NULL) == CAI_OK);
    cai_app_client_close(a->client);
    a->client = NULL;
    assert(drive(f, NULL, NULL) == CAI_OK);
    assert(cai_cli_workspace_host_input(f->host, NULL, NULL, NULL) == CAI_OK);
    assert(cai_input_controller_select(f->controller, NULL, NULL) == CAI_OK);
    assert(cai_input_controller_select(f->controller, f->queue, NULL) ==
           CAI_OK);
    assert(cai_cli_workspace_host_input(f->host, f->controller, "session",
                                        NULL) == CAI_OK);
    connect(f, a);
    assert(cai_cli_input_socket_reconnect(a->queue, a->client, NULL) == CAI_OK);
    assert(drive(f, a, NULL) == CAI_OK);
    assert(cai_app_client_bind(a->client, 1U, 0U, producer, "session", NULL) ==
           CAI_OK);
    assert(drive(f, a, NULL) == CAI_OK && a->state.conversation != old_epoch);
    assert(request(a, SL_QUEUE_RESOLVE, NULL, 0U, lost) == 8U);
    assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 58U &&
           count(f) == 4U);
    assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
           !info.pending_count);
    assert(sl_queue_backend_entry(a->editor, 0U, &entry) == SL_OK &&
           !entry.edit_held && entry.edit_token == NULL);
    assert(strcmp(sl_buffer(a->editor), "independent local draft") == 0);
  }
  /* A replacement binding cannot silently redirect this editor to another
   * producer, even in the same persistent session. */
  assert(cai_cli_input_socket_disconnect(a->queue, NULL) == CAI_OK);
  cai_app_client_close(a->client);
  a->client = NULL;
  assert(drive(f, NULL, NULL) == CAI_OK);
  connect(f, a);
  assert(cai_cli_input_socket_reconnect(a->queue, a->client, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_app_client_bind(a->client, 1U, 0U, 0U, NULL, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_input_socket_disconnect(a->queue, NULL) == CAI_OK);
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         !info.synchronized && info.visible_count == 4U && count(f) == 4U);
  close_frontend(a);
  assert(drive(f, NULL, NULL) == CAI_OK);
  /* No wrap or narrowing of backend IDs, including 32-bit Softline handles. */
  open_frontend(f, a, 1, 0U, ~(unsigned long long)0 - 1U);
  takeover(f, a);
  assert(request(a, SL_QUEUE_OFFER, "maximum backend ID", 0U, 0U) == 1U);
  assert(drive(f, a, NULL) == CAI_OK &&
         a->state.last_request == ~(unsigned long long)0);
  {
    sl_queue_request_t r;
    sl_queue_request_id_t id;
    memset(&r, 0, sizeof(r));
    r.operation = SL_QUEUE_OFFER;
    r.text = "must not wrap";
    assert(sl_queue_backend_request(a->editor, &r, &id) == SL_ERROR_IO);
    assert(cai_cli_input_socket_error(a->queue)->code == CAI_ERR_INVALID);
  }
  assert(count(f) == 5U);
  close_frontend(a);
  assert(drive(f, NULL, NULL) == CAI_OK);
  /* A host bypassing this bridge cannot make it recycle old editor IDs or
   * silently shift unresolved request mappings onto another receipt. */
  open_frontend(f, a, 1, 0U, 0U);
  takeover(f, a);
  memset(&seed, 0, sizeof(seed));
  seed.operation = CAI_INPUT_OFFER;
  seed.request_id = 9U;
  seed.text = "external producer use";
  seed.enabled = 1;
  scope.conversation = a->state.conversation;
  scope.generation = a->state.transport.generation;
  assert(cai_app_client_request(a->client, &scope, &seed, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK && a->state.last_request == 9U);
  {
    sl_queue_request_t r;
    sl_queue_request_id_t id;
    memset(&r, 0, sizeof(r));
    r.operation = SL_QUEUE_OFFER;
    r.text = "must not reuse another request identity";
    assert(sl_queue_backend_request(a->editor, &r, &id) == SL_ERROR_IO);
    assert(strstr(cai_cli_input_socket_error(a->queue)->message,
                  "outside this editor") != NULL);
  }
  assert(count(f) == 6U);
  close_frontend(a);
  free(a);
  free(b);
}
static void pause_cases(fixture *f) {
  frontend *a = calloc(1U, sizeof(*a)), *b = calloc(1U, sizeof(*b));
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  size_t before = count(f);
  assert(a != NULL && b != NULL);
  open_frontend(f, a, 1, 0U, 0U);
  takeover(f, a);
  open_frontend(f, b, 0, 0U, 0U);
  assert(sl_set_buffer(a->editor, "paused local draft") == SL_OK);
  assert(request(a, SL_QUEUE_OFFER, "accepted while observations pause", 0U,
                 0U) == 1U);
  assert(cai_cli_input_socket_pause(a->queue, NULL) == CAI_OK);
  assert(cai_cli_input_socket_pause(a->queue, NULL) == CAI_ERR_INVALID);
  a->paused = 1;
  assert(drive(f, a, b) == CAI_OK && count(f) == before + 1U);
  assert(sl_queue_backend_pending(a->editor, 0U, &pending) == SL_OK &&
         !pending.pending && pending.status == SL_QUEUE_UNKNOWN);
  assert(cai_cli_input_socket_resume(a->queue, b->client, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_cli_input_socket_reconnect(a->queue, a->client, NULL) ==
         CAI_ERR_INVALID);
  takeover(f, b); /* Authority changes; the original producer remains bound. */
  assert(cai_cli_input_socket_resume(a->queue, a->client, NULL) == CAI_OK);
  assert(cai_cli_input_socket_resume(a->queue, a->client, NULL) ==
         CAI_ERR_INVALID);
  a->paused = 0;
  assert(request(a, SL_QUEUE_SYNCHRONIZE, NULL, 0U, 0U) == 2U);
  assert(drive(f, a, b) == CAI_OK);
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         info.synchronized && info.visible_count == before + 1U);
  assert(request(a, SL_QUEUE_RESOLVE, NULL, 0U, 1U) == 3U);
  assert(drive(f, a, b) == CAI_OK && count(f) == before + 1U);
  assert(sl_queue_backend_info(a->editor, &info) == SL_OK &&
         !info.pending_count);
  assert(strcmp(sl_buffer(a->editor), "paused local draft") == 0);
  assert(request(a, SL_QUEUE_OFFER, "must remain an observer", 0U, 0U) == 4U);
  assert(drive(f, a, b) == CAI_OK && a->result.outcome == CAI_INPUT_REJECTED &&
         count(f) == before + 1U);
  close_frontend(a);
  close_frontend(b);
  free(a);
  free(b);
}
static void settlement_cases(fixture *f) {
  frontend *a = calloc(1U, sizeof(*a));
  frontend *b = calloc(1U, sizeof(*b));
  cai_input_scope scope;
  sl_queue_pending_info_t pending;
  cai_input_state before, after;
  struct pollfd descriptor;
  uint64_t deadline;
  int ready;
  assert(a != NULL && b != NULL);
  open_frontend(f, a, 1, 0U, 0U);
  takeover(f, a);
  assert(cai_app_client_manage_delivery(a->client, NULL) == CAI_ERR_INVALID);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  scope.conversation = a->state.conversation;
  scope.generation = a->state.transport.generation;
  assert(cai_app_client_settled(a->client, &scope, a->state.last_request + 1U,
                                NULL) == CAI_ERR_INVALID);
  assert(cai_input_queue_get_state(f->queue, &before, NULL) == CAI_OK);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  assert(cai_input_queue_get_state(f->queue, &after, NULL) == CAI_OK &&
         before.revision == after.revision &&
         before.view_revision == after.view_revision);
  assert(sl_set_buffer(a->editor, "a private draft must not reach the wire") ==
         SL_OK);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(cai_app_client_prepare(a->client, &descriptor, &deadline, NULL) ==
         CAI_OK);
  assert(!(descriptor.events & POLLOUT));
  assert(request(a, SL_QUEUE_OFFER, "a new gated item", 0U, 0U) == 1U);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  /* Every managed producer rearms after a role change, including an observer
   * that may still edit its own queries. It settles through the same idle
   * handoff without reconnecting or offering another item. */
  open_frontend(f, b, 1, 0U, 0U);
  takeover(f, b);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, b->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  takeover(f, a);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  assert(request(a, SL_QUEUE_OFFER, "offer after takeover", 0U, 0U) == 2U);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         !ready);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  /* Force TAKEOVER and its valid settlement into the same server pump. The
   * later role bookkeeping must preserve an already-current notification. */
  takeover(f, b);
  assert(drive(f, a, b) == CAI_OK);
  scope.conversation = a->state.conversation;
  scope.generation = a->state.transport.generation + 1U;
  assert(cai_app_client_takeover(a->client, 80U + ++a->takeover_request,
                                 a->state.transport.generation,
                                 NULL) == CAI_OK);
  assert(cai_app_client_settled(a->client, &scope, a->state.last_request,
                                NULL) == CAI_OK);
  assert(drive(f, a, b) == CAI_OK);
  assert(a->state.transport.generation == scope.generation);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  /* Receipt loss must be resolved before settlement. An ordinary local draft
   * never substitutes for that protocol decision or reaches the socket. */
  assert(request(a, SL_QUEUE_OFFER, "a receipt to reconcile", 0U, 0U) == 3U);
  assert(cai_cli_input_socket_pause(a->queue, NULL) == CAI_OK);
  a->paused = 1;
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_cli_input_socket_resume(a->queue, a->client, NULL) == CAI_OK);
  a->paused = 0;
  assert(request(a, SL_QUEUE_SYNCHRONIZE, NULL, 0U, 0U) == 4U);
  assert(drive(f, a, b) == CAI_OK);
  assert(sl_queue_backend_pending(a->editor, 0U, &pending) == SL_OK &&
         pending.status == SL_QUEUE_UNKNOWN);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(cai_app_client_prepare(a->client, &descriptor, &deadline, NULL) ==
             CAI_OK &&
         !(descriptor.events & POLLOUT));
  assert(request(a, SL_QUEUE_RESOLVE, NULL, 0U, 3U) == 5U);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_cli_input_socket_idle(a->queue, a->editor, NULL) == CAI_OK);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_input_controller_producer_ready(f->controller, a->state.producer,
                                             &ready, NULL) == CAI_OK &&
         ready);
  close_frontend(a);
  close_frontend(b);
  free(a);
  free(b);
}
static int select_conversation(void *context, const char *selector,
                               cai_error *error) {
  fixture *f = context;
  (void)error;
  ++f->selections;
  return selector == NULL || strcmp(selector, "1") == 0 ? CAI_OK
                                                        : CAI_ERR_INVALID;
}
/** Selection is setup, not an input offer. A failed/observer selection must
 * not bind a producer, fall back to the current session, or enqueue text.
 */
static void selection_cases(fixture *f) {
  frontend *a = calloc(1U, sizeof(*a)), *b = calloc(1U, sizeof(*b));
  assert(a != NULL && b != NULL);
  a->descriptors[0] = a->descriptors[1] = -1;
  b->descriptors[0] = b->descriptors[1] = -1;
  cai_cli_workspace_host_selection(f->host, select_conversation, f);
  connect(f, a);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_app_client_select(a->client, 1U, "", NULL) == CAI_ERR_INVALID);
  assert(cai_app_client_select(a->client, 1U, "missing", NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(f->selections == 1U && a->binding_rejections == 1U &&
         a->binding_code == CAI_ERR_INVALID && !a->state.bound);
  assert(cai_app_client_select(a->client, 2U, "1", NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK && a->state.ready);
  assert(f->selections == 2U && strcmp(a->state.session, "session") == 0 &&
         a->state.last_request == 0U && count(f) == 0U);
  assert(cai_app_client_select(a->client, 3U, NULL, NULL) == CAI_ERR_INVALID);
  connect(f, b);
  assert(drive(f, a, b) == CAI_OK);
  assert(cai_app_client_select(b->client, 1U, NULL, NULL) == CAI_OK);
  assert(drive(f, a, b) == CAI_OK);
  assert(b->binding_rejections == 1U && !b->state.bound &&
         f->selections == 2U && count(f) == 0U);
  close_frontend(a);
  close_frontend(b);
  server(f);
  memset(a, 0, sizeof(*a));
  a->descriptors[0] = a->descriptors[1] = -1;
  connect(f, a);
  assert(drive(f, a, NULL) == CAI_OK);
  assert(cai_app_client_select(a->client, 1U, NULL, NULL) == CAI_OK);
  assert(drive(f, a, NULL) == CAI_OK && a->state.ready && f->selections == 3U &&
         count(f) == 0U);
  close_frontend(a);
  cai_cli_workspace_host_selection(f->host, NULL, NULL);
  free(b);
  free(a);
}
static void *run(void *unused) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_attachment_config lifetime;
  cai_input_queue_config config;
  cai_input_controller_config admission;
  char root[] = "./iq.XXXXXX";
  DIR *directory;
  struct dirent *entry;
  (void)unused;
  assert(f != NULL && mkdtemp(root) != NULL);
  f->runtime = realpath(root, NULL);
  f->workspace = realpath(".", NULL);
  assert(f->runtime != NULL && f->workspace != NULL);
  cai_input_queue_config_init(&config);
  assert(cai_input_queue_open(&config, &f->queue, NULL) == CAI_OK);
  cai_attachment_config_init(&lifetime);
  lifetime.persist = 1;
  assert(cai_cli_workspace_host_open(f->runtime, f->workspace, NULL, NULL, 0U,
                                     &lifetime, NULL, NULL, NULL, &f->host,
                                     NULL) == CAI_OK);
  memset(&admission, 0, sizeof(admission));
  cai_cli_workspace_host_admission(f->host, &admission);
  assert(cai_input_controller_open(&admission, &f->controller, NULL) == CAI_OK);
  assert(cai_input_controller_select(f->controller, f->queue, NULL) == CAI_OK);
  assert(cai_cli_workspace_host_input(f->host, f->controller, "session",
                                      NULL) == CAI_OK);
  assert(cai_cli_workspace_host_ready(f->host, NULL) == CAI_OK);
  reject_instance(f);
  selection_cases(f);
  cancellation_cases(f);
  cases(f);
  pause_cases(f);
  settlement_cases(f);
  {
    frontend *a = calloc(1U, sizeof(*a));
    frontend *b = calloc(1U, sizeof(*b));
    cai_input_scope original;
    size_t queued = count(f);
    assert(a != NULL && b != NULL);
    open_frontend(f, a, 1, 0U, 0U);
    takeover(f, a);
    original.conversation = a->state.conversation;
    original.generation = a->state.transport.generation;
    open_frontend(f, b, 1, 0U, 0U);
    takeover(f, b);
    assert(drive(f, a, b) == CAI_OK);
    takeover(f, a);
    assert(drive(f, a, b) == CAI_OK);
    /* An accepted TCP receipt keeps its old generation even if the same
     * attachment regains control before its callback-driven OFFER is sent. */
    cai_cli_input_socket_offer_scope(a->queue, &original);
    (void)request(a, SL_QUEUE_OFFER, "stale external receipt", 0U, 0U);
    assert(drive(f, a, b) == CAI_OK &&
           a->result.outcome == CAI_INPUT_REJECTED && count(f) == queued);
    (void)request(a, SL_QUEUE_OFFER, "fresh local offer", 0U, 0U);
    assert(drive(f, a, b) == CAI_OK &&
           a->result.outcome == CAI_INPUT_ACCEPTED && count(f) == queued + 1U);
    close_frontend(a);
    close_frontend(b);
    free(a);
    free(b);
    server(f);
  }
  assert(cai_cli_workspace_host_input(f->host, NULL, NULL, NULL) == CAI_OK);
  cai_cli_workspace_host_close(f->host);
  cai_input_controller_close(f->controller);
  cai_input_queue_close(f->queue);
  directory = opendir(f->runtime);
  assert(directory != NULL);
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.') {
      char path[512];
      assert(snprintf(path, sizeof(path), "%s/%s", f->runtime, entry->d_name) >
             0);
      assert(unlink(path) == 0);
    }
  assert(closedir(directory) == 0 && rmdir(f->runtime) == 0);
  free(f->runtime);
  free(f->workspace);
  free(f);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
