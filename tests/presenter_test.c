/* Verify the actual frontend presenter without constructing a runtime. */
#define _POSIX_C_SOURCE 200809L
#include "../cli/softline_presenter.h"
#include "../cli/transcript_presenter.h"
#include <cai/agent_runtime.h>
#include <cai/dtg.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int command(cai_cli_presenter *state, const char *text) {
  return cai_cli_presenter_input(state, text, 1);
}
static int literal(cai_cli_presenter *state, const char *data, size_t n,
                   cai_error *error) {
  (void)error;
  return cai_cli_presenter_renderer_sink(state, data, n) == 0
             ? CAI_OK
             : CAI_ERR_TRANSPORT;
}
typedef struct event_log_capture {
  char bytes[16384];
  size_t length;
} event_log_capture;

static int event_log_write(void *context, const char *bytes, size_t count,
                           size_t *written) {
  event_log_capture *capture = (event_log_capture *)context;
  if (count >= sizeof(capture->bytes) - capture->length)
    return EIO;
  memcpy(capture->bytes + capture->length, bytes, count);
  capture->length += count;
  capture->bytes[capture->length] = '\0';
  *written = count;
  return 0;
}

typedef struct table_counter {
  size_t bytes, rows, matched;
} table_counter;
static int table_count_write(void *context, const char *bytes, size_t count) {
  table_counter *total = context;
  size_t i;
  total->bytes += count;
  for (i = 0U; i < count; ++i) {
    if (bytes[i] == "tool"[total->matched]) {
      if (++total->matched == 4U) {
        total->rows++;
        total->matched = 0U;
      }
    } else
      total->matched = bytes[i] == 't' ? 1U : 0U;
  }
  return 0;
}

/* Small tables retain full layout. Oversized tables flush once and continue
 * before EOF at either the byte or row limit, preserving every body row. */
