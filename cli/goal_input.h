#ifndef CAI_CLI_GOAL_INPUT_H
#define CAI_CLI_GOAL_INPUT_H
#include <cai/cai.h>
/** Bounded compound operator action, never a rendered/model document. */
typedef struct cai_cli_goal_input {
  char *objective, *prompt;
} cai_cli_goal_input;
/** Compile /goal start plus a JSON object into one editable queue entry. */
int cai_cli_goal_input_encode(const char *, const char *, char **, cai_error *);
/** Parse the JSON body into owned strings. Close even after a failed parse. */
int cai_cli_goal_input_parse(const char *, cai_cli_goal_input *, cai_error *);
void cai_cli_goal_input_close(cai_cli_goal_input *);
/** Return the JSON body of a goal's start argument, or NULL for other verbs. */
const char *cai_cli_goal_input_argument(const char *);
#endif
