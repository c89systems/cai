/* Exercise the actual socket frontend's status projection, without a runtime,
 * store or terminal process. Softline owns idempotent animation setters. */
#define _GNU_SOURCE
#include <assert.h>
#include <cai/app_client.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cai_app_view fixture_view;
static int busy_value, spinner_value;
static char status_message[1024];
static int fixture_get_view(cai_app_client *client, const cai_app_view **out,
                            cai_error *error) {
  (void)client;
  (void)error;
  *out = &fixture_view;
  return CAI_OK;
}
static int fixture_busy(sl_t *sl, int value) {
  busy_value = value;
  return sl_set_status_busy(sl, value);
}
static int fixture_spinner(sl_t *sl, int value) {
  spinner_value = value;
  return sl_set_status_spinner(sl, value);
}
static int fixture_message(sl_t *sl, const char *value) {
  snprintf(status_message, sizeof(status_message), "%s", value);
  return sl_set_status_message(sl, value);
}
#define cai_app_client_get_view fixture_get_view
#define sl_set_status_busy fixture_busy
#define sl_set_status_spinner fixture_spinner
#define sl_set_status_message fixture_message
#include "../cli/frontend.c"

int main(void) {
  frontend *f = calloc(1U, sizeof(*f));
  frontend_group *group = calloc(1U, sizeof(*group));
  frontend_view *view = calloc(1U, sizeof(*view));
  int turn, tick;
  assert(f != NULL && group != NULL && view != NULL);
  f->group = group;
  f->current = view;
  f->ui = &view->ui;
  f->ui->sl = sl_create();
  assert(f->ui->sl != NULL);
  f->ready = f->welcomed = 1;
  /* The getter is substituted; there is no transport or runtime. */
  f->client = (cai_app_client *)f;
  fixture_view.available = 1;
  fixture_view.effective.model = "gpt-6-luna";
  fixture_view.effective.reasoning_effort = "medium";
  assert(status(f) == 0 && !busy_value && !spinner_value);
  for (turn = 0; turn < 3; ++turn) {
    fixture_view.metrics.turn_active = 1;
    for (tick = 0; tick < 30; ++tick) {
      fixture_view.metrics.turn_elapsed_ms = (unsigned long long)tick * 1000U;
      fixture_view.runtime_state = tick % 3 == 0   ? CAI_AGENT_DISPATCHING_TOOL
                                   : tick % 3 == 1 ? CAI_AGENT_SAMPLING
                                                   : CAI_AGENT_IDLE;
      assert(status(f) == 0 && busy_value && spinner_value);
    }
    fixture_view.metrics.turn_active = 0;
    fixture_view.metrics.has_last_turn = 1;
    fixture_view.metrics.last_turn_duration_ms = 29000U;
    for (tick = 0; tick < 3; ++tick)
      assert(status(f) == 0 && !busy_value && !spinner_value);
  }
  /* A foreground reviewer remains busy even when the parent is idle. */
  f->review_open = 1;
  assert(status(f) == 0 && busy_value && spinner_value);
  f->review_open = 0;
  assert(status(f) == 0 && !busy_value && !spinner_value);
  fixture_view.metrics.turn_active = 1;
  f->compacting = 1;
  assert(status(f) == 0 &&
         strstr(status_message, "Compacting context") != NULL);
  fixture_view.metrics.turn_active = 0;
  assert(status(f) == 0 &&
         strstr(status_message, "Compacting context") == NULL);
  assert(goodbye(f) == CAI_OK && !busy_value && !spinner_value &&
         strcmp(status_message, "Goodbye.") == 0);
  sl_destroy(f->ui->sl);
  free(view);
  free(group);
  free(f);
  return 0;
}
