#include "theme.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
  const char *name;
  sl_prompt_theme_t prompt;
  const char *markdown;
} themes[] = {{"default", SL_PROMPT_THEME_DEFAULT, "default"},
              {"plain", SL_PROMPT_THEME_PLAIN, "ascii"},
              {"accent", SL_PROMPT_THEME_ACCENT, "night-owl"},
              {"dracula", SL_PROMPT_THEME_DRACULA, "dracula"},
              {"gruvbox", SL_PROMPT_THEME_GRUVBOX, "gruvbox"},
              {"monochrome", SL_PROMPT_THEME_MONOCHROME, "horizon"},
              {"monogreen", SL_PROMPT_THEME_MONOGREEN, "everforest"},
              {"outrun", SL_PROMPT_THEME_OUTRUN, "outrun-electric"},
              {"riced", SL_PROMPT_THEME_RICED, "catppuccin-mocha"},
              {"synthwave", SL_PROMPT_THEME_SYNTHWAVE, "synthwave-84"}};

int cai_cli_theme_completions(sl_completion_emit_t emit, void *context) {
  static const char *const commands[] = {"/theme", "/t"};
  size_t i, j, k, prompt_max = 0U, markdown_max = strlen("ascii");
  size_t count = sizeof(themes) / sizeof(themes[0]), capacity;
  char *candidate;
  int rc = SL_OK;
  if (emit == NULL)
    return SL_ERROR_INVALID;
  for (i = 0U; i < count; ++i)
    if (strlen(themes[i].name) > prompt_max)
      prompt_max = strlen(themes[i].name);
  for (i = 0U; i < mdf_theme_count(); ++i)
    if (strlen(mdf_theme_name(i)) > markdown_max)
      markdown_max = strlen(mdf_theme_name(i));
  capacity = strlen(commands[0]) + prompt_max + markdown_max + 3U;
  candidate = malloc(capacity);
  if (candidate == NULL)
    return SL_ERROR_NOMEM;
  /* Softline retains one phrase list per activation, so a typed space or
   * colon filters these candidates without another resolver or editor path. */
  for (k = 0U; k < sizeof(commands) / sizeof(commands[0]) && rc == SL_OK; ++k) {
    for (i = 0U; i <= count && rc == SL_OK; ++i) {
      const char *prompt = i < count ? themes[i].name : "";
      if (i < count) {
        snprintf(candidate, capacity, "%s %s", commands[k], prompt);
        rc = emit(candidate, context);
      }
      for (j = 0U; j < mdf_theme_count() + 2U && rc == SL_OK; ++j) {
        const char *markdown = j == 0U   ? ""
                               : j == 1U ? "ascii"
                                         : mdf_theme_name(j - 2U);
        snprintf(candidate, capacity, "%s %s:%s", commands[k], prompt,
                 markdown);
        rc = emit(candidate, context);
      }
    }
  }
  free(candidate);
  return rc;
}

static int theme_equal(const char *a, size_t length, const char *b) {
  size_t i;
  if (strlen(b) != length)
    return 0;
  for (i = 0U; i < length; i++) {
    unsigned char ch = (unsigned char)a[i];
    if (ch >= 'A' && ch <= 'Z')
      ch = (unsigned char)(ch - 'A' + 'a');
    if (ch != (unsigned char)b[i])
      return 0;
  }
  return 1;
}

int cai_cli_theme_validate(const cai_cli_options *options, cai_error *error) {
  cai_cli_theme selected;
  const char *reason = cai_cli_theme_parse(options->theme, &selected);
  return reason != NULL ? cai_cli_error(error, CAI_ERR_INVALID, reason)
                        : CAI_OK;
}

