/** @file Instruction-file ownership and offline base-prompt export. */
#ifndef CAI_CLI_INSTRUCTIONS_H
#define CAI_CLI_INSTRUCTIONS_H
#include "options.h"
#include <cai/agent_runtime.h>

typedef struct cai_cli_instruction_files {
  char *agent, *reviewer, *initial;
} cai_cli_instruction_files;

/** Load explicit regular files (up to 1 MiB each). All outputs are owned even
 * on failure; close before reuse. No auth, discovery or workspace reads occur.
 */
int cai_cli_instruction_files_open(const cai_cli_options *,
                                   cai_cli_instruction_files *, cai_error *);
void cai_cli_instruction_files_close(cai_cli_instruction_files *);
/** Write both resolved roles into newly created files in a recursive directory.
 * Existing files are never truncated. No runtime or storage client is opened.
 */
int cai_cli_dump_instructions(const cai_cli_options *, cai_error *);
#endif
