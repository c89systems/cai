#ifndef CAI_CLI_STARTUP_H
#define CAI_CLI_STARTUP_H
#include "options.h"
#include <cai/cai.h>

/** Ordered invocation intents, compiled into ordinary backend commands. No
 * transport, editor, runtime or configuration ownership belongs here. */
typedef enum cai_cli_startup_stage {
  CAI_CLI_STARTUP_SELECT = 0,
  CAI_CLI_STARTUP_MODEL,
  CAI_CLI_STARTUP_EFFORT,
  CAI_CLI_STARTUP_SUMMARY,
  CAI_CLI_STARTUP_TIER,
  CAI_CLI_STARTUP_YIELD,
  CAI_CLI_STARTUP_YOLO,
  CAI_CLI_STARTUP_REASONING_TOOL,
  CAI_CLI_STARTUP_NORMAL_EFFORT,
  CAI_CLI_STARTUP_HIGH_EFFORT,
  CAI_CLI_STARTUP_SHELL_CONTEXT,
  CAI_CLI_STARTUP_HISTORY,
  CAI_CLI_STARTUP_RETENTION,
  CAI_CLI_STARTUP_AUTOQUEUE,
  CAI_CLI_STARTUP_GOAL,
  CAI_CLI_STARTUP_INSTRUCTIONS,
  CAI_CLI_STARTUP_DONE
} cai_cli_startup_stage;
typedef struct cai_cli_startup {
  cai_cli_startup_stage stage;
  size_t instruction;
} cai_cli_startup;

/** Return one bounded, heap-owned input (caller frees), or NULL at completion.
 * control distinguishes generated commands from the user's instructions.
 * Call again only after the backend admits/applies the preceding input. */
int cai_cli_startup_next(cai_cli_startup *, const cai_cli_options *,
                         const char *session, char **text, int *control,
                         cai_error *);
#endif
