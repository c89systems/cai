#include "commands.h"
#include <string.h>
int cai_cli_manual_review_call(const char *call) {
  return call != NULL &&
         (strcmp(call, "/review") == 0 || strncmp(call, "command:", 8U) == 0);
}
/* One policy inventory for the complete slash-command surface. Bare queries
 * can differ from mutations; neither Enter nor Alt-Enter changes the boundary
 * required by a command. Ordinary session input keeps its native intent.
 */
static const struct cli_command_definition {
  const char *name;
  cli_command_boundary bare, arguments;
  int observer; /* 1=bare query, 2=attachment-local arguments as well. */
} cli_command_definitions[] = {
    {"/help", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1},
    {"/status", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1},
    {"/log", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1},
    {"/tools", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/mcp", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/skills", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1},
    {"/quit", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 2},
    {"/exit", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 2},
    {"/terminate", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 2},
    {"/new", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/resume", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/review", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/review-and-fix", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/model", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/reasoning", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/reasoning-summary", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/tier", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/fast", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/foreground", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/fg", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/background", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/bg", CLI_COMMAND_REQUEST, CLI_COMMAND_REQUEST, 0},
    {"/yield_time_ms", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/goal", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 1},
    {"/theme", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 2},
    {"/theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 2},
    {"/subagent-theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 2},
    {"/review-theme-offset", CLI_COMMAND_FEED, CLI_COMMAND_REQUEST, 2},
    {"/yolo", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/reasoning-tool", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/queue", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/todo", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/trim", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/export", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/compact", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/shell", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/shellshare", CLI_COMMAND_TURN, CLI_COMMAND_TURN, 0},
    {"/autoqueue", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 0},
    {"/output", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 2},
    {"/history", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1},
    {"/retention", CLI_COMMAND_FEED, CLI_COMMAND_TURN, 1},
    {"/shell-context", CLI_COMMAND_FEED, CLI_COMMAND_FEED, 1}};

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

void cai_cli_command_parse(const char *line, cai_cli_command_tokens *tokens) {
  tokens->name_length = strcspn(line, " \t");
  tokens->arguments = line + tokens->name_length;
  tokens->arguments += strspn(tokens->arguments, " \t");
  tokens->name = cai_cli_command_name(line, &tokens->name_length);
  tokens->arguments_length = strlen(tokens->arguments);
  while (tokens->arguments_length != 0U &&
         strchr(" \t", tokens->arguments[tokens->arguments_length - 1U]) !=
             NULL)
    --tokens->arguments_length;
}

int cai_cli_command_is_theme(const char *line) {
  size_t length = strcspn(line, " \t");
  const char *name = cai_cli_command_name(line, &length);
  return cli_word_is(name, length, "/theme") ||
         cli_word_is(name, length, "/theme-offset") ||
         cli_word_is(name, length, "/subagent-theme-offset") ||
         cli_word_is(name, length, "/review-theme-offset");
}
int cai_cli_command_quit(const char *line) {
  cai_cli_command_tokens tokens;
  if (line == NULL)
    return 0;
  cai_cli_command_parse(line, &tokens);
  if (cli_word_is(tokens.name, tokens.name_length, "/terminate"))
    return tokens.arguments_length == 0U ? 2 : -1;
  if (!cli_word_is(tokens.name, tokens.name_length, "/quit") &&
      !cli_word_is(tokens.name, tokens.name_length, "/exit"))
    return 0;
  if (tokens.arguments_length == 0U)
    return 1;
  if (cli_word_is(tokens.arguments, tokens.arguments_length, "now") ||
      cli_word_is(tokens.arguments, tokens.arguments_length, "force") ||
      cli_word_is(tokens.arguments, tokens.arguments_length, "-f") ||
      cli_word_is(tokens.arguments, tokens.arguments_length, "--force"))
    return 2;
  return -1;
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
         cli_word_is(name, length, "/terminate") ||
         cli_word_is(name, length, "/shell") ||
         cli_word_is(name, length, "/shellshare") || strcmp(line, "!sh") == 0 ||
         strcmp(line, "!sh+") == 0;
}
int cai_cli_command_observer(const char *line) {
  const char *name, *argument;
  const unsigned char *p;
  size_t length, size, word, i;
  if (line == NULL || line[0] != '/')
    return 0;
  /* Handler tokenizers also recognize CR/LF and other ASCII whitespace.
   * Observer admission must not reinterpret a multiline "name" as an action. */
  for (p = (const unsigned char *)line; *p != '\0'; ++p)
    if ((*p < 32U && *p != '\t') || *p == 127U)
      return 0;
  if (cai_cli_command_pager(line) != 0)
    return 1;
  length = strcspn(line, " \t");
  argument = line + length;
  argument += strspn(argument, " \t");
  size = strlen(argument);
  while (size > 0U && strchr(" \t", argument[size - 1U]) != NULL)
    --size;
  word = strcspn(argument, " \t");
  name = cai_cli_command_name(line, &length);
  if (cli_word_is(name, length, "/goal")) {
    if (cli_word_is(argument, size, "status") ||
        cli_word_is(argument, size, "help"))
      return 1;
  } else if (cli_word_is(name, length, "/tier"))
    return cli_word_is(argument, size, "list"); /* Bare /tier cycles routing. */
  else if (cli_word_is(name, length, "/mcp") && size != 0U)
    return word == size && !cli_word_is(argument, word, "enable") &&
           !cli_word_is(argument, word, "disable") &&
           !cli_word_is(argument, word, "refresh");
  else if (cli_word_is(name, length, "/todo") && size != 0U)
    return cli_word_is(argument, size, "help") ||
           cli_word_is(argument, size, "list") ||
           cli_word_is(argument, size, "layout") ||
           cli_word_is(argument, word, "see");
  for (i = 0U;
       i < sizeof(cli_command_definitions) / sizeof(cli_command_definitions[0]);
       ++i)
    if (cli_word_is(name, length, cli_command_definitions[i].name))
      return cli_command_definitions[i].observer == 2 ||
             (cli_command_definitions[i].observer == 1 && size == 0U);
  return 0;
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
