#define _POSIX_C_SOURCE 200809L
#include "softline_presenter.h"
#include "error.h"
#include "literal.h"
#include <cai/dtg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Observe resize on the UI owner thread before entering a softline feed.
 * Include height-only changes and synchronous output after readline returns.
 * Ordinary fragments stay immediate; only changed geometry can need a CPR. */
int cai_cli_presenter_reconcile_output(cai_cli_presenter *state) {
  struct winsize size;
  if (state->batch || ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
      size.ws_col == 0 || size.ws_row == 0 ||
      (size.ws_col == state->output_columns &&
       size.ws_row == state->output_rows))
    return 0;
  if (sl_output_stream_reconcile(state->sl) != SL_OK)
    return -1;
  state->output_columns = size.ws_col;
  state->output_rows = size.ws_row;
  return 0;
}

int cai_cli_presenter_sink(void *context, const char *bytes, size_t count) {
  cai_cli_presenter *state;
  size_t i;
  state = (cai_cli_presenter *)context;
  if (count == 0U)
    return 0;
  if (state->batch) {
    if (fwrite(bytes, 1U, count, stdout) == count)
      return 0;
    state->sink_failed = 1;
    return -1;
  }
  if (cai_cli_presenter_reconcile_output(state) != 0 ||
      sl_output_stream_write(state->sl, bytes, count) != SL_OK) {
    state->sink_failed = 1;
    return -1;
  }
  for (i = 0U; i < count; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    if (state->feed_ansi == 1) {
      state->feed_ansi = ch == '[' ? 2 : 0;
    } else if (state->feed_ansi == 2) {
      if (ch >= 0x40U && ch <= 0x7eU)
        state->feed_ansi = 0;
    } else if (ch == 27U) {
      state->feed_ansi = 1;
    } else if (ch == '\n') {
      if (state->feed_breaks < 2)
        state->feed_breaks++;
    } else if (ch != '\r') {
      state->feed_breaks = 0;
    }
  }
  return 0;
}

int cai_cli_presenter_render_width(void) {
  return isatty(STDOUT_FILENO) ? mdf_terminal_width(STDOUT_FILENO, 80) : 80;
}

static int cai_cli_presenter_tool_sink(void *context, const void *bytes,
                                       size_t count, cai_error *error) {
  cai_cli_presenter *state = context;
  if (cai_cli_presenter_sink(context, (const char *)bytes, count) == 0) {
    if (count > 0U)
      state->block_spacing_pending = 1;
    return CAI_OK;
  }
  return cai_cli_error(error, CAI_ERR_TRANSPORT, "cannot write tool activity");
}

static int cai_cli_presenter_tool_begin_block(void *context, cai_error *error) {
  cai_cli_presenter *state = context;
  while (state->feed_breaks < 2) {
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot separate tool activity");
  }
  return CAI_OK;
}

/* Event scope prevents old records without timestamps from acquiring the
 * replay clock. Outside an observation, commands/notices use their display
 * time. No timestamp text enters the model, editable draft, or prompt history.
 */
int cai_cli_presenter_timestamp(cai_cli_presenter *state, char *out,
                                size_t capacity) {
  time_t when;
  time_t now = time(NULL);
  unsigned long long seconds;
  if (!state->interactive || state->batch || now == (time_t)-1)
    return 0;
  when = now;
  if (state->presentation_event) {
    if (state->presentation_timestamp_ns == 0U)
      return 0;
    seconds = state->presentation_timestamp_ns / 1000000000ULL;
    when = (time_t)seconds;
    if ((unsigned long long)when != seconds)
      return 0;
  }
  return cai_dtg_format_time(when, now, state->utc, out, capacity) ==
         CAI_DTG_OK;
}

static int cai_cli_presenter_timestamp_write(cai_cli_presenter *state,
                                             const char *stamp) {
  int color = !state->theme.ascii && getenv("NO_COLOR") == NULL;
  if (color && cai_cli_presenter_sink(state, "\033[0;90m", 7U) != 0)
    return -1;
  if (cai_cli_presenter_sink(state, stamp, strlen(stamp)) != 0)
    return -1;
  return color ? cai_cli_presenter_sink(state, "\033[0m", 4U) : 0;
}