static const char *parse(const char *text, const cai_cli_theme *current,
                         cai_cli_theme *out) {
  const char *separator, *markdown;
  size_t length, i;
  cai_cli_theme selected;
  if (out == NULL)
    return "theme output is required";
  if (text == NULL)
    text = "default";
  separator = strchr(text, ':');
  length = separator != NULL ? (size_t)(separator - text) : strlen(text);
  if ((length == 0U && separator == NULL) ||
      (current == NULL && separator != NULL &&
       (length == 0U || separator[1] == '\0')))
    return "theme requires a softline name or SOFTLINE:LIBMDF; use "
           "--list-themes";
  if (separator != NULL && strchr(separator + 1U, ':') != NULL)
    return "theme must contain at most one ':'; use SOFTLINE:LIBMDF";
  if (length != 0U) {
    for (i = 0U; i < sizeof(themes) / sizeof(themes[0]); i++)
      if (theme_equal(text, length, themes[i].name))
        break;
    if (i == sizeof(themes) / sizeof(themes[0]))
      return "unknown softline theme; use --list-themes";
    selected.prompt = themes[i].prompt;
    markdown = separator != NULL ? separator + 1U : themes[i].markdown;
  } else {
    selected.prompt = current->prompt;
    markdown = separator + 1U;
  }
  if (markdown[0] == '\0') {
    selected.markdown = current->markdown;
    selected.ascii = current->ascii;
    *out = selected;
    return NULL;
  }
  selected.ascii = theme_equal(markdown, strlen(markdown), "ascii");
  selected.markdown = NULL;
  if (!selected.ascii) {
    for (i = 0U; i < mdf_theme_count(); i++) {
      const char *name = mdf_theme_name(i);
      if (theme_equal(markdown, strlen(markdown), name)) {
        selected.markdown = name;
        break;
      }
    }
    if (selected.markdown == NULL)
      return "unknown libmdf theme; use --list-themes";
  }
  *out = selected;
  return NULL;
}

const char *cai_cli_theme_parse(const char *text, cai_cli_theme *out) {
  return parse(text, NULL, out);
}
const char *cai_cli_theme_change(const char *text, const cai_cli_theme *current,
                                 cai_cli_theme *out) {
  return current != NULL ? parse(text, current, out)
                         : "current theme is required";
}

void cai_cli_theme_markdown(const cai_cli_theme *theme, mdf_options *options) {
  options->theme_name = theme->markdown;
  if (theme->ascii)
    options->ansi_mode = MDF_ANSI_OFF;
}

int cai_cli_theme_tools(const cai_cli_theme *theme, cai_tool_display *display,
                        cai_error *error) {
  mdf_theme_palette palette;
  if (theme == NULL ||
      mdf_theme_get_palette(theme->markdown, &palette) != MDF_OK) {
    if (error != NULL) {
      cai_error_cleanup(error);
      error->code = CAI_ERR_INVALID;
      error->message = strdup("cannot resolve libmdf tool palette");
    }
    return CAI_ERR_INVALID;
  }
  return cai_tool_display_set_palette(display, palette.heading[2],
                                      palette.heading[1], error);
}

void cai_cli_theme_offset(const cai_cli_theme *base, long long offset,
                          cai_cli_theme *out) {
  size_t i, count = 0U, index = 0U, current = 0U;
  long long step;
  *out = *base;
  if (offset == 0LL)
    return;
  for (i = 0U; i < mdf_theme_count(); i++) {
    const char *name = mdf_theme_name(i);
    if (strcmp(name, "boring") == 0)
      continue;
    if (strcmp(name, base->markdown != NULL ? base->markdown : "default") == 0)
      index = count;
    count++;
  }
  if (count == 0U)
    return;
  step = offset % (long long)count;
  if (step < 0LL)
    step += (long long)count;
  index = (index + (size_t)step) % count;
  for (i = 0U; i < mdf_theme_count(); i++) {
    const char *name = mdf_theme_name(i);
    if (strcmp(name, "boring") == 0)
      continue;
    if (current++ == index) {
      out->markdown = name;
      return;
    }
  }
}