static int test_streaming_tables(void) {
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  mdf_sink sink;
  table_counter total;
  int scenario, i, rows, rc = 1;
  const char *header = "| Tool | Description |\n| --- | --- |\n";
  const char *long_row =
      "| tool | A complete description with enough words to exceed the full "
      "table retention budget after many rows. |\n";
  const char *short_row = "| tool | short |\n";
  if (state == NULL)
    return 1;
  state->width = 120;
  for (scenario = 0; scenario < 3; ++scenario) {
    const char *row = scenario == 1 ? long_row : short_row;
    memset(&total, 0, sizeof(total));
    if (cai_cli_presenter_renderer_create(state, &state->render_theme,
                                          &state->renderer, NULL) != CAI_OK)
      goto done;
    sink.userdata = &total;
    sink.write = table_count_write;
    if (state->renderer->set_sink(state->renderer, &sink) != MDF_OK ||
        state->renderer->feed(state->renderer, header, strlen(header)) !=
            MDF_OK ||
        state->renderer->feed(state->renderer, row, strlen(row)) != MDF_OK ||
        total.rows != 0U)
      goto done;
    rows = scenario == 0 ? 5 : 1030;
    for (i = 0; i < rows; i++)
      if (state->renderer->feed(state->renderer, row, strlen(row)) != MDF_OK)
        goto done;
    if ((scenario == 0 && total.rows != 0U) ||
        (scenario != 0 && total.rows == 0U) ||
        state->renderer->finish_document(state->renderer) != MDF_OK ||
        total.rows != (size_t)rows + 1U)
      goto done;
    state->renderer->destroy(state->renderer);
    state->renderer = NULL;
  }
  rc = 0;
done:
  if (state->renderer != NULL)
    state->renderer->destroy(state->renderer);
  free(state);
  if (rc)
    fprintf(stderr, "full-layout table fallback lost layout or body rows\n");
  return rc;
}
typedef struct reasoning_capture {
  char bytes[2048];
  size_t length;
  int fail;
  int calls, fail_at;
} reasoning_capture;
static int reasoning_capture_body(void *context, const char *data,
                                  size_t length) {
  reasoning_capture *capture = context;
  capture->calls++;
  if (capture->fail ||
      (capture->fail_at != 0 && capture->calls == capture->fail_at) ||
      length >= sizeof(capture->bytes) - capture->length)
    return -1;
  memcpy(capture->bytes + capture->length, data, length);
  capture->length += length;
  capture->bytes[capture->length] = '\0';
  return 0;
}
static int test_reasoning_quotes(void) {
  static const char *inputs[] = {
      "",
      "one",
      "one\n",
      "one\r\n\r\ntwo\rthree\n",
      "**Bold**\n\n- item with `code`\n\n```c\nint x;\n```",
      "caf\303\251\n> nested\n"};
  static const char *expected[] = {
      "",
      "> one",
      "> one\n",
      "> one\n> \n> two\n> three\n",
      "> **Bold**\n> \n> - item with `code`\n> \n> ```c\n> int x;\n> ```",
      "> caf\303\251\n> > nested\n"};
  cai_cli_reasoning_quote quote;
  reasoning_capture capture;
  size_t n, chunk, offset, count, length;
  int fail_at;
  for (n = 0U; n < sizeof(inputs) / sizeof(inputs[0]); n++) {
    length = strlen(inputs[n]);
    for (chunk = 1U; chunk <= (length != 0U ? length : 1U); chunk++) {
      cai_cli_reasoning_quote_init(&quote);
      memset(&capture, 0, sizeof(capture));
      if (cai_cli_reasoning_quote_feed(&quote, NULL, 0U, reasoning_capture_body,
                                       &capture) != 0)
        return 1;
      for (offset = 0U; offset < length; offset += count) {
        count = length - offset;
        if (count > chunk)
          count = chunk;
        if (cai_cli_reasoning_quote_feed(&quote, inputs[n] + offset, count,
                                         reasoning_capture_body, &capture) != 0)
          return 1;
      }
      if (strcmp(capture.bytes, expected[n]) != 0) {
        fprintf(stderr, "quoted Markdown case %lu chunk %lu: %s\n",
                (unsigned long)n, (unsigned long)chunk, capture.bytes);
        return 1;
      }
    }
  }
  cai_cli_reasoning_quote_init(&quote);
  memset(&capture, 0, sizeof(capture));
  if (cai_cli_reasoning_quote_feed(&quote, "partial", 7U,
                                   reasoning_capture_body, &capture) != 0 ||
      strcmp(capture.bytes, "> partial") != 0)
    return 1; /* Output arrives without waiting for a newline or section EOF. */
  for (fail_at = 1; fail_at <= 3; fail_at++) {
    cai_cli_reasoning_quote_init(&quote);
    memset(&capture, 0, sizeof(capture));
    capture.fail_at = fail_at;
    if (cai_cli_reasoning_quote_feed(&quote, "line\n", 5U,
                                     reasoning_capture_body, &capture) != -1 ||
        capture.calls != fail_at)
      return 1;
  }
  if (cai_cli_reasoning_quote_feed(&quote, NULL, 1U, reasoning_capture_body,
                                   &capture) != -1 ||
      cai_cli_reasoning_quote_feed(NULL, "text", 4U, reasoning_capture_body,
                                   &capture) != -1)
    return 1;
  return 0;
}
static int test_reasoning_sections(void) {
  static const char *inputs[] = {"**Applying patch**",
                                 "**Applying patch**\n\n",
                                 "\n  **Applying patch**\r\n\n",
                                 "**Applying patch**\n\nBody å🚀\nNext",
                                 "**Applying patch**\r\n  Body å🚀\nNext",
                                 "**Å🚀 title**\nbody"};
  static const char *bodies[] = {
      "", "", "", "Body å🚀\nNext", "  Body å🚀\nNext", "body"};
  static const char *titles[] = {"Applying patch", "Applying patch",
                                 "Applying patch", "Applying patch",
                                 "Applying patch", "Å🚀 title"};
  cai_cli_reasoning reasoning;
  reasoning_capture capture;
  const char *title;
  char long_header[1024];
  size_t n, chunk, offset, count;
  for (n = 0U; n < sizeof(inputs) / sizeof(inputs[0]); n++) {
    for (chunk = 1U; chunk <= strlen(inputs[n]); chunk++) {
      cai_cli_reasoning_reset(&reasoning);
      memset(&capture, 0, sizeof(capture));
      for (offset = 0U; offset < strlen(inputs[n]); offset += count) {
        count = strlen(inputs[n]) - offset;
        if (count > chunk)
          count = chunk;
        if (cai_cli_reasoning_feed(&reasoning, inputs[n] + offset, count,
                                   reasoning_capture_body, &capture) != 0 ||
            cai_cli_reasoning_title(&reasoning, &title) != 0) {
          fprintf(stderr,
                  "fragmented heading failed at section %lu offset %lu\n",
                  (unsigned long)n, (unsigned long)offset);
          return 1;
        }
      }
      if (cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, titles[n]) != 0 ||
          strcmp(capture.bytes, bodies[n]) != 0) {
        fprintf(stderr, "section %lu chunk %lu: title=%s body=%s\n",
                (unsigned long)n, (unsigned long)chunk, title, capture.bytes);
        return 1;
      }
      cai_cli_reasoning_end(&reasoning);
      if (cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, titles[n]) != 0)
        return 1;
      if (cai_cli_reasoning_feed(&reasoning, "# Fresh", 7U,
                                 reasoning_capture_body, &capture) != 0 ||
          cai_cli_reasoning_title(&reasoning, &title) != 0 ||
          strcmp(title, "# Fresh") != 0) {
        fprintf(stderr, "fresh heading: %s\n", title);
        return 1;
      }
    }
  }
  memset(long_header, 'x', sizeof(long_header));
  cai_cli_reasoning_reset(&reasoning);
  memset(&capture, 0, sizeof(capture));
  if (cai_cli_reasoning_feed(&reasoning, long_header, sizeof(long_header),
                             reasoning_capture_body, &capture) != 0 ||
      reasoning.header_length != sizeof(reasoning.header) - 1U ||
      capture.length != 0U ||
      cai_cli_reasoning_feed(&reasoning, "\nbody", 5U, reasoning_capture_body,
                             &capture) != 0 ||
      strcmp(capture.bytes, "body") != 0)
    return 1;
  /* The body is delivered before section completion; sink failures propagate.
   */
  capture.fail = 1;
  if (cai_cli_reasoning_feed(&reasoning, "more", 4U, reasoning_capture_body,
                             &capture) != -1)
    return 1;
  cai_cli_reasoning_reset(&reasoning);
  if (cai_cli_reasoning_feed(&reasoning, "title\n  body", 12U,
                             reasoning_capture_body, &capture) != -1)
    return 1;
  cai_cli_reasoning_reset(&reasoning);
  if (cai_cli_reasoning_title(&reasoning, &title) != 0 || title[0] != '\0')
    return 1;
  return 0;
}