static int cai_cli_presenter_timestamp_header(cai_cli_presenter *state) {
  char stamp[CAI_DTG_CAPACITY];
  if (state->tool_result_body ||
      (state->command_output && !state->presentation_event))
    return 0;
  if (!cai_cli_presenter_timestamp(state, stamp, sizeof(stamp)))
    return 0;
  while (state->feed_breaks < 2)
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      return -1;
  if (cai_cli_presenter_sink(state, "  ", 2U) != 0 ||
      cai_cli_presenter_timestamp_write(state, stamp) != 0 ||
      cai_cli_presenter_sink(state, "\n", 1U) != 0)
    return -1;
  state->block_spacing_pending = 0;
  return 0;
}

/* Prefix visible renderer rows without retaining body text or inserting a
 * marker inside an ANSI sequence split across writes. Blank rows stay blank. */
int cai_cli_presenter_notice_sink(void *context, const char *bytes,
                                  size_t count) {
  cai_cli_presenter *state = context;
  size_t i;
  size_t start = 0U;
  for (i = 0U; i < count; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    if (state->row_line_start && state->row_ansi == 0 && ch != 27U &&
        ch != '\n' && ch != '\r') {
      char styled_prefix[96];
      const char *prefix =
          state->notice_first_row ? (state->command_input ? "$ " : "* ") : "  ";
      if (state->interactive && !state->theme.ascii &&
          getenv("NO_COLOR") == NULL) {
        if (state->command_input) {
          snprintf(styled_prefix, sizeof(styled_prefix), "%s\033[0;1m%s",
                   !state->notice_first_row ? "  " : "\033[0;92m$\033[0m ",
                   state->command_foreground);
          prefix = styled_prefix;
        } else if (state->notice_first_row)
          prefix = "\033[91m*\033[0m ";
      }
      if (cai_cli_presenter_sink(state, bytes + start, i - start) != 0 ||
          cai_cli_presenter_sink(state, prefix, strlen(prefix)) != 0)
        return -1;
      if (state->notice_first_row) {
        char stamp[CAI_DTG_CAPACITY];
        if (cai_cli_presenter_timestamp(state, stamp, sizeof(stamp))) {
          if (cai_cli_presenter_timestamp_write(state, stamp) != 0 ||
              cai_cli_presenter_sink(state, " ", 1U) != 0)
            return -1;
          if (state->command_input && state->interactive &&
              !state->theme.ascii && getenv("NO_COLOR") == NULL &&
              (cai_cli_presenter_sink(state, "\033[0;1m", 6U) != 0 ||
               cai_cli_presenter_sink(state, state->command_foreground,
                                      strlen(state->command_foreground)) != 0))
            return -1;
        }
      }
      start = i;
      state->row_line_start = 0;
      state->notice_first_row = 0;
    }
    if (state->row_ansi == 1) {
      state->row_ansi = ch == '[' ? 2 : ch == ']' ? 3 : 0;
    } else if (state->row_ansi == 2) {
      if (ch >= 0x40U && ch <= 0x7eU)
        state->row_ansi = 0;
    } else if (state->row_ansi == 3) {
      if (ch == 7U)
        state->row_ansi = 0;
      else if (ch == 27U)
        state->row_ansi = 4;
    } else if (state->row_ansi == 4) {
      state->row_ansi = ch == '\\' ? 0 : 3;
    } else if (ch == 27U) {
      state->row_ansi = 1;
    } else if (ch == '\n') {
      state->row_line_start = 1;
    }
  }
  return cai_cli_presenter_sink(state, bytes + start, count - start);
}

/* Keep a single libmdf binding: replacing a sink discards its document state.
 */
int cai_cli_presenter_renderer_sink(void *context, const char *bytes,
                                    size_t count) {
  cai_cli_presenter *state = context;
  if (count > 0U && state->block_spacing_pending) {
    while (state->feed_breaks < 2)
      if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
        return -1;
    state->block_spacing_pending = 0;
  }
  return state->notice_open
             ? cai_cli_presenter_notice_sink(context, bytes, count)
             : cai_cli_presenter_sink(context, bytes, count);
}

