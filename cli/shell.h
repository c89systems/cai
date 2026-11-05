#ifndef CAI_CLI_SHELL_H
#define CAI_CLI_SHELL_H

#include "shell_text.h"
#include <cai/cai.h>
#include <cai/environment.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

/* Linux pipe execution for explicit user commands; independent of agent
 * tools/sandbox and never a PTY. Owner-thread callbacks consume output. */
typedef struct cai_cli_shell {
  pid_t pid;
  int output_fd;
  int exit_fd;
  int timer_fd;
  int exited;
  int eof;
  int exit_code;
  pthread_t waiter;
  int waiter_started;
  struct timespec started;
  cai_cli_shell_text text;
  /** One unread-to-consumer span; the pipe remains untouched until accepted. */
  char pending[4096];
  size_t pending_length;
} cai_cli_shell;
typedef int (*cai_cli_shell_stop_fn)(void *);
/** Return OK with accepted=0 to retry this same span after output credit. */
typedef int (*cai_cli_shell_consume_fn)(void *, const char *, size_t, int *,
                                        cai_error *);

void cai_cli_shell_init(cai_cli_shell *shell);
int cai_cli_shell_start(cai_cli_shell *shell, const char *command,
                        const char *directory, cai_environment *,
                        cai_error *error);
/* Consume at most 64 KiB per dispatch; output precedes completion. */
int cai_cli_shell_pump(cai_cli_shell *shell, cai_cli_shell_consume_fn output,
                       void *context, cai_error *error);
void cai_cli_shell_cancel(cai_cli_shell *shell);
unsigned long long cai_cli_shell_elapsed(const cai_cli_shell *shell);
void cai_cli_shell_close(cai_cli_shell *shell);
/* Called with the editor released. NULL output gives the child the real
 * terminal directly; a capture callback selects a PTY relay and observes raw
 * chunks before they reach the TTY. stop_fd/stop optionally interrupt either
 * mode with CAI_ERR_CANCELLED. The callback must not render or acknowledge
 * controls until the handoff returns and terminal ownership is restored.
 * Restores termios, signal dispositions and window handling on every exit.
 * status is the waitpid status (distinguishes exit 137 from SIGKILL). */
int cai_cli_shell_handoff(const char *path, const char *directory,
                          cai_cli_shell_output_fn output, void *context,
                          int stop_fd, cai_cli_shell_stop_fn stop, int *status,
                          cai_error *error);

#endif