static int literal_capture_write(void *context, const char *data,
                                 size_t length) {
  size_t written;
  return event_log_write(context, data, length, &written);
}
static void without_spaces(char *out, const char *input) {
  while (*input != '\0') {
    if (*input != ' ' && *input != '\n' && *input != '\r' && *input != '\t')
      *out++ = *input;
    input++;
  }
  *out = '\0';
}
static int test_literal_tokens(void) {
  static const char *inputs[] = {
      "**stars** _path_ `tick` [name](url) <tag> {value} a+b-1.! \\path &amp;",
      "café λ å🚀 **stars** _path_ `tick` &amp;",
      "invalid \xff**stars** \xc3[_path_]",
      "control \033[2J **stars**",
      "first\nsecond\n\nlast",
      "foo_bar_baz a\xc2\xa0"
      "b",
      ""};
  static const char *expected[] = {
      "**stars** _path_ `tick` [name](url) <tag> {value} a+b-1.! \\path &amp;",
      "café λ å🚀 **stars** _path_ `tick` &amp;",
      "invalid ?**stars** ?[_path_]",
      "control [2J **stars**",
      "first\nsecond\n\nlast",
      "foo_bar_baz a\xc2\xa0"
      "b",
      ""};
  struct literal_fixture {
    event_log_capture capture;
    char actual_text[16384], expected_text[16384], long_text[6001];
  } *fixture = calloc(1U, sizeof(*fixture));
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  size_t i, n, columns;
  int rc = 0;
  if (fixture == NULL)
    return 1;
  mdf_options_init(&options);
  options.width = 40;
  options.boring = 1;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK) {
    free(fixture);
    return 1;
  }
  memset(&fixture->capture, 0, sizeof(fixture->capture));
  sink.userdata = &fixture->capture;
  sink.write = literal_capture_write;
  if (renderer->set_sink(renderer, &sink) != MDF_OK) {
    renderer->destroy(renderer);
    free(fixture);
    return 1;
  }
  for (i = 0U; i < sizeof(fixture->long_text) - 1U; i++)
    fixture->long_text[i] = "a*\\_[x]&"[i % (sizeof("a*\\_[x]&") - 1U)];
  fixture->long_text[i] = '\0';
  for (n = 0U; n <= sizeof(inputs) / sizeof(inputs[0]); n++) {
    const char *input =
        n < sizeof(inputs) / sizeof(inputs[0]) ? inputs[n] : fixture->long_text;
    const char *wanted = n < sizeof(inputs) / sizeof(inputs[0])
                             ? expected[n]
                             : fixture->long_text;
    memset(&fixture->capture, 0, sizeof(fixture->capture));
    if (cai_cli_literal_render(renderer, input) != 0) {
      rc = 1;
      break;
    }
    without_spaces(fixture->actual_text, fixture->capture.bytes);
    without_spaces(fixture->expected_text, wanted);
    if (strcmp(fixture->actual_text, fixture->expected_text) != 0 ||
        strchr(fixture->capture.bytes, '\033') != NULL) {
      fprintf(stderr, "literal token case %lu changed text\n",
              (unsigned long)n);
      rc = 1;
      break;
    }
    if (n == 1U)
      continue; /* Unicode display width is also checked by the PTY test. */
    columns = 0U;
    for (i = 0U; i < fixture->capture.length; i++) {
      if (fixture->capture.bytes[i] == '\n')
        columns = 0U;
      else if (++columns > 40U) {
        fprintf(stderr, "literal token case %lu exceeded wrap width\n",
                (unsigned long)n);
        rc = 1;
        break;
      }
    }
    if (rc != 0)
      break;
  }
  renderer->destroy(renderer);
  free(fixture);
  return rc;
}

