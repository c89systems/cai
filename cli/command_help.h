#ifndef CAI_CLI_COMMAND_HELP_H
#define CAI_CLI_COMMAND_HELP_H
#include <cai/cai.h>
#include <stddef.h>
typedef struct cai_cli_help_reader cai_cli_help_reader;
/** Finite Markdown source shared by inline help and the native pager. Retains
 * paragraph cursors and one bounded startup example, never a whole document.
 * read has the libmdf source callback shape; zero bytes means EOF. */
int cai_cli_help_reader_open(const char *invocation, int attached,
                             cai_cli_help_reader **, cai_error *);
size_t cai_cli_help_reader_read(void *, char *, size_t, int *);
void cai_cli_help_reader_close(cai_cli_help_reader *);
/** Common incremental help document for terminal and synchronous receivers.
 * attached adds connection operations owned by a socket terminal. Static
 * paragraphs go directly to write; no complete document or renderer is owned.
 */
int cai_cli_command_help(const char *invocation, int attached,
                         int (*write)(void *, const char *, size_t), void *);
#endif