const char *cai_cli_theme_prompt_name(const cai_cli_theme *theme) {
  size_t i;
  for (i = 0U; i < sizeof(themes) / sizeof(themes[0]); i++)
    if (themes[i].prompt == theme->prompt)
      return themes[i].name;
  return NULL;
}

int cai_cli_theme_describe(const cai_cli_theme *theme, cai_sink *sink,
                           cai_error *error) {
  char row[256];
  const char *text;
  size_t i;
  int n, rc;
  n = snprintf(row, sizeof(row), "## Theme\n\nCurrent: `%s:%s`\n\n",
               cai_cli_theme_prompt_name(theme),
               theme->ascii ? "ascii" : theme->markdown);
  rc = cai_sink_write(sink, row, (size_t)n, error);
  text = "Use `/theme NAME` or `/theme SOFTLINE:LIBMDF` at a safe "
         "response/tool boundary. Empty colon sides keep that component: "
         "`/theme :ayu-mirage` changes only libmdf; `/theme default:` changes "
         "only Softline. `/theme default` selects the complete default pair. "
         "Changes affect future output in this app invocation.\n\n";
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, text, strlen(text), error);
  if (rc == CAI_OK && getenv("NO_COLOR") != NULL) {
    text = "`NO_COLOR` is set: the prompt stays plain and Markdown ANSI "
           "styling stays disabled.\n\n";
    rc = cai_sink_write(sink, text, strlen(text), error);
  }
  text = "### Softline themes and pairings\n\n"
         "| Alias | Softline:libmdf |\n| --- | --- |\n";
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, text, strlen(text), error);
  for (i = 0U; rc == CAI_OK && i < sizeof(themes) / sizeof(themes[0]); i++) {
    n = snprintf(row, sizeof(row), "| `%s` | `%s:%s` |\n", themes[i].name,
                 themes[i].name, themes[i].markdown);
    rc = cai_sink_write(sink, row, (size_t)n, error);
  }
  text = "\n### Libmdf themes\n\n";
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, text, strlen(text), error);
  for (i = 0U; rc == CAI_OK && i < mdf_theme_count(); i++) {
    n = snprintf(row, sizeof(row), "%s`%s`", i != 0U ? ", " : "",
                 mdf_theme_name(i));
    rc = cai_sink_write(sink, row, (size_t)n, error);
  }
  text = ", `ascii`.\n\n`ascii` selects escape-free UTF-8 Markdown, "
         "not a color theme.\n";
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, text, strlen(text), error);
  return rc;
}

int cai_cli_theme_list(cai_sink *sink, cai_error *error) {
  static const char heading[] = "Softline themes and default libmdf pairings:\n"
                                "  NAME         SOFTLINE:LIBMDF\n";
  static const char middle[] = "\nlibmdf themes:\n";
  static const char ending[] =
      "  ascii (escape-free UTF-8 Markdown, not a color theme)\n\n"
      "Use --theme NAME for a pairing, or --theme SOFTLINE:LIBMDF.\n";
  char row[256];
  size_t i;
  int rc = cai_sink_write(sink, heading, sizeof(heading) - 1U, error);
  for (i = 0U; rc == CAI_OK && i < sizeof(themes) / sizeof(themes[0]); i++) {
    int n = snprintf(row, sizeof(row), "  %-12s %s:%s\n", themes[i].name,
                     themes[i].name, themes[i].markdown);
    rc = cai_sink_write(sink, row, (size_t)n, error);
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, middle, sizeof(middle) - 1U, error);
  for (i = 0U; rc == CAI_OK && i < mdf_theme_count(); i++) {
    const char *name = mdf_theme_name(i);
    rc = cai_sink_write(sink, "  ", 2U, error);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, name, strlen(name), error);
    if (rc == CAI_OK)
      rc = cai_sink_write(sink, "\n", 1U, error);
  }
  if (rc == CAI_OK)
    rc = cai_sink_write(sink, ending, sizeof(ending) - 1U, error);
  return rc;
}
