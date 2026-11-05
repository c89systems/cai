/* Test owner-side reconciliation and failure propagation without a terminal,
 * model request, timer or producer delay. Actual terminal cells are covered
 * by the separate resize regressions. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/softline_presenter.h"
#include "../cli/transcript_presenter.h"
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
static int capturing, write_error;
static size_t emitted;
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
  if (write_error)
    return SL_ERROR_IO;
  emitted += count;
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
        writes > 7);
  cai_cli_presenter_close(&state);
  return 0;
}

static int prompt_cases(void) {
  static const char text[] =
      "Literal **stars** _path_ `code` [name](url) <tag> &amp; \\path\n"
      "café λ 🚀\n\nlast \033[2J \xc2\x9b invalid \xff \xe2\x82";
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  sl_t *editor = sl_create();
  char reference[sizeof(capture)];
  char *long_text = malloc(65537U);
  size_t before;
  CHECK(state != NULL && editor != NULL && long_text != NULL);
  state->sl = editor;
  state->interactive = 1;
  state->width = 80;
  CHECK(cai_cli_theme_parse("plain", &state->theme) == NULL);
  state->render_theme = state->theme;
  CHECK(sl_set_buffer(editor, "Untouched draft") == SL_OK);
  capturing = 1;
  captured = 0U;
  capture[0] = '\0';
  CHECK(cai_cli_presenter_prompt(state, text) == 0);
  CHECK(strstr(capture, "  > Literal **stars** _path_ `code` [name](url)") !=
        NULL);
  CHECK(strstr(capture, "café λ 🚀") != NULL &&
        strchr(capture, '\033') == NULL);
  CHECK(strstr(capture, "invalid ? ??") != NULL);
  memcpy(reference, capture, captured + 1U);
  captured = 0U;
  capture[0] = '\0';
  state->feed_breaks = state->block_spacing_pending = 0;
  CHECK(cai_cli_presenter_prompt(state, text) == 0 &&
        strcmp(capture, reference) == 0);
  before = captured;
  CHECK(cai_cli_presenter_prompt(state, "") == 0 && captured == before);
  write_error = 1;
  CHECK(cai_cli_presenter_prompt(state, "failed write") == -1);
  write_error = 0;
  CHECK(cai_cli_presenter_prompt(state, "recovered") == 0);
  CHECK(strstr(capture, "  > recovered") != NULL);
  /* Native accepted input is already available as a complete string. This
   * checks long literal rendering, not bounded remote streaming: libmdf
   * currently retains this whole word before rendering it. */
  capturing = 0;
  memset(long_text, 'x', 65536U);
  long_text[65536U] = '\0';
  before = emitted;
  CHECK(cai_cli_presenter_prompt(state, long_text) == 0 &&
        emitted > before + 65536U);
  CHECK(strcmp(sl_buffer(editor), "Untouched draft") == 0);
  cai_cli_presenter_close(state);
  sl_destroy(editor);
  free(long_text);
  free(state);
  return 0;
}

/** Socket fragmentation must not alter literal text, wrapping or prompt
 * markers. Exercise real libmdf output, not the adapter's internal counters. */
