/* Test owner-side reconciliation and failure propagation without a terminal,
 * model request, timer or producer delay. Actual terminal cells are covered
 * by the separate resize regressions. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/softline_presenter.h"
#include <pthread.h>
#include <softline/softline.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static struct winsize geometry;
static int geometry_error, reconcile_error, reconciles, writes;
static char capture[8192];
static size_t captured;
static int capturing;
int __wrap_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...) {
  va_list arguments;
  struct winsize *out;
  (void)fd;
  (void)request;
  va_start(arguments, request);
  out = va_arg(arguments, struct winsize *);
  va_end(arguments);
  *out = geometry;
  return geometry_error;
}
int __wrap_sl_output_stream_reconcile(sl_t *sl);
int __wrap_sl_output_stream_reconcile(sl_t *sl) {
  (void)sl;
  reconciles++;
  return reconcile_error ? SL_ERROR_IO : SL_OK;
}
int __wrap_sl_output_stream_write(sl_t *sl, const char *bytes, size_t count);
int __wrap_sl_output_stream_write(sl_t *sl, const char *bytes, size_t count) {
  (void)sl;
  (void)bytes;
  (void)count;
  if (capturing) {
    if (count >= sizeof(capture) - captured)
      return SL_ERROR_IO;
    memcpy(capture + captured, bytes, count);
    captured += count;
    capture[captured] = '\0';
  }
  writes++;
  return SL_OK;
}
int __wrap_sl_output_stream_write_quoted_prompt(sl_t *sl, const char *text);
int __wrap_sl_output_stream_write_quoted_prompt(sl_t *sl, const char *text) {
  return __wrap_sl_output_stream_write(sl, text, strlen(text));
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "geometry assertion failed at line %d\n", __LINE__);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int geometry_cases(void) {
  cai_cli_presenter state;
  memset(&state, 0, sizeof(state));
  geometry.ws_col = 120;
  geometry.ws_row = 30;
  CHECK(cai_cli_presenter_sink(&state, "first", 5U) == 0 && reconciles == 1 &&
        writes == 1);
  CHECK(cai_cli_presenter_sink(&state, "next", 4U) == 0 && reconciles == 1 &&
        writes == 2);
  geometry.ws_row = 18;
  CHECK(cai_cli_presenter_sink(&state, "height", 6U) == 0 && reconciles == 2 &&
        writes == 3);
  geometry.ws_col = 60;
  reconcile_error = 1;
  CHECK(cai_cli_presenter_sink(&state, "blocked", 7U) == -1 &&
        reconciles == 3 && writes == 3);
  CHECK(state.output_columns == 120 && state.output_rows == 18);
  reconcile_error = 0;
  CHECK(cai_cli_presenter_sink(&state, "retry", 5U) == 0 && reconciles == 4 &&
        writes == 4);
  CHECK(state.output_columns == 60 && state.output_rows == 18);
  geometry.ws_col = 130;
  CHECK(cai_cli_presenter_sink(&state, "", 0U) == 0 && reconciles == 4 &&
        writes == 4);
  geometry_error = -1;
  CHECK(cai_cli_presenter_sink(&state, "redirected", 10U) == 0 &&
        reconciles == 4 && writes == 5);
  geometry_error = 0;
  geometry.ws_row = 0;
  CHECK(cai_cli_presenter_sink(&state, "unknown", 7U) == 0 && reconciles == 4 &&
        writes == 6);
  geometry.ws_row = 36;
  CHECK(cai_cli_presenter_sink(&state, "grown", 5U) == 0 && reconciles == 5 &&
        writes == 7);
  state.batch = 1;
  geometry.ws_row = 40;
  CHECK(cai_cli_presenter_reconcile_output(&state) == 0 && reconciles == 5);
  CHECK(cai_cli_presenter_prompt(&state, "batch") == 0 && reconciles == 5 &&
        writes == 7);
  state.batch = 0;
  reconcile_error = 1;
  CHECK(cai_cli_presenter_prompt(&state, "blocked") == -1 && reconciles == 6 &&
        writes == 7);
  reconcile_error = 0;
  CHECK(cai_cli_presenter_prompt(&state, "prompt") == 0 && reconciles == 7 &&
        writes == 8);
  return 0;
}
/* Real libmdf/Softline resources, independently of libcai_app and storage.
 * The link intentionally contains no controller/session/Pouch objects. */
static int presenter_lifecycle(void) {
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  sl_t *editor = sl_create();
  cai_cli_theme selected;
  CHECK(state != NULL && editor != NULL);
  state->sl = editor;
  state->width = 40; /* Native width refresh must keep a zero left margin. */
  capturing = 1;
  CHECK(cai_cli_theme_parse("plain", &state->theme) == NULL);
  state->render_theme = state->theme;
  state->theme_offsets[2] = 1LL;
  CHECK(cai_cli_presenter_renderer_create(state, &state->render_theme,
                                          &state->renderer, NULL) == CAI_OK);
  CHECK(cai_cli_presenter_tool_display_init(state, NULL) == CAI_OK);
  CHECK(cai_cli_presenter_text(state, "answer", 6U) == 0 &&
        state->response_open && !state->reasoning_open);
  CHECK(cai_cli_presenter_reasoning_body(state, "summary", 7U) == 0 &&
        !state->response_open && state->reasoning_open);
  CHECK(cai_cli_presenter_input(state, "/status", 1) == 0 &&
        !state->response_open && !state->reasoning_open);
  CHECK(cai_cli_theme_parse("gruvbox", &selected) == NULL);
  CHECK(cai_cli_presenter_set_theme(state, &selected, NULL) == CAI_OK);
  CHECK(cai_cli_presenter_text(state, "after-theme", 11U) == 0);
  CHECK(cai_cli_presenter_set_theme_scope(state, 2, NULL) == CAI_OK &&
        state->theme_scope == 2 && state->theme.prompt == selected.prompt);
  CHECK(cai_cli_presenter_set_theme_scope(state, 0, NULL) == CAI_OK);
  CHECK(cai_cli_presenter_text(state, "final", 5U) == 0);
  cai_cli_presenter_close(state);
  capturing = 0;
  CHECK(strstr(capture, "answer") != NULL &&
        strstr(capture, "after-theme") != NULL &&
        strstr(capture, "  answer") == NULL &&
        strstr(capture, "  after-theme") == NULL);
  CHECK(sl_set_status_message(editor, "Still owned by frontend") == SL_OK &&
        sl_buffer(editor) != NULL);
  sl_destroy(editor);
  free(state);
  return 0;
}
static void *run(void *context) {
  int *result = context;
  *result = geometry_cases();
  if (*result == 0)
    *result = presenter_lifecycle();
  return NULL;
}
int main(void) {
  pthread_t worker;
  int result = 1;
  CHECK(pthread_create(&worker, NULL, run, &result) == 0);
  CHECK(pthread_join(worker, NULL) == 0);
  return result;
}