static int test_markdown_identifiers(void) {
  static const char input[] = "foo_bar_baz *foo_bar_baz* a b_c_d b *a b_c_d b* "
                              "foo___bar___baz *foo___bar___baz*\n";
  static const char text[] = "foo_bar_baz foo_bar_baz a b_c_d b a b_c_d b "
                             "foo___bar___baz foo___bar___baz\n";
  event_log_capture *capture = calloc(1U, sizeof(*capture));
  mdf_options options;
  mdf_sink sink;
  mdf *renderer = NULL;
  char actual[512], expected[512];
  size_t span, at;
  int failed = 1;
  if (capture == NULL)
    return 1;
  mdf_options_init(&options);
  options.width = 240;
  options.boring = 1;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK)
    goto done;
  sink.userdata = capture;
  sink.write = literal_capture_write;
  if (renderer->set_sink(renderer, &sink) != MDF_OK)
    goto done;
  without_spaces(expected, text);
  for (span = 1U; span <= sizeof(input); ++span) {
    memset(capture, 0, sizeof(*capture));
    if (span > 1U && renderer->begin_document(renderer) != MDF_OK)
      goto done;
    for (at = 0U; at < sizeof(input) - 1U;) {
      size_t length = sizeof(input) - 1U - at;
      if (length > span)
        length = span;
      if (renderer->feed(renderer, input + at, length) != MDF_OK)
        goto done;
      at += length;
    }
    if (renderer->finish_document(renderer) != MDF_OK ||
        capture->length >= sizeof(actual))
      goto done;
    without_spaces(actual, capture->bytes);
    if (strcmp(actual, expected) != 0) {
      fprintf(stderr, "fragment size %lu: expected %s, got %s\n",
              (unsigned long)span, expected, actual);
      goto done;
    }
  }
  failed = 0;