static int streamed_input_cases(void) {
  static const char text[] =
      "Literal **stars** _path_ `code` [name](url) <tag> &amp; \\path\n"
      "café λ 🚀 \xe2\xc3\xa9 end\n\nlast \033[2J \xc2\x9b invalid \xff "
      "\xe2\x82";
  static const char words[] = "a short word **literal** café 🚀 ";
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  sl_t *editor = sl_create();
  char reference[sizeof(capture)];
  size_t split, i, before;
  int command;
  CHECK(state != NULL && editor != NULL);
  state->sl = editor;
  state->interactive = 1;
  state->width = 80;
  /* Unknown original time suppresses the wall clock, making native/socket
   * comparisons stable even across minute boundaries. */
  state->presentation_event = 1;
  CHECK(cai_cli_theme_parse("plain", &state->theme) == NULL);
  state->render_theme = state->theme;
  CHECK(sl_set_buffer(editor, "Do not submit this draft") == SL_OK);
  capturing = 1;
  for (command = 0; command <= 1; ++command) {
    captured = 0U;
    capture[0] = '\0';
    state->feed_breaks = state->block_spacing_pending = 0;
    CHECK(cai_cli_presenter_input(state, text, command) == 0);
    memcpy(reference, capture, captured + 1U);
    for (split = 0U; split <= strlen(text); ++split) {
      captured = 0U;
      capture[0] = '\0';
      state->feed_breaks = state->block_spacing_pending = 0;
      CHECK(cai_cli_presenter_input_begin(state, command) == 0);
      CHECK(cai_cli_presenter_input_write(state, text, split) == 0);
      CHECK(cai_cli_presenter_input_write(state, text + split,
                                          strlen(text) - split) == 0);
      CHECK(cai_cli_presenter_input_end(state) == 0);
      CHECK(strcmp(capture, reference) == 0);
    }
    captured = 0U;
    capture[0] = '\0';
    state->feed_breaks = state->block_spacing_pending = 0;
    CHECK(cai_cli_presenter_input_begin(state, command) == 0);
    for (i = 0U; i < strlen(text); ++i)
      CHECK(cai_cli_presenter_input_write(state, text + i, 1U) == 0);
    CHECK(cai_cli_presenter_input_end(state) == 0);
    CHECK(strcmp(capture, reference) == 0);
    before = captured;
    CHECK(cai_cli_presenter_input_begin(state, command) == 0);
    CHECK(cai_cli_presenter_input_write(state, NULL, 0U) == 0);
    CHECK(cai_cli_presenter_input_end(state) == 0 && captured == before);
    CHECK(cai_cli_presenter_input_begin(state, command) == 0);
    CHECK(cai_cli_presenter_input_write(state, "discarded suffix", 16U) == 0);
    /* A command may already have separated its marker from the preceding
     * block. Aborting must not emit the retained literal suffix. */
    before = captured;
    CHECK(cai_cli_presenter_input_begin(state, command) == -1);
    CHECK(cai_cli_presenter_text(state, "must not interleave", 19U) == -1);
    CHECK(cai_cli_presenter_reasoning_body(state, "waiting", 7U) == -1);
    CHECK(cai_cli_presenter_set_theme_scope(state, 1, NULL) == CAI_ERR_INVALID);
    cai_cli_presenter_input_abort(state);
    CHECK(captured == before);
    CHECK(cai_cli_presenter_input(state, "after abort", command) == 0);
    CHECK(strstr(capture, "discarded suffix") == NULL &&
          strstr(capture, "after abort") != NULL);
    CHECK(cai_cli_presenter_input_begin(state, command) == 0);
    write_error = 1;
    CHECK(cai_cli_presenter_input_write(state, words, sizeof(words) - 1U) ==
              -1 ||
          cai_cli_presenter_input_end(state) == -1);
    write_error = 0;
    before = emitted;
    CHECK(cai_cli_presenter_input_write(state, words, sizeof(words) - 1U) ==
              -1 &&
          cai_cli_presenter_input_end(state) == -1 && emitted == before);
    cai_cli_presenter_input_abort(state);
    CHECK(cai_cli_presenter_input(state, "after failure", command) == 0);
  }
  /* Generate a multi-megabyte logical prompt without ever holding it in CAI.
   * Ordinary words must reach Softline before the producer/field ends. This
   * does not claim to cap the pinned libmdf's unbroken-word decision buffer. */
  capturing = 0;
  CHECK(cai_cli_presenter_input_begin(state, 0) == 0);
  before = emitted;
  for (i = 0U; i < 100000U; ++i) {
    CHECK(cai_cli_presenter_input_write(state, words, sizeof(words) - 1U) == 0);
    if (i == 100U)
      CHECK(emitted > before + 1000U);
  }
  CHECK(emitted > before + 1000000U);
  CHECK(cai_cli_presenter_input_end(state) == 0);
  CHECK(strcmp(sl_buffer(editor), "Do not submit this draft") == 0);
  cai_cli_presenter_close(state);
  sl_destroy(editor);
  free(state);
  return 0;
}

/** Deliver the same decoded shapes as the application socket client. Only
 * BEGIN carries the original event timestamp on the wire. */
