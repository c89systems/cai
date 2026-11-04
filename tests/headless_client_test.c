/* Drives the actual standalone server through installed public client APIs.
 * No runtime/store is allowed in this frontend process. */
#define _POSIX_C_SOURCE 200809L
#include "../cli/transcript_presenter.h"
#include <assert.h>
#include <cai/app_client.h>
#include <cai/workspace_discovery.h>
#include <cai/workspace_registry.h>
#include <errno.h>
#include <lc/lc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static char rendered[4096];
static size_t rendered_length;
int __wrap_sl_output_stream_write(sl_t *, const char *, size_t);
int __wrap_sl_output_stream_write(sl_t *editor, const char *bytes,
                                  size_t length) {
  (void)editor;
  assert(length < sizeof(rendered) - rendered_length);
  memcpy(rendered + rendered_length, bytes, length);
  rendered_length += length;
  rendered[rendered_length] = '\0';
  return SL_OK;
}
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
typedef struct view {
  cai_app_client *client;
  cai_app_client_state state;
  cai_error error;
  uint64_t until;
  size_t matched;
  size_t statuses;
  size_t execution_chunks;
  int reading, found, done, finished, accepted, closing, replayed;
  cai_cli_presenter presenter;
  cai_cli_transcript_presenter *presentation;
} view;
static uint64_t now_ms(void) {
  struct timespec clock;
  assert(clock_gettime(CLOCK_MONOTONIC, &clock) == 0);
  return (uint64_t)clock.tv_sec * 1000U + (uint64_t)clock.tv_nsec / 1000000U;
}
static int event(void *context, const cai_app_client_event *e,
                 cai_error *error) {
  view *v = context;
  (void)error;
  if (e->type == CAI_APP_CLIENT_STATUS) {
    const cai_app_view *status = NULL;
    assert(cai_app_client_get_view(v->client, &status, NULL) == CAI_OK &&
           status == e->status && status->available && !status->stale);
    assert(status->runtime_ready && status->session_id != NULL &&
           status->effective.model != NULL && status->provider != NULL);
    assert(status->log_path == NULL || status->log_path[0] == '/');
    ++v->statuses;
  }
  if (e->type == CAI_APP_CLIENT_INPUT && e->input != NULL &&
      e->input->type == CAI_INPUT_OBSERVATION_RESULT) {
    assert(e->input->result.outcome == CAI_INPUT_ACCEPTED);
    v->accepted = 1;
  }
  return CAI_OK;
}
static int transcript(void *context, const cai_transcript_event *e,
                      cai_error *error) {
  static const char answer[] = "HEADLESS_ANSWER",
                    completed[] = "turn.completed";
  view *v = context;
  const char *bytes = e->data;
  size_t i;
  const char *wanted;
  if (v->presentation != NULL) {
    int rc = cai_cli_transcript_presenter_event(v->presentation, e, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (e->kind == CAI_TRANSCRIPT_REPLAY_END)
    v->replayed = 1;
  if (e->kind == CAI_TRANSCRIPT_ERROR)
    assert(v->closing);
  if (e->kind == CAI_TRANSCRIPT_EVENT_BEGIN) {
    v->reading = strcmp(e->type, "assistant_text_delta") == 0 ? 1
                 : strcmp(e->type, "execution") == 0          ? 2
                                                              : 0;
    v->matched = 0U;
    v->found = 0;
  } else if (e->kind == CAI_TRANSCRIPT_DATA && v->reading) {
    wanted = v->reading == 1 ? answer : completed;
    for (i = 0U; i < e->length && !v->found; ++i) {
      if (bytes[i] == wanted[v->matched])
        ++v->matched;
      else
        v->matched = bytes[i] == wanted[0] ? 1U : 0U;
      if (wanted[v->matched] == '\0')
        v->found = 1;
    }
  } else if (e->kind == CAI_TRANSCRIPT_EVENT_END && v->found) {
    if (v->reading == 1)
      v->done = 1;
    else
      v->finished = 1;
  } /* Only authenticated completed entries satisfy the probe. */
  return CAI_OK;
}
static int execution(void *context, const cai_app_execution_event *e,
                     cai_error *error) {
  view *v = context;
  cai_app_client_state state;
  (void)error;
  assert(e->record != NULL && e->record->kind != NULL &&
         e->record->session_id != NULL && e->boundary != NULL);
  assert(cai_app_client_get_state(v->client, &state, NULL) == CAI_OK &&
         state.transcript.position < e->boundary->position);
  ++v->execution_chunks;
  return CAI_OK;
}
static void step(view *v) {
  struct pollfd fd;
  uint64_t deadline, now = now_ms();
  int rc;
  assert(now < v->until);
  assert(cai_app_client_prepare(v->client, &fd, &deadline, NULL) == CAI_OK &&
         fd.fd >= 0);
  if (deadline == 0U || deadline > v->until)
    deadline = v->until;
  rc = poll(&fd, 1U, deadline > now ? (int)(deadline - now) : 0);
  assert(rc >= 0 || errno == EINTR);
  rc = cai_app_client_pump(v->client, now_ms(), &v->error);
  if (rc != CAI_OK)
    fprintf(stderr, "%s\n", v->error.message);
  assert(rc == CAI_OK);
  assert(cai_app_client_get_state(v->client, &v->state, NULL) == CAI_OK);
}
static void discover(const char *runtime, const char *workspace,
                     const char *name, unsigned char instance[16]) {
  cai_workspace_discovery_config config;
  cai_workspace_discovery_state state;
  cai_workspace_discovery *query;
  const cai_workspace_discovery_result *result;
  struct pollfd fd;
  uint64_t deadline, now, until = now_ms() + 10000U;
  cai_workspace_discovery_config_init(&config);
  config.runtime_directory = runtime;
  config.selector = name ? name : workspace;
  assert(cai_workspace_discovery_open(&config, now_ms(), &query, NULL) ==
         CAI_OK);
  do {
    assert(cai_workspace_discovery_prepare(query, &fd, &deadline, NULL) ==
           CAI_OK);
    now = now_ms();
    assert(now < until && deadline != 0U);
    if (deadline > until)
      deadline = until;
    assert(poll(&fd, 1U, deadline > now ? (int)(deadline - now) : 0) >= 0 ||
           errno == EINTR);
    assert(cai_workspace_discovery_pump(query, now_ms(), NULL) == CAI_OK);
    assert(cai_workspace_discovery_get_state(query, &state, NULL) == CAI_OK);
  } while (state.phase == CAI_WORKSPACE_DISCOVERING);
  assert(state.phase == CAI_WORKSPACE_FOUND);
  result = cai_workspace_discovery_get_result(query);
  assert(result && strcmp(result->record.workspace, workspace) == 0);
  if (name)
    assert(strcmp(result->record.name, name) == 0);
  memcpy(instance, result->record.instance, 16U);
  cai_workspace_discovery_close(query);
}
static void identify(const char *runtime, const char *workspace,
                     const char *name) {
  cai_workspace_client_config config;
  cai_workspace_client_state state;
  cai_workspace_client *client;
  struct pollfd fd;
  uint64_t deadline, now;
  cai_workspace_registry *registry;
  const cai_workspace_record *record;
  size_t matches = 0U;
  unsigned char instance[16];
  discover(runtime, workspace, name, instance);
  cai_workspace_client_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.expected_instance = instance;
  config.probe = 1;
  config.handshake_timeout_ms = 10000U;
  assert(cai_workspace_client_open(&config, now_ms(), &client, NULL) == CAI_OK);
  do {
    assert(cai_workspace_client_prepare(client, &fd, &deadline, NULL) ==
           CAI_OK);
    now = now_ms();
    assert(fd.fd >= 0 && deadline > now);
    assert(poll(&fd, 1U, (int)(deadline - now)) >= 0 || errno == EINTR);
    assert(cai_workspace_client_pump(client, now_ms(), NULL) == CAI_OK);
    assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
  } while (state.phase != CAI_WORKSPACE_IDENTIFIED);
  assert(state.has_identity && !state.has_welcome && !state.attachment &&
         !state.generation && !state.controller && state.persist &&
         state.server_phase == CAI_ATTACHMENT_READY);
  assert(cai_workspace_client_prepare(client, &fd, &deadline, NULL) == CAI_OK);
  assert(fd.fd == -1 && !deadline);
  assert(cai_workspace_registry_open(runtime, &registry, NULL) == CAI_OK);
  do {
    assert(cai_workspace_registry_next(registry, &record, NULL) == CAI_OK);
    if (record && strcmp(record->workspace,
                         cai_workspace_client_directory(client)) == 0) {
      ++matches;
      assert(memcmp(record->instance, state.instance, sizeof(state.instance)) ==
             0);
      if (name)
        assert(strcmp(record->name, name) == 0);
    }
  } while (record);
  assert(matches == 1U);
  cai_workspace_registry_close(registry);
  cai_workspace_client_close(client);
}
int main(int argc, char **argv) {
  cai_app_client_config config;
  sl_t *editor = NULL;
  view *v = calloc(1U, sizeof(*v));
  unsigned char expected[16];
  int shutdown, work, abrupt, ephemeral, render;
  assert(argc == 4 && v != NULL);
  if (strcmp(argv[3], "probe") == 0 || strcmp(argv[3], "named") == 0) {
    identify(argv[1], argv[2],
             strcmp(argv[3], "named") == 0 ? "fixture-workspace" : NULL);
    free(v);
    return 0;
  }
  shutdown = strcmp(argv[3], "shutdown") == 0;
  work = strcmp(argv[3], "work") == 0;
  render = strcmp(argv[3], "render") == 0;
  abrupt = strcmp(argv[3], "abrupt") == 0;
  ephemeral = strcmp(argv[3], "ephemeral") == 0;
  assert(shutdown || work || render || abrupt || ephemeral ||
         strcmp(argv[3], "inspect") == 0);
  if (work || render) {
    editor = sl_create();
    assert(editor != NULL);
    v->presenter.sl = editor;
    v->presenter.interactive = 1;
    assert(cai_cli_theme_parse("plain", &v->presenter.theme) == NULL);
    v->presenter.render_theme = v->presenter.theme;
    assert(cai_cli_presenter_renderer_create(
               &v->presenter, &v->presenter.render_theme,
               &v->presenter.renderer, NULL) == CAI_OK);
    assert(cai_cli_transcript_presenter_open(&v->presenter, &v->presentation,
                                             NULL) == CAI_OK);
  }
  cai_error_init(&v->error);
  cai_app_client_config_init(&config);
  config.runtime_directory = argv[1];
  config.workspace = argv[2];
  discover(argv[1], argv[2], NULL, expected);
  config.expected_instance = expected;
  config.event = event;
  config.context = v;
  config.transcript = transcript;
  config.transcript_context = v;
  config.execution = execution;
  config.execution_context = v;
  v->until = now_ms() + 10000U;
  assert(cai_app_client_open(&config, now_ms(), &v->client, &v->error) ==
         CAI_OK);
  while (v->state.transport.phase != CAI_WORKSPACE_CONNECTED ||
         v->state.transport.generation == 0U)
    step(v);
  assert(v->state.transport.persist == !ephemeral &&
         v->state.transport.controller == v->state.transport.attachment);
  while (v->statuses == 0U)
    step(v);
  assert(!v->state.bound && v->state.producer == 0U);
  if (shutdown) {
    assert(cai_app_client_shutdown(v->client, 1U, v->state.transport.generation,
                                   &v->error) == CAI_OK);
  } else {
    assert(cai_app_client_bind(v->client, 1U, 0U, 0U, NULL, &v->error) ==
           CAI_OK);
    while (!v->state.ready)
      step(v);
    assert(v->state.session[0] != '\0');
    printf("%s\n", v->state.session);
    if (work || render) {
      cai_input_request request;
      cai_input_scope scope;
      assert(cai_app_client_subscribe(v->client, 1U, v->state.conversation,
                                      v->state.session, NULL, 0U, 0U,
                                      &v->error) == CAI_OK);
      while (!v->replayed)
        step(v);
      if (work) {
        memset(&request, 0, sizeof(request));
        request.operation = CAI_INPUT_OFFER;
        request.request_id = 1U;
        request.text = "Do the headless fixture task";
        scope.conversation = v->state.conversation;
        scope.generation = v->state.transport.generation;
        assert(cai_app_client_request(v->client, &scope, &request, &v->error) ==
               CAI_OK);
        while (!v->done || !v->finished || !v->accepted)
          step(v);
        assert(v->execution_chunks != 0U);
      }
      if (strstr(rendered, "  > Do the headless fixture task") == NULL ||
          strstr(rendered, "  HEADLESS ANSWER") == NULL)
        fprintf(stderr, "Rendered headless transcript:\n%s\n", rendered);
      assert(strstr(rendered, "  > Do the headless fixture task") != NULL &&
             strstr(rendered, "  HEADLESS ANSWER") != NULL &&
             strstr(rendered, "input_queue_delivery") == NULL &&
             strstr(rendered, "turn.completed") == NULL);
    }
    v->closing = 1;
    if (!abrupt)
      assert(cai_app_client_detach(v->client, &v->error) == CAI_OK);
  }
  if (!abrupt)
    while (v->state.transport.phase != CAI_WORKSPACE_DISCONNECTED)
      step(v);
  cai_app_client_close(v->client);
  cai_cli_transcript_presenter_close(v->presentation);
  cai_cli_presenter_close(&v->presenter);
  sl_destroy(editor);
  cai_error_cleanup(&v->error);
  free(v);
  return 0;
}