done:
  if (failed)
    fprintf(stderr,
            "Markdown intraword underscores changed during rendering\n");
  if (renderer != NULL)
    renderer->destroy(renderer);
  free(capture);
  return failed;
}
static int test_literal_fragments(void) {
  static const char input[] =
      "prefix **stars** _path_ `tick` [name](url) &amp; \\x café λ🚀\n"
      "controls\0\033\xc2\x9b end\xc3";
  static const char expected[] =
      "prefix **stars** _path_ `tick` [name](url) &amp; \\x café λ🚀\n"
      "controls   end?";
  event_log_capture *capture = calloc(1U, sizeof(*capture));
  cai_cli_literal literal;
  mdf_options options;
  mdf_sink sink;
  mdf *renderer = NULL;
  size_t span, at;
  char actual[512], wanted[512], reference[512];
  int rc = 1;
  if (capture == NULL)
    return 1;
  mdf_options_init(&options);
  options.width = 240;
  options.boring = 1;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK)
    goto done;
  sink.userdata = capture;
  sink.write = literal_capture_write;
  if (renderer->set_sink(renderer, &sink) != MDF_OK)
    goto done;
  cai_cli_literal_init(&literal, renderer);
  if (cai_cli_literal_feed(&literal, input, sizeof(input) - 1U) != 0 ||
      cai_cli_literal_finish(&literal) != 0 ||
      renderer->finish(renderer) != MDF_OK ||
      capture->length >= sizeof(reference))
    goto done;
  memcpy(reference, capture->bytes, capture->length + 1U);
  without_spaces(wanted, expected);
  for (span = 1U; span <= 7U; ++span) {
    memset(capture, 0, sizeof(*capture));
    cai_cli_literal_init(&literal, renderer);
    for (at = 0U; at < sizeof(input) - 1U;) {
      size_t n = sizeof(input) - 1U - at;
      if (n > span)
        n = span;
      if (cai_cli_literal_feed(&literal, input + at, n) != 0)
        goto done;
      at += n;
    }
    /* Complete words flow before field completion, without Markdown escapes
     * or interpreting literal stars/entities/links at fragment boundaries. */
    if (strstr(capture->bytes, "**stars**") == NULL ||
        cai_cli_literal_finish(&literal) != 0 ||
        renderer->finish(renderer) != MDF_OK)
      goto done;
    without_spaces(actual, capture->bytes);
    if (strcmp(capture->bytes, reference) != 0 || strcmp(actual, wanted) != 0 ||
        strchr(capture->bytes, '\033') != NULL ||
        cai_cli_literal_feed(&literal, "late", 4U) != -1 ||
        cai_cli_literal_finish(&literal) != -1)
      goto done;
  }
  capture->length = sizeof(capture->bytes) - 1U;
  cai_cli_literal_init(&literal, renderer);
  if (cai_cli_literal_feed(&literal, "sink failure\n", 13U) != -1)
    goto done;
  capture->length = 0U;
  if (cai_cli_literal_feed(&literal, "retry", 5U) != -1 ||
      cai_cli_literal_finish(&literal) != -1 || capture->length != 0U)
    goto done;
  rc = 0;
done:
  if (rc != 0)
    fprintf(stderr, "fragmented literal rendering failed\n");
  if (renderer != NULL)
    renderer->destroy(renderer);
  free(capture);
  return rc;
}

