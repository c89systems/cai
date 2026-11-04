#ifndef CAI_CLI_SOFTLINE_PRESENTER_H
#define CAI_CLI_SOFTLINE_PRESENTER_H
#include "reasoning.h"
#include "theme.h"
#include <cai/display.h>
/* Frontend-owned state. No runtime, store, controller, provider, command
 * admission or application lifecycle belongs here. sl is borrowed; configure
 * preferences before creating renderers. Initialize to zero, borrow the
 * editor, and set width/mode/theme explicitly. All calls belong to the
 * terminal owner thread. No operation starts model or application work. */
typedef struct cai_cli_presenter {
  sl_t *sl;
  int interactive, batch, utc;
  const char *tool_output;
  cai_cli_theme theme;
  cai_cli_theme render_theme;
  long long theme_offsets[3];
  int theme_scope; /* 0 parent, 1 ordinary child, 2 reviewer. */
  mdf *renderer;
  mdf *notice_renderer;
  mdf *prompt_renderer;
  cai_tool_display *tool_display;
  cai_sink *tool_sink;
  int width;
  unsigned short output_columns;
  unsigned short output_rows;
  int response_open;
  int documents_rendered;
  int reasoning_open;
  int block_spacing_pending;
  int feed_breaks;
  int feed_ansi;
  cai_cli_reasoning_quote reasoning_quote;
  int row_line_start;
  int row_ansi;
  int notice_open;
  int notice_first_row;
  int command_input;
  int command_output;
  int tool_result_body;
  int sink_failed;
  mdf_status renderer_error;
  unsigned long long presentation_timestamp_ns;
  int presentation_event;
  char command_foreground[MDF_THEME_FOREGROUND_SIZE];
} cai_cli_presenter;
int cai_cli_presenter_reconcile_output(cai_cli_presenter *state);
int cai_cli_presenter_sink(void *context, const char *bytes, size_t count);
int cai_cli_presenter_render_width(void);
int cai_cli_presenter_timestamp(cai_cli_presenter *state, char *out,
                                size_t capacity);
int cai_cli_presenter_notice_sink(void *context, const char *bytes,
                                  size_t count);
int cai_cli_presenter_renderer_sink(void *context, const char *bytes,
                                    size_t count);
int cai_cli_presenter_renderer_create(cai_cli_presenter *state,
                                      const cai_cli_theme *theme, mdf **out,
                                      cai_error *error);
int cai_cli_presenter_tool_display_init(cai_cli_presenter *state,
                                        cai_error *error);
int cai_cli_presenter_geometry(cai_cli_presenter *state);
int cai_cli_presenter_finish_response(cai_cli_presenter *state);
int cai_cli_presenter_finish_reasoning(cai_cli_presenter *state);
int cai_cli_presenter_reasoning_body(void *context, const char *data,
                                     size_t length);
int cai_cli_presenter_text(cai_cli_presenter *state, const char *data,
                           size_t length);
int cai_cli_presenter_introduction(cai_cli_presenter *state,
                                   const char *introduction, int margin_left,
                                   int margin_right);
int cai_cli_presenter_tool_markdown(void *context, const char *text,
                                    size_t length);
int cai_cli_presenter_input(cai_cli_presenter *state, const char *text,
                            int command);
int cai_cli_presenter_prompt(cai_cli_presenter *state, const char *text);
int cai_cli_presenter_notice(void *context, const char *text);
void cai_cli_presenter_theme_for_scope(const cai_cli_presenter *state,
                                       const cai_cli_theme *base, int scope,
                                       cai_cli_theme *out);
int cai_cli_presenter_set_theme_scope(cai_cli_presenter *state, int scope,
                                      cai_error *error);
int cai_cli_presenter_set_theme(cai_cli_presenter *, const cai_cli_theme *,
                                cai_error *);
/* Release rendering resources and reset state; never destroys borrowed sl. */
void cai_cli_presenter_close(cai_cli_presenter *);
#endif