int cai_cli_presenter_renderer_create(cai_cli_presenter *state,
                                      const cai_cli_theme *theme, mdf **out,
                                      cai_error *error) {
  mdf_options options;
  mdf_sink sink;
  mdf_status status;
  int width = state->width;
  mdf_options_init(&options);
  cai_cli_theme_markdown(theme, &options);
  options.output_fd = STDOUT_FILENO;
  if (getenv("NO_COLOR") != NULL)
    options.ansi_mode = MDF_ANSI_OFF;
  options.width = width;
  options.margin_left = state->interactive && width >= 5 ? 2 : 0;
  options.boring = !state->interactive;
  options.table_buffer_mode = MDF_TABLE_BUFFER_FULL;
  status = mdf_create(MDF_FORMAT_ANSI, &options, out);
  if (status != MDF_OK)
    return cai_cli_error(
        error, status == MDF_ERROR_NOMEM ? CAI_ERR_NOMEM : CAI_ERR_INVALID,
        "create Markdown renderer");
  sink.userdata = state;
  sink.write = cai_cli_presenter_renderer_sink;
  if ((*out)->set_sink(*out, &sink) != MDF_OK) {
    (*out)->destroy(*out);
    *out = NULL;
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "connect Markdown renderer");
  }
  return CAI_OK;
}

static int cai_cli_presenter_tool_display_create(cai_cli_presenter *state,
                                                 const cai_cli_theme *theme,
                                                 cai_tool_display **out,
                                                 cai_error *error) {
  cai_tool_display_config config;
  int rc, width;
  memset(&config, 0, sizeof(config));
  config.sink = state->tool_sink;
  config.mode =
      state->tool_output != NULL && strcmp(state->tool_output, "full") == 0
          ? CAI_TOOL_OUTPUT_FULL
          : CAI_TOOL_OUTPUT_COMPACT;
  config.color =
      state->interactive && !theme->ascii && getenv("NO_COLOR") == NULL;
  config.begin_block = cai_cli_presenter_tool_begin_block;
  config.begin_block_context = state;
  config.timestamps = state->interactive;
  config.utc = state->utc;
  rc = cai_tool_display_new(&config, out, error);
  if (rc == CAI_OK)
    rc = cai_cli_theme_tools(theme, *out, error);
  width = cai_cli_presenter_render_width();
  if (rc == CAI_OK)
    rc = cai_tool_display_set_width(*out,
                                    (size_t)(width < 32     ? 0
                                             : width > 4096 ? 4096
                                                            : width),
                                    error);
  if (rc != CAI_OK) {
    cai_tool_display_close(*out);
    *out = NULL;
  }
  return rc;
}

int cai_cli_presenter_tool_display_init(cai_cli_presenter *state,
                                        cai_error *error) {
  cai_sink_callbacks callbacks;
  int rc;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_cli_presenter_tool_sink;
  callbacks.context = state;
  rc = cai_sink_from_callbacks(&callbacks, &state->tool_sink, error);
  return rc == CAI_OK
             ? cai_cli_presenter_tool_display_create(
                   state, &state->render_theme, &state->tool_display, error)
             : rc;
}

int cai_cli_presenter_geometry(cai_cli_presenter *state) {
  int width;
  width = cai_cli_presenter_render_width();
  if (width == state->width) {
    return 0;
  }
  /* Softline owns native terminal sizing; only libmdf needs the new width. */
  if (state->renderer != NULL &&
      state->renderer->set_geometry(state->renderer, width,
                                    state->interactive && width >= 5 ? 2 : 0,
                                    0) != MDF_OK) {
    return -1;
  }
  if (state->prompt_renderer != NULL &&
      state->prompt_renderer->set_geometry(
          state->prompt_renderer, width,
          state->interactive && width >= 5 ? 2 : 0, 0) != MDF_OK)
    return -1;
  state->width = width;
  return 0;
}

static int cai_cli_presenter_mdf_status(cai_cli_presenter *state,
                                        mdf_status status) {
  if (status == MDF_OK)
    return 0;
  state->renderer_error = status;
  return -1;
}

int cai_cli_presenter_finish_response(cai_cli_presenter *state) {
  if (!state->response_open) {
    return 0;
  }
  if (cai_cli_presenter_mdf_status(
          state, state->renderer->finish_document(state->renderer)) != 0) {
    return -1;
  }
  state->documents_rendered++;
  state->response_open = 0;
  state->block_spacing_pending = 1;
  return 0;
}