static int test_reasoning_replay(void) {
  static const char *const types[] = {
      "assistant_text_delta",      "reasoning_summary_started",
      "reasoning_summary",         "reasoning_summary",
      "reasoning_summary",         "reasoning_summary_started",
      "reasoning_summary",         "assistant_text_end",
      "assistant_text_delta",      "assistant_text_end",
      "reasoning_summary_started", "reasoning_summary",
      "assistant_text_end"};
  static const char *const values[] = {"Commentary.",
                                       NULL,
                                       "**First heading**\r",
                                       "\n\nBody **one** λ ",
                                       "continues.",
                                       NULL,
                                       "**Second heading**\n\nBody two.",
                                       NULL,
                                       "Answer.",
                                       NULL,
                                       NULL,
                                       "**Title only**",
                                       NULL};
  cai_cli_presenter state;
  cai_transcript_event event = {0};
  cai_cli_transcript_presenter *transcript = NULL;
  reasoning_capture capture;
  mdf_options options;
  mdf_sink sink;
  size_t i;
  int rc = CAI_OK;
  memset(&state, 0, sizeof(state));
  memset(&event, 0, sizeof(event));
  memset(&capture, 0, sizeof(capture));
  state.width = 80;
  mdf_options_init(&options);
  options.width = state.width;
  options.margin_left = 2;
  options.ansi_mode = MDF_ANSI_OFF;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &state.renderer) != MDF_OK)
    return 1;
  sink.userdata = &capture;
  sink.write = reasoning_capture_body;
  if (state.renderer->set_sink(state.renderer, &sink) != MDF_OK)
    rc = CAI_ERR_TRANSPORT;
  if (rc == CAI_OK)
    rc = cai_cli_transcript_presenter_open(&state, &transcript, NULL);
  event.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  event.stream = 1U;
  if (rc == CAI_OK)
    rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
  for (i = 0U; rc == CAI_OK && i < sizeof(types) / sizeof(types[0]); i++) {
    event.type = types[i];
    event.data = values[i];
    event.sequence = (unsigned long long)i + 1U;
    event.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
    event.has_data = values[i] != NULL;
    event.data = NULL;
    event.length = event.offset = 0U;
    rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
    if (rc == CAI_OK && values[i] != NULL) {
      event.kind = CAI_TRANSCRIPT_DATA;
      event.data = values[i];
      event.length = strlen(values[i]);
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
      event.offset = event.length;
    }
    event.kind = CAI_TRANSCRIPT_EVENT_END;
    event.length = 0U;
    event.data = NULL;
    if (rc == CAI_OK)
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
  }
  cai_cli_transcript_presenter_close(transcript);
  state.renderer->destroy(state.renderer);
  /* This sink captures Markdown rendering alone. Softline block spacing is
   * exercised separately by the real-terminal replay regression. */
  if (rc != CAI_OK || state.reasoning_open || state.documents_rendered != 4 ||
      strstr(capture.bytes, "  Commentary.\n  > Body one λ continues.") ==
          NULL ||
      strstr(capture.bytes, "  > Body two.\n  Answer.") == NULL ||
      strstr(capture.bytes, "heading") != NULL ||
      strstr(capture.bytes, "Title only") != NULL) {
    fprintf(stderr, "reasoning replay lost sections: %s\n", capture.bytes);
    return 1;
  }
  return 0;
}

static int test_deferred_command_spacing(void) {
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  char output[256];
  int pipes[2], saved = -1, rc = 1;
  ssize_t count;
  if (state == NULL)
    return 1;
  if (pipe(pipes) != 0) {
    free(state);
    return 1;
  }
  saved = dup(STDOUT_FILENO);
  if (saved < 0 || dup2(pipes[1], STDOUT_FILENO) < 0)
    goto done;
  close(pipes[1]);
  pipes[1] = -1;
  if (fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0)
    goto done;
  state->sl = sl_create();
  state->width = 80;
  state->feed_breaks = 2;
  if (state->sl == NULL || sl_output_stream_begin(state->sl) != SL_OK ||
      cai_cli_presenter_renderer_create(state, &state->render_theme,
                                        &state->renderer, NULL) != CAI_OK ||
      command(state, "/delayed") != 0)
    goto done;
  count = read(pipes[0], output, sizeof(output));
  if (count != (ssize_t)strlen("$ /delayed\n") ||
      memcmp(output, "$ /delayed\n", (size_t)count) != 0)
    goto done;
  /* The same renderer adds the separator when a response finally arrives. */
  if (cai_cli_presenter_text(state, "Response arrived.",
                             strlen("Response arrived.")) != 0 ||
      cai_cli_presenter_finish_response(state) != 0)
    goto done;
  count = read(pipes[0], output, sizeof(output));
  if (count != (ssize_t)strlen("\nResponse arrived.\n") ||
      memcmp(output, "\nResponse arrived.\n", (size_t)count) != 0)
    goto done;
  /* Raw shell output shares that deferred boundary without inserting gaps
   * between subsequent chunks. */
  if (command(state, "!delayed") != 0)
    goto done;
  count = read(pipes[0], output, sizeof(output));
  if (count != (ssize_t)strlen("\n$ !delayed\n") ||
      memcmp(output, "\n$ !delayed\n", (size_t)count) != 0)
    goto done;
  if (literal(state, "first", 5U, NULL) != CAI_OK)
    goto done;
  count = read(pipes[0], output, sizeof(output));
  if (count != 6 || memcmp(output, "\nfirst", 6U) != 0)
    goto done;
  if (literal(state, "second", 6U, NULL) != CAI_OK)
    goto done;
  count = read(pipes[0], output, sizeof(output));
  if (count != 6 || memcmp(output, "second", 6U) != 0)
    goto done;
  rc = 0;
done:
  if (state->renderer != NULL)
    state->renderer->destroy(state->renderer);
  if (state->notice_renderer != NULL)
    state->notice_renderer->destroy(state->notice_renderer);
  if (state->sl != NULL) {
    (void)sl_output_stream_end(state->sl);
    sl_destroy(state->sl);
  }
  if (saved >= 0) {
    (void)dup2(saved, STDOUT_FILENO);
    close(saved);
  }
  if (pipes[1] >= 0)
    close(pipes[1]);
  close(pipes[0]);
  free(state);
  if (rc)
    fprintf(stderr, "command separator was not deferred until output\n");
  return rc;
}

