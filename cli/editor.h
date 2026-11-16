/** @file Local editor handoff, independent of servers and terminal renderers.
 */
#ifndef CAI_CLI_EDITOR_H
#define CAI_CLI_EDITOR_H
#include <cai/environment.h>
/** Run VISUAL, then EDITOR, then vi with literal absolute filenames. Only the
 * explicitly configured editor command is shell syntax; filenames are argv.
 * Nonzero editor exit or host STOP cancels the edit. The caller owns temporary
 * files and removes them on every return, including failure/cancellation. */
int cai_cli_editor_run(cai_environment *, const char *const *, size_t, int,
                       int (*)(void *), void *, cai_error *);
#endif