static int transcript_record(cai_cli_transcript_presenter *p, const char *type,
                             const void *data, size_t length, size_t chunk,
                             uint64_t sequence, uint64_t timestamp) {
  cai_transcript_event event;
  size_t at;
  memset(&event, 0, sizeof(event));
  event.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
  event.type = type;
  event.stream = 1U;
  event.sequence = sequence;
  event.has_data = data != NULL;
  event.timestamp_ns = timestamp;
  if (cai_cli_transcript_presenter_event(p, &event, NULL) != CAI_OK)
    return -1;
  event.timestamp_ns = 0U;
  for (at = 0U; at < length;) {
    event.kind = CAI_TRANSCRIPT_DATA;
    event.offset = at;
    event.length = length - at < chunk ? length - at : chunk;
    event.data = (const char *)data + at;
    if (cai_cli_transcript_presenter_event(p, &event, NULL) != CAI_OK)
      return -1;
    at += event.length;
  }
  event.kind = CAI_TRANSCRIPT_EVENT_END;
  event.offset = length;
  event.length = 0U;
  event.data = NULL;
  return cai_cli_transcript_presenter_event(p, &event, NULL) == CAI_OK ? 0 : -1;
}

/** Verify accepted-input provenance stripping and Markdown continuity across
 * replay/live spans with real renderers and no store/controller link. */