static int test_timestamps(void) {
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  cai_error error;
  char output[4096], stamp[CAI_DTG_CAPACITY], expected[128];
  int pipes[2], saved, rc = 0;
  ssize_t count;
  if (state == NULL || pipe(pipes) != 0)
    return 1;
  saved = dup(STDOUT_FILENO);
  if (saved < 0 || dup2(pipes[1], STDOUT_FILENO) < 0)
    return 1;
  close(pipes[1]);
  state->sl = sl_create();
  state->interactive = 1;
  state->width = 80;
  state->utc = 1;
  state->presentation_event = 1;
  state->presentation_timestamp_ns = 1793622600123456789ULL;
  cai_error_init(&error);
  if (cai_cli_theme_parse("default", &state->theme) != NULL ||
      sl_output_stream_begin(state->sl) != SL_OK ||
      cai_cli_presenter_renderer_create(state, &state->theme, &state->renderer,
                                        &error) != CAI_OK ||
      cai_cli_presenter_timestamp(state, stamp, sizeof(stamp)) == 0 ||
      command(state, "/status") != 0 ||
      cai_cli_presenter_tool_markdown(state, "Tool result body.",
                                      strlen("Tool result body.")) != 0 ||
      cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_text(state, "Visible answer.", 15U) != 0 ||
      cai_cli_presenter_finish_response(state) != 0)
    rc = 1;
  state->presentation_timestamp_ns = 0U;
  if (cai_cli_presenter_timestamp(state, expected, sizeof(expected)) != 0)
    rc = 1;
  state->interactive = 0;
  state->presentation_event = 0;
  if (cai_cli_presenter_timestamp(state, expected, sizeof(expected)) != 0)
    rc = 1;
  state->renderer->destroy(state->renderer);
  if (state->notice_renderer != NULL)
    state->notice_renderer->destroy(state->notice_renderer);
  (void)sl_output_stream_end(state->sl);
  sl_destroy(state->sl);
  fflush(stdout);
  (void)dup2(saved, STDOUT_FILENO);
  close(saved);
  count = read(pipes[0], output, sizeof(output) - 1U);
  close(pipes[0]);
  if (count < 0)
    rc = 1;
  else {
    output[count] = '\0';
    snprintf(expected, sizeof(expected), "$ %s /status", stamp);
    if (memchr(output, '\0', (size_t)count) != NULL ||
        strstr(output, expected) == NULL || strstr(output, "/status") == NULL ||
        strstr(output, "Visible answer.") == NULL)
      rc = 1;
    snprintf(expected, sizeof(expected), "  %s%s%s\n",
             getenv("NO_COLOR") == NULL ? "\033[0;90m" : "", stamp,
             getenv("NO_COLOR") == NULL ? "\033[0m" : "");
    if (strstr(output, expected) == NULL ||
        strstr(strstr(output, expected) + strlen(expected), expected) != NULL ||
        strstr(output, "Tool result body.") == NULL ||
        strstr(output, "Tool result body.") > strstr(output, expected))
      rc = 1;
    if (rc != 0)
      fprintf(stderr, "timestamp rendering failed: %s\n", output);
  }
  cai_error_cleanup(&error);
  free(state);
  return rc;
}

/* Revoke the real Softline output descriptor, preserving the production
 * Markdown -> presenter -> editor path. Buffered table completion and replay
 * failures must propagate just like immediately visible output. */
