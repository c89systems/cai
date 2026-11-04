#ifndef CAI_CLI_COMMAND_HELP_H
#define CAI_CLI_COMMAND_HELP_H
#include <stddef.h>
/** Common incremental help document for terminal and synchronous receivers.
 * attached adds connection operations owned by a socket terminal. Static
 * paragraphs go directly to write; no complete document or renderer is owned.
 */
int cai_cli_command_help(const char *invocation, int attached,
                         int (*write)(void *, const char *, size_t), void *);
#endif
