#ifndef CAI_CLI_THEME_H
#define CAI_CLI_THEME_H

#include "options.h"
#include <cai/cai.h>
#include <cai/display.h>
#include <libmdf/mdf.h>
#include <softline/softline.h>

typedef struct cai_cli_theme {
  sl_prompt_theme_t prompt;
  const char *markdown;
  int ascii;
} cai_cli_theme;

/* Borrowed canonical names; NULL means success, otherwise a static diagnostic.
 */
const char *cai_cli_theme_parse(const char *text, cai_cli_theme *out);
/** Frontend-only validation after the common configuration loader. */
int cai_cli_theme_validate(const cai_cli_options *, cai_error *);
const char *cai_cli_theme_prompt_name(const cai_cli_theme *theme);
void cai_cli_theme_markdown(const cai_cli_theme *theme, mdf_options *options);
/* Public libmdf palette: H3 activity, H2 labels; display owns copied colors. */
int cai_cli_theme_tools(const cai_cli_theme *theme, cai_tool_display *display,
                        cai_error *error);
/* Rotate in libmdf's published order, excluding boring; preserve prompt and
 * ANSI policy. Signed offsets are reduced before addition (including MIN). */
void cai_cli_theme_offset(const cai_cli_theme *base, long long offset,
                          cai_cli_theme *out);
int cai_cli_theme_list(cai_sink *sink, cai_error *error);
int cai_cli_theme_describe(const cai_cli_theme *theme, cai_sink *sink,
                           cai_error *error);

#endif
