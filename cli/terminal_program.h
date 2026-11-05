#ifndef CAI_CLI_TERMINAL_PROGRAM_H
#define CAI_CLI_TERMINAL_PROGRAM_H
#include <cai/environment.h>
#include <softline/softline.h>
/** Nonblocking owner-thread maintenance during a terminal relay. May register
 * external descriptors with epoll_fd and shorten timeout_ms (-1 means none).
 * Must not call Softline or render. Borrowed through the synchronous run. */
typedef int (*cai_cli_terminal_service_fn)(void *, int epoll_fd,
                                           int *timeout_ms, cai_error *);
/** Consume one borrowed output chunk. accepted=0 applies backpressure: the
 * relay retains that bounded chunk and retries after descriptor progress.
 * The callback must not block, render or reenter Softline. */
typedef int (*cai_cli_terminal_capture_fn)(void *, const void *, size_t,
                                           int *accepted, cai_error *);
/** Execute an argv directly, in an explicit directory and environment, through
 * Softline's public PTY relay. No shell interpolation, process-global chdir or
 * environment mutation. Optional capture retains at most one 4 KiB chunk
 * while awaiting consumer acknowledgement. Softline owns terminal modes,
 * resize, local escape and draft continuation. A single child waiter is joined
 * after relay callbacks end; no child or waiter survives return.
 * status is a waitpid status, distinguishing exit 137 from SIGKILL.
 * escaped reports Softline's beginning-of-line ~. escape. */
int cai_cli_terminal_program(sl_t *, const char *const *argv,
                             const char *directory, cai_environment *,
                             cai_cli_terminal_service_fn,
                             cai_cli_terminal_capture_fn, void *, int *status,
                             int *escaped, cai_error *);
#endif