int cai_cli_presenter_finish_reasoning(cai_cli_presenter *state) {
  if (!state->reasoning_open) {
    return 0;
  }
  if (!state->reasoning_quote.line_start &&
      state->renderer->feed(state->renderer, "\n", 1U) != MDF_OK)
    return -1;
  if (state->renderer->finish_document(state->renderer) != MDF_OK) {
    return -1;
  }
  state->documents_rendered++;
  state->reasoning_open = 0;
  state->block_spacing_pending = 1;
  return 0;
}

static int cai_cli_presenter_reasoning_markdown(void *context, const char *data,
                                                size_t length) {
  cai_cli_presenter *state = context;
  return cai_cli_presenter_mdf_status(
      state, state->renderer->feed(state->renderer, data, length));
}

int cai_cli_presenter_reasoning_body(void *context, const char *data,
                                     size_t length) {
  cai_cli_presenter *state = context;
  if (state->input_open)
    return -1;
  if (length == 0U)
    return 0;
  if (cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_geometry(state) != 0)
    return -1;
  if (!state->reasoning_open) {
    if (state->documents_rendered > 0 &&
        state->renderer->begin_document(state->renderer) != MDF_OK)
      return -1;
    if (cai_cli_presenter_timestamp_header(state) != 0)
      return -1;
    state->reasoning_open = 1;
    cai_cli_reasoning_quote_init(&state->reasoning_quote);
  }
  return cai_cli_reasoning_quote_feed(&state->reasoning_quote, data, length,
                                      cai_cli_presenter_reasoning_markdown,
                                      state);
}

int cai_cli_presenter_text(cai_cli_presenter *state, const char *data,
                           size_t length) {
  if (state->input_open)
    return -1;
  if (length == 0U)
    return 0;
  if (state->response_open &&
      state->response_tool != (state->tool_result_body != 0) &&
      cai_cli_presenter_finish_response(state) != 0)
    return -1;
  if (cai_cli_presenter_finish_reasoning(state) != 0) {
    return -1;
  }
  if (cai_cli_presenter_geometry(state) != 0) {
    return -1;
  }
  if (!state->response_open) {
    if (state->documents_rendered > 0 &&
        state->renderer->begin_document(state->renderer) != MDF_OK) {
      return -1;
    }
    if (cai_cli_presenter_timestamp_header(state) != 0)
      return -1;
    state->response_open = 1;
    state->response_tool = state->tool_result_body != 0;
  }
  return cai_cli_presenter_mdf_status(
      state, state->renderer->feed(state->renderer, data, length));
}

int cai_cli_presenter_introduction(cai_cli_presenter *state,
                                   const char *introduction, int margin_left,
                                   int margin_right) {
  mdf_options options;
  mdf_sink sink;
  mdf *renderer = NULL;
  int rc = -1;
  if (!state->interactive || introduction == NULL || introduction[0] == '\0')
    return 0;
  mdf_options_init(&options);
  cai_cli_theme_markdown(&state->render_theme, &options);
  options.output_fd = STDOUT_FILENO;
  if (getenv("NO_COLOR") != NULL)
    options.ansi_mode = MDF_ANSI_OFF;
  options.width = cai_cli_presenter_render_width();
  options.margin_left = margin_left;
  options.margin_right = margin_right;
  if (mdf_create(MDF_FORMAT_ANSI, &options, &renderer) != MDF_OK)
    return -1;
  sink.userdata = state;
  sink.write = cai_cli_presenter_renderer_sink;
  if (renderer->set_sink(renderer, &sink) == MDF_OK &&
      renderer->feed(renderer, introduction, strlen(introduction)) == MDF_OK &&
      renderer->finish_document(renderer) == MDF_OK)
    rc = 0;
  renderer->destroy(renderer);
  /* Separate the completed introduction block from conversation or editor
   * content, just as other feed blocks do. The raw banner is never changed. */
  while (rc == 0 && state->feed_breaks < 2)
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      rc = -1;
  return rc;
}

