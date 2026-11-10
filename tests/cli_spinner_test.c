/* Exercise the actual socket frontend's status projection, without a runtime,
 * store or terminal process. Repeated status refreshes must not restart the
 * Softline animation or resynchronize its queue's busy state. */
#define _GNU_SOURCE
#include <assert.h>
#include <cai/app_client.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cai_app_view fixture_view;
static int busy_value, spinner_value;
static unsigned int busy_calls, spinner_calls;
static char status_message[1024];
static int fixture_get_view(cai_app_client *client, const cai_app_view **out,
                            cai_error *error) {
  (void)client;
  (void)error;
  *out = &fixture_view;
  return CAI_OK;
}
static int fixture_busy(sl_t *sl, int value) {
  ++busy_calls;
  busy_value = value;
  return sl_set_status_busy(sl, value);
}
static int fixture_spinner(sl_t *sl, int value) {
  ++spinner_calls;
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

static void expect_status(frontend *f, int busy, unsigned int calls) {
  assert(status(f) == 0);
  assert(busy_value == busy && spinner_value == busy);
  assert(busy_calls == calls && spinner_calls == calls);
}

int main(void) {
  frontend *f = calloc(1U, sizeof(*f));
  frontend_group *group = calloc(1U, sizeof(*group));
  frontend_view *view = calloc(1U, sizeof(*view));
  frontend_view *other = calloc(1U, sizeof(*other));
  unsigned int calls;
  sl_key_action_t action;
  int turn, tick;
  assert(f != NULL && group != NULL && view != NULL && other != NULL);
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
  cai_cli_status_init(&f->status, "/workspace", NULL);
  expect_status(f, 0, 1U);
  /* Exercise the real frontend's provider selection, not just the status
   * formatter: an empty subscription object must not hide API spending. */
  fixture_view.provider = "openai";
  fixture_view.metrics.session_usage.estimated_spend_usd = 1.25;
  expect_status(f, 0, 1U);
  assert(strcmp(f->status.usage, "cost ~$1.2500") == 0);
  fixture_view.metrics.session_usage.estimated_spend_usd = 0.0;
  expect_status(f, 0, 1U);
  assert(strcmp(f->status.usage, "cost ?$") == 0);
  fixture_view.provider = "berget";
  fixture_view.metrics.session_usage.estimated_spend_eur = 1.25;
  expect_status(f, 0, 1U);
  assert(strcmp(f->status.usage, "cost ~€1.2500") == 0);
  fixture_view.metrics.session_usage.estimated_spend_eur = 0.0;
  expect_status(f, 0, 1U);
  assert(strcmp(f->status.usage, "cost ?€") == 0);
  fixture_view.provider = "chatgpt";
  fixture_view.quota.has_weekly = 1;
  fixture_view.quota.weekly_remaining_percent = 88.0;
  fixture_view.quota.has_credit_balance = 1;
  fixture_view.quota.credit_balance = 20.0;
  expect_status(f, 0, 1U);
  assert(strcmp(f->status.usage, "w 88%") == 0 &&
         strcmp(f->status.credits, "crd 20") == 0);
  for (turn = 0; turn < 3; ++turn) {
    fixture_view.metrics.turn_active = 1;
    for (tick = 0; tick < 30; ++tick) {
      fixture_view.metrics.turn_elapsed_ms = (unsigned long long)tick * 1000U;
      fixture_view.runtime_state = tick % 3 == 0   ? CAI_AGENT_DISPATCHING_TOOL
                                   : tick % 3 == 1 ? CAI_AGENT_SAMPLING
                                                   : CAI_AGENT_IDLE;
      assert(status(f) == 0 && busy_value && spinner_value);
      assert(busy_calls == 2U + (unsigned int)turn * 2U);
      assert(spinner_calls == busy_calls);
    }
    fixture_view.metrics.turn_active = 0;
    fixture_view.metrics.has_last_turn = 1;
    fixture_view.metrics.last_turn_duration_ms = 29000U;
    /* The worker has finished timing, but the final append has not settled.
     * State changes and repeated observations cannot stop/restart animation
     * or expose the previous/computed completion message prematurely. */
    for (tick = 0; tick < 10; ++tick) {
      fixture_view.runtime_state =
          tick % 2 ? CAI_AGENT_SAMPLING : CAI_AGENT_DISPATCHING_TOOL;
      expect_status(f, 1, 2U + (unsigned int)turn * 2U);
      assert(strstr(status_message, "Finishing turn") != NULL &&
             strstr(status_message, "Worked for") == NULL);
    }
    fixture_view.runtime_state = CAI_AGENT_COMPLETED;
    for (tick = 0; tick < 3; ++tick) {
      assert(status(f) == 0 && !busy_value && !spinner_value);
      assert(busy_calls == 3U + (unsigned int)turn * 2U);
      assert(spinner_calls == busy_calls);
    }
  }
  /* A foreground reviewer remains busy even when the parent is idle. */
  calls = busy_calls;
  fixture_view.metrics.turn_active = 1;
  expect_status(f, 1, ++calls);
  f->review_open = 1;
  fixture_view.metrics.turn_active = 0;
  for (tick = 0; tick < 30; ++tick) {
    expect_status(f, 1, calls);
    assert(strstr(status_message, "Worked for") == NULL);
  }
  f->review_open = 0;
  expect_status(f, 0, ++calls);
  fixture_view.metrics.turn_active = 1;
  expect_status(f, 1, ++calls);
  f->compacting = 1;
  for (tick = 0; tick < 30; ++tick) {
    expect_status(f, 1, calls);
    assert(strstr(status_message, "Compacting context") != NULL);
  }
  /* Compaction ending within a turn must not interrupt its spinner. */
  f->compacting = 0;
  expect_status(f, 1, calls);
  assert(strstr(status_message, "Compacting context") == NULL);
  fixture_view.metrics.turn_active = 0;
  expect_status(f, 0, ++calls);

  /* Both cancelled and failed operations stop once, and can restart. */
  for (turn = 0; turn < 2; ++turn) {
    fixture_view.metrics.turn_active = 1;
    expect_status(f, 1, ++calls);
    fixture_view.runtime_state =
        turn == 0 ? CAI_AGENT_CANCELLED : CAI_AGENT_FAILED;
    fixture_view.metrics.turn_active = 0;
    for (tick = 0; tick < 3; ++tick)
      expect_status(f, 0, calls + 1U);
    ++calls;
  }

  /* Each retained editor owns its animation; switching back must not reset
   * an already busy editor, even after displaying an idle session. */
  fixture_view.metrics.turn_active = 1;
  expect_status(f, 1, ++calls);
  other->ui.sl = sl_create();
  assert(other->ui.sl != NULL);
  f->current = other;
  f->ui = &other->ui;
  fixture_view.metrics.turn_active = 0;
  expect_status(f, 0, ++calls);
  f->current = view;
  f->ui = &view->ui;
  fixture_view.metrics.turn_active = 1;
  assert(status(f) == 0);
  assert(busy_calls == calls && spinner_calls == calls);

  assert(sl_set_buffer(f->ui->sl, "draft") == SL_OK);
  assert(eof_key(f->ui->sl, SL_KEY_CTRL_D, f, &action) == SL_OK &&
         action == SL_KEY_ACTION_PASS && !f->quit &&
         strcmp(sl_buffer(f->ui->sl), "draft") == 0);
  assert(busy_calls == calls && spinner_calls == calls);
  assert(sl_set_buffer(f->ui->sl, "") == SL_OK);
  assert(eof_key(f->ui->sl, SL_KEY_CTRL_D, f, &action) == SL_OK &&
         action == SL_KEY_ACTION_SUSPEND && f->quit);
  assert(!busy_value && !spinner_value &&
         strcmp(status_message, "Goodbye.") == 0);
  ++calls;
  assert(busy_calls == calls && spinner_calls == calls);
  assert(goodbye(f) == CAI_OK);
  assert(busy_calls == calls && spinner_calls == calls);
  sl_destroy(other->ui.sl);
  sl_destroy(f->ui->sl);
  free(other);
  free(view);
  free(group);
  free(f);
  return 0;
}