static int transcript_cases(void) {
  static const char text[] = "literal **stars** café 🚀\nnext line";
  static const char prefix[] = "L\n7\nsession\n18446744073709551615\n1\n";
  static const char *const invalid[] = {
      "",
      "X\n1\ns\n1\n0\nprompt",
      "L!1\ns\n1\n0\nprompt",
      "L\n0\n",
      "L\n129\n",
      "L\n18446744073709551616\n",
      "L\n1\ns!1\n0\nprompt",
      "L\n1\ns\n0\n0\nprompt",
      "L\n1\ns\n1\n3\nprompt",
      "L\n1\ns\n1\n1!prompt",
      "L\n1\ns\n1\n1\n",
      "C\n1\ns\n18446744073709551616\n1\nprompt"};
  cai_cli_presenter *ui = calloc(1U, sizeof(*ui));
  cai_cli_transcript_presenter *p = NULL;
  sl_t *editor = sl_create();
  cai_transcript_event event;
  char delivery_text[sizeof(prefix) + sizeof(text)];
  char reference[sizeof(capture)];
  size_t i, before;
  CHECK(ui != NULL && editor != NULL);
  ui->sl = editor;
  ui->interactive = 1;
  ui->presentation_event = 1;
  CHECK(cai_cli_theme_parse("plain", &ui->theme) == NULL);
  ui->render_theme = ui->theme;
  CHECK(cai_cli_presenter_renderer_create(ui, &ui->render_theme, &ui->renderer,
                                          NULL) == CAI_OK);
  CHECK(cai_cli_transcript_presenter_open(ui, &p, NULL) == CAI_OK);
  CHECK(sl_set_buffer(editor, "Unsent draft") == SL_OK);
  snprintf(delivery_text, sizeof(delivery_text), "%s%s", prefix, text);
  capturing = 1;
  captured = 0U;
  capture[0] = '\0';
  CHECK(cai_cli_presenter_prompt(ui, text) == 0);
  memcpy(reference, capture, captured + 1U);
  for (i = 1U; i <= strlen(delivery_text); ++i) {
    captured = 0U;
    capture[0] = '\0';
    ui->feed_breaks = ui->block_spacing_pending = 0;
    CHECK(transcript_record(p, "input_queue_delivery", delivery_text,
                            strlen(delivery_text), i, i, 0U) == 0);
    CHECK(strcmp(capture, reference) == 0);
  }
  /* Admission metadata and encoded tool output must not escape through this
   * adapter. The tool presenter consumes the separate typed execution path. */
  before = captured;
  CHECK(transcript_record(p, "input_queue", "not a delivered prompt", 22U, 1U,
                          1000U, 0U) == 0);
  CHECK(transcript_record(p, "execution", "{\"data\":\"not raw output\"}", 25U,
                          1U, 1001U, 0U) == 0);
  CHECK(captured == before);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    CHECK(transcript_record(p, "input_queue_delivery", invalid[i],
                            strlen(invalid[i]), 1U, 2000U + i, 0U) == -1);
    CHECK(transcript_record(p, "turn_submitted", "held", 4U, 1U, 3000U + i,
                            0U) == -1);
    cai_cli_transcript_presenter_reset(p);
  }
  captured = 0U;
  capture[0] = '\0';
  ui->feed_breaks = ui->block_spacing_pending = 0;
  CHECK(cai_cli_presenter_text(ui, "A **bold** café answer.\n", 25U) == 0);
  CHECK(cai_cli_presenter_finish_response(ui) == 0);
  memcpy(reference, capture, captured + 1U);
  captured = 0U;
  capture[0] = '\0';
  ui->feed_breaks = ui->block_spacing_pending = 0;
  CHECK(transcript_record(p, "assistant_text_delta", "A **bo", 6U, 1U, 4000U,
                          0U) == 0);
  memset(&event, 0, sizeof(event));
  event.kind = CAI_TRANSCRIPT_REPLAY_END;
  event.stream = 1U;
  CHECK(cai_cli_transcript_presenter_event(p, &event, NULL) == CAI_OK);
  CHECK(transcript_record(p, "assistant_text_delta", "ld** café answer.\n", 19U,
                          1U, 4001U, 0U) == 0);
  CHECK(transcript_record(p, "assistant_text_end", NULL, 0U, 1U, 4002U, 0U) ==
        0);
  CHECK(strcmp(capture, reference) == 0);
  CHECK(transcript_record(p, "reasoning_summary_started", NULL, 0U, 1U, 4003U,
                          0U) == 0);
  CHECK(transcript_record(p, "reasoning_summary", "Private title\n\nThinking ",
                          24U, 1U, 4004U, 0U) == 0);
  CHECK(transcript_record(p, "reasoning_summary", "**carefully**.\n", 15U, 1U,
                          4005U, 0U) == 0);
  CHECK(transcript_record(p, "assistant_text_end", NULL, 0U, 1U, 4006U, 0U) ==
        0);
  CHECK(strstr(capture, "  > Thinking carefully.") != NULL &&
        strstr(capture, "Private title") == NULL);
  /* A provisional prompt must not flush on disconnect. Neither malformed
   * offsets nor resets acknowledge a cursor or alter the local draft. */
  memset(&event, 0, sizeof(event));
  event.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
  event.stream = 1U;
  event.sequence = 5000U;
  event.type = "turn_submitted";
  event.has_data = 1;
  CHECK(cai_cli_transcript_presenter_event(p, &event, NULL) == CAI_OK);
  event.kind = CAI_TRANSCRIPT_DATA;
  event.data = "unfinished prompt";
  event.length = 17U;
  CHECK(cai_cli_transcript_presenter_event(p, &event, NULL) == CAI_OK);
  before = emitted;
  event.offset = 2U;
  CHECK(cai_cli_transcript_presenter_event(p, &event, NULL) ==
        CAI_ERR_PROTOCOL);
  cai_cli_transcript_presenter_reset(p);
  CHECK(emitted == before && strstr(capture, "unfinished prompt") == NULL);
  CHECK(transcript_record(p, "turn_submitted", "Recovered", 9U, 1U, 5001U,
                          0U) == 0);
  /* DATA/END lack a wire timestamp; the retained BEGIN must supply the DTG. */
  captured = 0U;
  capture[0] = '\0';
  ui->feed_breaks = ui->block_spacing_pending = 0;
  ui->utc = 1;
  ui->presentation_timestamp_ns = 1609459200000000000ULL;
  CHECK(cai_cli_presenter_text(ui, "dated", 5U) == 0);
  CHECK(cai_cli_presenter_finish_response(ui) == 0);
  memcpy(reference, capture, captured + 1U);
  captured = 0U;
  capture[0] = '\0';
  ui->feed_breaks = ui->block_spacing_pending = 0;
  ui->presentation_timestamp_ns = 0U;
  CHECK(transcript_record(p, "assistant_text_delta", "dated", 5U, 1U, 6000U,
                          1609459200000000000ULL) == 0);
  CHECK(transcript_record(p, "assistant_text_end", NULL, 0U, 1U, 6001U, 0U) ==
        0);
  CHECK(strcmp(capture, reference) == 0);
  CHECK(ui->presentation_timestamp_ns == 0U && ui->presentation_event == 1);
  CHECK(strcmp(sl_buffer(editor), "Unsent draft") == 0);
  captured = 0U;
  capture[0] = '\0';
  CHECK(transcript_record(p, "user_shell_started", "echo output", 11U, 1U,
                          7000U, 0U) == 0);
  CHECK(transcript_record(p, "user_shell_output_hex", "c2", 2U, 1U, 7001U,
                          0U) == 0);
  CHECK(transcript_record(p, "user_shell_output_hex", "9b", 2U, 1U, 7002U,
                          0U) == 0);
  CHECK(transcript_record(p, "user_shell_output_tail", "VISIBLE_TAIL\n", 13U,
                          1U, 7003U, 0U) == 0);
  CHECK(transcript_record(p, "user_shell_completed", NULL, 0U, 1U, 7004U, 0U) ==
        0);
  CHECK(strstr(capture, "echo output") != NULL &&
        strstr(capture, "\\xc2\\x9b") != NULL &&
        strstr(capture, "VISIBLE_TAIL") != NULL);
  captured = 0U;
  capture[0] = '\0';
  cai_cli_transcript_presenter_skip_shell(p, 8000U, 8004U);
  memset(&event, 0, sizeof(event));
  event.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  event.stream = 1U;
  CHECK(cai_cli_transcript_presenter_event(p, &event, NULL) == CAI_OK);
  CHECK(transcript_record(p, "user_shell_started", "HIDDEN_COMMAND", 14U, 1U,
                          8000U, 0U) == 0);
  CHECK(transcript_record(p, "assistant_text_delta", "KEEPVISIBLE", 11U, 1U,
                          8001U, 0U) == 0);
  CHECK(transcript_record(p, "assistant_text_end", NULL, 0U, 1U, 8002U, 0U) ==
        0);
  CHECK(transcript_record(p, "user_shell_output_tail", "HIDDEN_TAIL", 11U, 1U,
                          8003U, 0U) == 0);
  CHECK(transcript_record(p, "user_shell_completed", NULL, 0U, 1U, 8004U, 0U) ==
        0);
  CHECK(transcript_record(p, "user_shell_output_tail", "AFTER_RANGE", 11U, 1U,
                          8005U, 0U) == 0);
  CHECK(strstr(capture, "HIDDEN") == NULL &&
        strstr(capture, "KEEPVISIBLE") != NULL &&
        strstr(capture, "AFTER_RANGE") != NULL);
  cai_cli_transcript_presenter_skip_shell(p, 9000U, 0U);
  CHECK(transcript_record(p, "user_shell_output_tail", "OPEN_HIDDEN", 11U, 1U,
                          9001U, 0U) == 0);
  CHECK(transcript_record(p, "user_shell_completed", NULL, 0U, 1U, 9002U, 0U) ==
        0);
  CHECK(transcript_record(p, "user_shell_output_tail", "OPEN_VISIBLE", 12U, 1U,
                          9003U, 0U) == 0);
  CHECK(strstr(capture, "OPEN_HIDDEN") == NULL &&
        strstr(capture, "OPEN_VISIBLE") != NULL);
  {
    static const char table[] = "| Field |\n| --- |\n| TABLEVALUE |\n";
    captured = 0U;
    capture[0] = '\0';
    ui->tool_result_body = 1;
    CHECK(cai_cli_presenter_text(ui, table, sizeof(table) - 1U) == 0);
    ui->tool_result_body = 0;
    CHECK(transcript_record(p, "assistant_text_delta", "AFTERTABLE", 10U, 1U,
                            9010U, 0U) == 0);
    CHECK(transcript_record(p, "assistant_text_end", NULL, 0U, 1U, 9011U, 0U) ==
          0);
    CHECK(strstr(capture, "TABLEVALUE") != NULL &&
          strstr(capture, "AFTERTABLE") != NULL);
  }
  cai_cli_transcript_presenter_close(p);
  cai_cli_presenter_close(ui);
  sl_destroy(editor);
  free(ui);
  capturing = 0;
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
    *result = prompt_cases();
  if (*result == 0)
    *result = streamed_input_cases();
  if (*result == 0)
    *result = transcript_cases();
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