/* Manual text tokens have their own renderer, separate from model Markdown. */
static int cai_cli_presenter_literal_prepare(cai_cli_presenter *state) {
  mdf_options options;
  mdf_sink sink;
  char stamp[CAI_DTG_CAPACITY];
  int available = state->width - 2;
  if (state->notice_renderer == NULL) {
    mdf_options_init(&options);
    options.width = state->width;
    options.boring = 1;
    options.ansi_mode = MDF_ANSI_OFF;
    if (mdf_create(MDF_FORMAT_ANSI, &options, &state->notice_renderer) !=
        MDF_OK)
      return -1;
    sink.userdata = state;
    sink.write = cai_cli_presenter_renderer_sink;
    if (state->notice_renderer->set_sink(state->notice_renderer, &sink) !=
        MDF_OK)
      return -1;
  }
  if (cai_cli_presenter_timestamp(state, stamp, sizeof(stamp)))
    available -= (int)strlen(stamp) + 1;
  return state->notice_renderer->set_geometry(state->notice_renderer,
                                              available >= 3 ? available : 3, 0,
                                              0) == MDF_OK
             ? 0
             : -1;
}

int cai_cli_presenter_tool_markdown(void *context, const char *text,
                                    size_t length) {
  cai_cli_presenter *state = context;
  int saved = state->tool_result_body, rc;
  state->tool_result_body = 1;
  rc = cai_cli_presenter_text(state, text, length);
  state->tool_result_body = saved;
  return rc;
}

static int cai_cli_presenter_prompt_token(cai_cli_presenter *state,
                                          mdf_token_type type) {
  mdf_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  return cai_cli_presenter_mdf_status(
      state,
      state->prompt_renderer->write_token(state->prompt_renderer, &token));
}

/* The native path supplies a complete accepted prompt. User prompts have
 * literal semantics through write_literal; reasoning uses ordinary Markdown.
 * Word-based emission is intentional: socket spans must not force wrapping
 * decisions or quote boundaries, or accumulate into a whole prompt here. */
static int cai_cli_presenter_prompt_prepare(cai_cli_presenter *state) {
  if (cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_finish_reasoning(state) != 0 ||
      cai_cli_presenter_reconcile_output(state) != 0 ||
      cai_cli_presenter_geometry(state) != 0)
    return -1;
  if (state->prompt_renderer == NULL &&
      cai_cli_presenter_renderer_create(
          state, &state->render_theme, &state->prompt_renderer, NULL) != CAI_OK)
    return -1;
  state->block_spacing_pending = 1;
  return cai_cli_presenter_prompt_token(state, MDF_TOKEN_BLOCKQUOTE_START);
}

