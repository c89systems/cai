#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app_client.h>
#include <errno.h>
#include <lc/lc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct control {
  cai_app_client *client;
  cai_app_client_state state;
  uint64_t until;
  int replayed, accepted, complete;
} control;
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
static uint64_t now_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int event(void *context, const cai_app_client_event *e,
                 cai_error *error) {
  control *c = context;
  (void)error;
  c->state = *e->state;
  if (e->type == CAI_APP_CLIENT_INPUT && e->input != NULL &&
      e->input->type == CAI_INPUT_OBSERVATION_RESULT) {
    assert(e->input->result.outcome == CAI_INPUT_ACCEPTED);
    c->accepted = 1;
  }
  return CAI_OK;
}
static int transcript(void *context, const cai_transcript_event *e,
                      cai_error *error) {
  control *c = context;
  (void)error;
  if (e->kind == CAI_TRANSCRIPT_REPLAY_END)
    c->replayed = 1;
  return CAI_OK;
}
static int execution(void *context, const cai_app_execution_event *e,
                     cai_error *error) {
  (void)error;
  if (e->record != NULL && strcmp(e->record->kind, "turn.completed") == 0)
    ((control *)context)->complete = 1;
  return CAI_OK;
}
static void step(control *c) {
  struct pollfd fd;
  uint64_t deadline, now = now_ms();
  assert(now < c->until);
  assert(cai_app_client_prepare(c->client, &fd, &deadline, NULL) == CAI_OK);
  if (deadline == 0U || deadline > c->until)
    deadline = c->until;
  assert(poll(&fd, 1U, deadline > now ? (int)(deadline - now) : 0) >= 0 ||
         errno == EINTR);
  assert(cai_app_client_pump(c->client, now_ms(), NULL) == CAI_OK);
  assert(cai_app_client_get_state(c->client, &c->state, NULL) == CAI_OK);
}
int main(int argc, char **argv) {
  cai_app_client_config config;
  cai_input_scope scope;
  cai_input_request request;
  control c;
  char previous[129];
  int new_session, stop;
  assert(argc == 4);
  new_session = strcmp(argv[3], "new") == 0;
  stop = strcmp(argv[3], "stop") == 0;
  assert(new_session || stop || strcmp(argv[3], "work") == 0);
  memset(&c, 0, sizeof(c));
  c.until = now_ms() + 10000U;
  cai_app_client_config_init(&config);
  config.runtime_directory = argv[1];
  config.workspace = argv[2];
  config.event = event;
  config.context = &c;
  config.transcript = transcript;
  config.transcript_context = &c;
  config.execution = execution;
  config.execution_context = &c;
  assert(cai_app_client_open(&config, now_ms(), &c.client, NULL) == CAI_OK);
  while (c.state.transport.phase != CAI_WORKSPACE_CONNECTED)
    step(&c);
  assert(cai_app_client_takeover(c.client, 1U, c.state.transport.generation,
                                 NULL) == CAI_OK);
  while (c.state.transport.controller != c.state.transport.attachment)
    step(&c);
  if (stop) {
    assert(cai_app_client_shutdown(c.client, 2U, c.state.transport.generation,
                                   NULL) == CAI_OK);
    while (c.state.transport.phase != CAI_WORKSPACE_DISCONNECTED)
      step(&c);
    cai_app_client_close(c.client);
    return 0;
  }
  assert(cai_app_client_bind(c.client, 1U, 0U, 0U, NULL, NULL) == CAI_OK);
  while (!c.state.ready)
    step(&c);
  snprintf(previous, sizeof(previous), "%s", c.state.session);
  assert(cai_app_client_subscribe(c.client, 1U, c.state.conversation,
                                  c.state.session, NULL, 0U, 0U, 0LL,
                                  NULL) == CAI_OK);
  while (!c.replayed)
    step(&c);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = 1U;
  request.text = new_session ? "/new" : "Work during terminal handoff.";
  scope.conversation = c.state.conversation;
  scope.generation = c.state.transport.generation;
  assert(cai_app_client_request(c.client, &scope, &request, NULL) == CAI_OK);
  if (new_session) {
    const cai_app_view *view = NULL;
    do {
      step(&c);
      assert(cai_app_client_get_view(c.client, &view, NULL) == CAI_OK);
    } while (view == NULL || !view->available ||
             strcmp(previous, view->session_id) == 0);
  } else
    while (!c.accepted || !c.complete)
      step(&c);
  assert(cai_app_client_detach(c.client, NULL) == CAI_OK);
  while (c.state.transport.phase != CAI_WORKSPACE_DISCONNECTED)
    step(&c);
  cai_app_client_close(c.client);
  return 0;
}
