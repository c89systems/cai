/** @file shell_text.h
 * Incremental terminal-safe plaintext, independent of process execution.
 */
#ifndef CAI_CLI_SHELL_TEXT_H
#define CAI_CLI_SHELL_TEXT_H
#include <cai/cai.h>
typedef struct cai_cli_shell_text {
  unsigned char pending[4];
  size_t pending_length;
} cai_cli_shell_text;
typedef int (*cai_cli_shell_output_fn)(void *, const char *, size_t,
                                       cai_error *);
/** Preserve printable UTF-8, LF and tabs; escape other controls and invalid
 * bytes as \\xNN, including decoded C1 controls split between chunks.
 * Zero-initialize state; final flushes only its incomplete scalar. */
int cai_cli_shell_plain(cai_cli_shell_text *, const char *, size_t, int final,
                        cai_cli_shell_output_fn, void *, cai_error *);
#endif