static int cai_cli_presenter_command_prepare(cai_cli_presenter *state) {
  if (cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_finish_reasoning(state) != 0 ||
      cai_cli_presenter_reconcile_output(state) != 0 ||
      cai_cli_presenter_geometry(state) != 0 ||
      cai_cli_presenter_literal_prepare(state) != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      return -1;
  if (mdf_theme_heading_foreground(state->render_theme.markdown, 2,
                                   state->command_foreground,
                                   sizeof(state->command_foreground)) != MDF_OK)
    return -1;
  state->notice_open = 1;
  state->notice_first_row = 1;
  state->command_input = 1;
  state->row_line_start = 1;
  state->row_ansi = 0;
  return 0;
}

int cai_cli_presenter_input_begin(cai_cli_presenter *state, int command) {
  if (state->input_open)
    return -1;
  state->input_open = 1;
  state->input_started = state->input_failed = 0;
  state->input_command = command != 0;
  return 0;
}

int cai_cli_presenter_input_write(cai_cli_presenter *state, const void *bytes,
                                  size_t length) {
  int rc;
  if (!state->input_open || state->input_failed)
    return -1;
  if (bytes == NULL && length != 0U) {
    state->input_failed = 1;
    return -1;
  }
  if (state->batch || length == 0U)
    return 0;
  if (!state->input_started) {
    rc = state->input_command ? cai_cli_presenter_command_prepare(state)
                              : cai_cli_presenter_prompt_prepare(state);
    if (rc != 0) {
      state->input_failed = 1;
      return -1;
    }
    cai_cli_literal_init(&state->input_literal, state->input_command
                                                    ? state->notice_renderer
                                                    : state->prompt_renderer);
    state->input_started = 1;
  }
  rc = cai_cli_literal_feed(&state->input_literal, bytes, length);
  if (rc != 0)
    state->input_failed = 1;
  return rc;
}

void cai_cli_presenter_input_abort(cai_cli_presenter *state) {
  if (!state->input_open)
    return;
  if (state->input_started || state->input_failed) {
    mdf *renderer =
        state->input_command ? state->notice_renderer : state->prompt_renderer;
    if (renderer != NULL)
      (void)mdf_reset(renderer);
    state->notice_open = state->command_input = 0;
    state->block_spacing_pending = 1;
  }
  memset(&state->input_literal, 0, sizeof(state->input_literal));
  state->input_open = state->input_started = state->input_failed = 0;
}

int cai_cli_presenter_input_end(cai_cli_presenter *state) {
  int rc;
  if (!state->input_open || state->input_failed)
    return -1;
  if (!state->input_started) {
    state->input_open = 0;
    return 0;
  }
  rc = cai_cli_literal_finish(&state->input_literal);
  if (!state->input_command) {
    if (rc == 0)
      rc = cai_cli_presenter_prompt_token(state, MDF_TOKEN_BLOCKQUOTE_END);
    if (rc == 0)
      rc = cai_cli_presenter_mdf_status(
          state, state->prompt_renderer->finish(state->prompt_renderer));
    if (rc != 0) {
      state->input_failed = 1;
      return -1;
    }
    state->input_open = state->input_started = 0;
    state->block_spacing_pending = 1;
    return 0;
  }
  if (rc == 0)
    rc = cai_cli_presenter_mdf_status(
        state, state->notice_renderer->finish(state->notice_renderer));
  state->notice_open = 0;
  state->command_input = 0;
  if (state->interactive && !state->theme.ascii && getenv("NO_COLOR") == NULL &&
      cai_cli_presenter_sink(state, "\033[0m", 4U) != 0)
    rc = -1;
  /* End the input line now. Separate a response only when it actually writes
   * to the feed; no-output commands must not leave an empty response row. */
  while (rc == 0 && state->feed_breaks < 1)
    rc = cai_cli_presenter_sink(state, "\n", 1U);
  if (rc == 0)
    state->block_spacing_pending = 1;
  if (rc != 0) {
    state->input_failed = 1;
    return -1;
  }
  state->input_open = state->input_started = 0;
  return rc;
}

int cai_cli_presenter_input(cai_cli_presenter *state, const char *text,
                            int command) {
  int rc = cai_cli_presenter_input_begin(state, command);
  if (rc != 0)
    return rc;
  rc = cai_cli_presenter_input_write(state, text, strlen(text));
  if (rc == 0)
    rc = cai_cli_presenter_input_end(state);
  if (rc != 0)
    cai_cli_presenter_input_abort(state);
  return rc;
}

int cai_cli_presenter_prompt(cai_cli_presenter *state, const char *text) {
  return cai_cli_presenter_input(state, text, 0);
}

int cai_cli_presenter_notice(void *context, const char *text) {
  cai_cli_presenter *state = (cai_cli_presenter *)context;
  int rc;
  if (state->input_open)
    return -1;
  if (cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_finish_reasoning(state) != 0 ||
      cai_cli_presenter_geometry(state) != 0)
    return -1;
  /* Keep one blank row around each notice; the renderer supplies its newline.
   */
  while (state->feed_breaks < 2)
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      return -1;
  if (cai_cli_presenter_literal_prepare(state) != 0)
    return -1;
  state->notice_open = 1;
  state->notice_first_row = 1;
  state->row_line_start = 1;
  state->row_ansi = 0;
  if (strncmp(text, "* ", 2U) == 0)
    text += 2;
  rc = cai_cli_literal_render(state->notice_renderer, text);
  state->notice_open = 0;
  if (rc != 0)
    return -1;
  while (state->feed_breaks < 2)
    if (cai_cli_presenter_sink(state, "\n", 1U) != 0)
      return -1;
  return 0;
}

void cai_cli_presenter_theme_for_scope(const cai_cli_presenter *state,
                                       const cai_cli_theme *base, int scope,
                                       cai_cli_theme *out) {
  cai_cli_theme parent;
  cai_cli_theme_offset(base, state->theme_offsets[0], &parent);
  cai_cli_theme_offset(&parent,
                       scope == 2   ? state->theme_offsets[2]
                       : scope == 1 ? state->theme_offsets[1]
                                    : 0LL,
                       out);
}

int cai_cli_presenter_set_theme_scope(cai_cli_presenter *state, int scope,
                                      cai_error *error) {
  cai_cli_theme selected;
  mdf *renderer = NULL;
  int changed, rc;
  if (state->input_open)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "theme change requires a complete input field");
  cai_cli_presenter_theme_for_scope(state, &state->theme, scope, &selected);
  changed =
      selected.ascii != state->render_theme.ascii ||
      (selected.markdown != state->render_theme.markdown &&
       (selected.markdown == NULL || state->render_theme.markdown == NULL ||
        strcmp(selected.markdown, state->render_theme.markdown) != 0));
  if (!changed && state->theme_scope == scope)
    return CAI_OK;
  if (changed && state->renderer != NULL) {
    rc = cai_cli_presenter_renderer_create(state, &selected, &renderer, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (cai_cli_presenter_finish_response(state) != 0 ||
      cai_cli_presenter_finish_reasoning(state) != 0) {
    if (renderer != NULL)
      renderer->destroy(renderer);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "finish themed Markdown block");
  }
  if (changed && state->tool_display != NULL) {
    rc = cai_cli_theme_tools(&selected, state->tool_display, error);
    if (rc != CAI_OK) {
      if (renderer != NULL)
        renderer->destroy(renderer);
      return rc;
    }
  }
  if (renderer != NULL) {
    state->renderer->destroy(state->renderer);
    state->renderer = renderer;
    state->documents_rendered = 0;
  }
  if (changed && state->prompt_renderer != NULL) {
    state->prompt_renderer->destroy(state->prompt_renderer);
    state->prompt_renderer = NULL;
  }
  state->render_theme = selected;
  state->theme_scope = scope;

  return CAI_OK;
}

int cai_cli_presenter_set_theme(cai_cli_presenter *state,
                                const cai_cli_theme *theme, cai_error *error) {
  cai_cli_theme selected = *theme, effective;
  mdf *renderer = NULL;
  int rc;
  if (state->input_open)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "theme change requires a complete input field");
  /* The caller selects a presentation seam. Finish open blocks before
   * replacing renderer/theme state; no model or queue policy belongs here. */
  cai_cli_presenter_theme_for_scope(state, &selected, state->theme_scope,
                                    &effective);
  rc = cai_cli_presenter_renderer_create(state, &effective, &renderer, error);
  if (rc == CAI_OK &&
      (cai_cli_presenter_finish_response(state) != 0 ||
       cai_cli_presenter_finish_reasoning(state) != 0 ||
       renderer->set_geometry(renderer, state->width,
                              state->interactive && state->width >= 5 ? 2 : 0,
                              0) != MDF_OK ||
       sl_set_prompt_theme(state->sl, getenv("NO_COLOR") != NULL
                                          ? SL_PROMPT_THEME_PLAIN
                                          : selected.prompt) != SL_OK))
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "apply terminal theme");
  if (rc != CAI_OK) {
    if (renderer != NULL)
      renderer->destroy(renderer);
    return rc;
  }
  /* A terminal call can span several write_stdin tools. Keep its identity,
   * output limit, UTF-8/control decoder and pending command state intact. */
  rc = cai_cli_theme_tools(&effective, state->tool_display, error);
  if (rc == CAI_OK)
    rc = cai_tool_display_set_color(state->tool_display,
                                    state->interactive && !selected.ascii &&
                                        getenv("NO_COLOR") == NULL,
                                    error);
  if (rc != CAI_OK) {
    renderer->destroy(renderer);
    return rc;
  }
  state->renderer->destroy(state->renderer);
  state->renderer = renderer;
  if (state->prompt_renderer != NULL) {
    state->prompt_renderer->destroy(state->prompt_renderer);
    state->prompt_renderer = NULL;
  }
  state->theme = selected;
  state->render_theme = effective;
  state->documents_rendered = 0;
  return CAI_OK;
}
void cai_cli_presenter_close(cai_cli_presenter *state) {
  if (state == NULL)
    return;
  cai_cli_presenter_input_abort(state);
  (void)cai_cli_presenter_finish_response(state);
  (void)cai_cli_presenter_finish_reasoning(state);
  if (state->renderer != NULL)
    state->renderer->destroy(state->renderer);
  if (state->notice_renderer != NULL)
    state->notice_renderer->destroy(state->notice_renderer);
  if (state->prompt_renderer != NULL)
    state->prompt_renderer->destroy(state->prompt_renderer);
  cai_tool_display_close(state->tool_display);
  cai_sink_close(state->tool_sink);
  memset(state, 0, sizeof(*state));
}
