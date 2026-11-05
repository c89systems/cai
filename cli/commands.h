#ifndef CAI_CLI_COMMANDS_H
#define CAI_CLI_COMMANDS_H
#include <stddef.h>
typedef enum cli_command_boundary {
  CLI_COMMAND_PROMPT,
  CLI_COMMAND_FEED,
  CLI_COMMAND_REQUEST,
  CLI_COMMAND_TURN
} cli_command_boundary;
/* Shared by backend admission, execution and frontend catalogs. Names borrow
 * static storage. at() includes canonical names and aliases. */
size_t cai_cli_command_count(void);
const char *cai_cli_command_at(size_t index);
const char *cai_cli_command_name(const char *line, size_t *length);
cli_command_boundary cai_cli_command_delivery(const char *line);
int cai_cli_command_is_theme(const char *line);
/** Native manual reviewers and admitted review commands share presentation;
 * model-invoked reviewers retain their provider tool-call identity. */
int cai_cli_manual_review_call(const char *);
/* Presentation-only actions are delivered to the originating frontend. */
int cai_cli_command_frontend(const char *line);
/** Select a valid paged command: 1 rendered help, 2 literal log, 0 neither.
 * Whitespace follows ordinary command normalization. No policy or execution. */
int cai_cli_command_pager(const char *line);
#endif