static int test_output_failure(int operation) {
  cai_cli_presenter *state = calloc(1U, sizeof(*state));
  cai_cli_transcript_presenter *transcript = NULL;
  cai_transcript_event event = {0};
  sl_t *editor;
  sl_config_t config;
  mdf_options options;
  mdf_sink sink;
  int fds[2], rc = CAI_OK, failed = 1;
  const char *data = "Output unavailable.\n\n";
  const char *table = "| a | b |\n|---|---|\n| x | y |\n";
  if (state == NULL || pipe(fds) != 0)
    return 1;
  sl_config_init(&config);
  config.output_fd = fds[1];
  state->sl = sl_create_with_config(&config);
  state->width = 80;
  mdf_options_init(&options);
  options.width = 80;
  options.boring = 1;
  if (state->sl == NULL || sl_output_stream_begin(state->sl) != SL_OK ||
      mdf_create(MDF_FORMAT_ANSI, &options, &state->renderer) != MDF_OK)
    goto done;
  sink.userdata = state;
  sink.write = cai_cli_presenter_renderer_sink;
  if (state->renderer->set_sink(state->renderer, &sink) != MDF_OK)
    goto done;
  if (operation == 2 &&
      cai_cli_presenter_text(state, table, strlen(table)) != 0)
    goto done;
  close(fds[1]);
  fds[1] = -1;
  if (operation == 0)
    rc = cai_cli_presenter_text(state, data, strlen(data));
  else if (operation == 1)
    rc = cai_cli_presenter_reasoning_body(state, data, strlen(data));
  else if (operation == 2)
    rc = cai_cli_presenter_finish_response(state);
  else if (operation == 3)
    rc = cai_cli_presenter_prompt(state, "literal prompt");
  else if (operation == 4)
    rc = cai_cli_presenter_input(state, "/status", 1);
  else if (operation == 5)
    rc = cai_cli_presenter_notice(state, "* WARN: Output unavailable");
  else if (operation == 6) {
    rc = cai_cli_presenter_tool_markdown(state, data, strlen(data));
    if (rc == 0)
      rc = cai_cli_presenter_finish_response(state);
  } else {
    rc = cai_cli_transcript_presenter_open(state, &transcript, NULL);
    event.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
    event.stream = 1U;
    if (rc == CAI_OK)
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
    event.kind = CAI_TRANSCRIPT_EVENT_BEGIN;
    event.type = operation == 7   ? "assistant_text_delta"
                 : operation == 8 ? "reasoning_summary"
                                  : "input_presented";
    event.sequence = 1U;
    event.has_data = 1;
    if (rc == CAI_OK)
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
    event.kind = CAI_TRANSCRIPT_DATA;
    event.data = operation == 8 ? "Title\n\nOutput unavailable.\n\n" : data;
    event.length = strlen(event.data);
    if (rc == CAI_OK)
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
    event.kind = CAI_TRANSCRIPT_EVENT_END;
    event.offset = event.length;
    event.length = 0U;
    event.data = NULL;
    if (rc == CAI_OK)
      rc = cai_cli_transcript_presenter_event(transcript, &event, NULL);
  }
  failed = rc == CAI_OK;
done:
  if (failed)
    fprintf(stderr, "presenter output failure was lost: operation %d\n",
            operation);
  cai_cli_transcript_presenter_close(transcript);
  editor = state->sl;
  cai_cli_presenter_close(state);
  sl_destroy(editor);
  close(fds[0]);
  if (fds[1] >= 0)
    close(fds[1]);
  free(state);
  return failed;
}

static void *verify(void *context) {
  int operation;
  int *result = context;
  for (operation = 0; operation < 10; ++operation)
    if (test_output_failure(operation)) {
      *result = 1;
      return NULL;
    }
  *result = test_streaming_tables() || test_reasoning_sections() ||
            test_reasoning_quotes() || test_literal_tokens() ||
            test_markdown_identifiers() || test_literal_fragments() ||
            test_reasoning_replay() || test_deferred_command_spacing() ||
            test_timestamps();
  return NULL;
}

int main(void) {
  pthread_t thread;
  int result = 1;
  if (pthread_create(&thread, NULL, verify, &result) != 0)
    return 1;
  if (pthread_join(thread, NULL) != 0)
    return 1;
  return result;
}
