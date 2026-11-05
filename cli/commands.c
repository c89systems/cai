#include "commands.h"
#include <string.h>
int cai_cli_manual_review_call(const char *call) {
  return call != NULL &&
         (strcmp(call, "/review") == 0 || strncmp(call, "command:", 8U) == 0);
}
/* One policy inventory for the complete slash-command surface. Bare queries
 * can differ from mutations; neither Enter nor Alt-Enter changes the boundary
 * required by a command. Ordinary conversation input keeps its native intent.
 */
static const struct cli_command_definition {
  const char *name;
  cli_command_boundary bare, arguments;
} cli_command_definitions[] = {
    {"/help", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/status", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/log", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/tools", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/mcp", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/skills", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/quit", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/exit", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/new", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/resume", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/review", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/review-and-fix", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/model", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/reasoning", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/reasoning-summary", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/tier", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/fast", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/foreground", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/fg", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/background", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/bg", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST},
    {"/yield_time_ms", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/goal", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/theme", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/subagent-theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/review-theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST},
    {"/yolo", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/reasoning-tool", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/queue", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/todo", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/trim", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/export", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/compact", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/shell", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/shellshare", CLI_COMMAND_TURN, CLI_COMMAND_TURN},
    {"/autoqueue", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/output", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/history", CLI_COMMAND_FEED, CLI_COMMAND_FEED},
    {"/retention", CLI_COMMAND_FEED, CLI_COMMAND_TURN},
    {"/shell-context", CLI_COMMAND_FEED, CLI_COMMAND_FEED}};

static int cli_word_is(const char *text, size_t length, const char *word) {
  return strlen(word) == length && strncmp(text, word, length) == 0;
}

/* Expand only the command token. Queue policy and dispatch share this map;
 * the original alias stays in the editor, prompt history and input echo. */
static const struct {
  const char *alias, *name;
} cli_command_aliases[] = {
    {"/m", "/model"}, {"/r", "/reasoning"}, {"/t", "/theme"}};
const char *cai_cli_command_name(const char *line, size_t *length) {
  size_t i;
  for (i = 0U; i < sizeof(cli_command_aliases) / sizeof(cli_command_aliases[0]);
       i++)
    if (cli_word_is(line, *length, cli_command_aliases[i].alias)) {
      *length = strlen(cli_command_aliases[i].name);
      return cli_command_aliases[i].name;
    }
  return line;
}

int cai_cli_command_is_theme(const char *line) {
  size_t length = strcspn(line, " \t");
  const char *name = cai_cli_command_name(line, &length);
  return cli_word_is(name, length, "/theme") ||
         cli_word_is(name, length, "/theme-offset") ||
         cli_word_is(name, length, "/subagent-theme-offset") ||
         cli_word_is(name, length, "/review-theme-offset");
}

int cai_cli_command_pager(const char *line) {
  size_t n = strcspn(line, " \t"), length;
  const char *arg = line + n;
  arg += strspn(arg, " \t");
  length = strlen(arg);
  while (length > 0U && strchr(" \t", arg[length - 1U]) != NULL)
    --length;
  if (!cli_word_is(arg, length, "-p") && !cli_word_is(arg, length, "less"))
    return 0;
  return cli_word_is(line, n, "/help")  ? 1
         : cli_word_is(line, n, "/log") ? 2
                                        : 0;
}
int cai_cli_command_frontend(const char *line) {
  const char *name;
  size_t length = strcspn(line, " \t");
  name = cai_cli_command_name(line, &length);
  return cai_cli_command_pager(line) != 0 || cai_cli_command_is_theme(line) ||
         cli_word_is(name, length, "/output") ||
         cli_word_is(name, length, "/quit") ||
         cli_word_is(name, length, "/exit") ||
         cli_word_is(name, length, "/shell") ||
         cli_word_is(name, length, "/shellshare") || strcmp(line, "!sh") == 0 ||
         strcmp(line, "!sh+") == 0;
}

cli_command_boundary cai_cli_command_delivery(const char *line) {
  const char *arg, *name;
  size_t length, arg_length, word_length, i;
  if (line[0] == '!')
    return CLI_COMMAND_TURN;
  if (line[0] != '/')
    return CLI_COMMAND_PROMPT;
  length = strcspn(line, " \t");
  arg = line + length;
  arg += strspn(arg, " \t");
  name = cai_cli_command_name(line, &length);
  arg_length = strlen(arg);
  while (arg_length > 0U && strchr(" \t", arg[arg_length - 1U]) != NULL)
    arg_length--;
  word_length = strcspn(arg, " \t");
  if (cli_word_is(name, length, "/mcp") &&
      !cli_word_is(arg, word_length, "enable") &&
      !cli_word_is(arg, word_length, "disable") &&
      !cli_word_is(arg, word_length, "refresh"))
    return CLI_COMMAND_FEED;
  if (cli_word_is(name, length, "/goal") &&
      cli_word_is(arg, word_length, "start"))
    return CLI_COMMAND_TURN;
  if (cli_word_is(name, length, "/goal") &&
      (cli_word_is(arg, arg_length, "status") ||
       cli_word_is(arg, arg_length, "help")))
    return CLI_COMMAND_FEED;
  if (cli_word_is(name, length, "/tier") &&
      cli_word_is(arg, arg_length, "list"))
    return CLI_COMMAND_FEED;
  if (cli_word_is(name, length, "/todo") &&
      (cli_word_is(arg, arg_length, "list") ||
       cli_word_is(arg, arg_length, "help") ||
       cli_word_is(arg, word_length, "layout") ||
       cli_word_is(arg, word_length, "see")))
    return CLI_COMMAND_FEED;
  for (i = 0U;
       i < sizeof(cli_command_definitions) / sizeof(cli_command_definitions[0]);
       i++)
    if (cli_word_is(name, length, cli_command_definitions[i].name))
      return arg_length == 0U ? cli_command_definitions[i].bare
                              : cli_command_definitions[i].arguments;
  /* Reject unknown commands locally; they are never model steering. */
  return CLI_COMMAND_FEED;
}

size_t cai_cli_command_count(void) {
  return sizeof(cli_command_definitions) / sizeof(cli_command_definitions[0]) +
         sizeof(cli_command_aliases) / sizeof(cli_command_aliases[0]);
}
const char *cai_cli_command_at(size_t index) {
  size_t count =
      sizeof(cli_command_definitions) / sizeof(cli_command_definitions[0]);
  if (index < count)
    return cli_command_definitions[index].name;
  index -= count;
  return index < sizeof(cli_command_aliases) / sizeof(cli_command_aliases[0])
             ? cli_command_aliases[index].alias
             : NULL;
}
