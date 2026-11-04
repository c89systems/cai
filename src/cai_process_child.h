#ifndef CAI_PROCESS_CHILD_H
#define CAI_PROCESS_CHILD_H
#include <signal.h>
#include <stddef.h>
#include <string.h>

/** After fork, immediately before execution: do not export the host's blocked
 * stop signals or ignored SIGPIPE/job-control dispositions to an independent
 * command. These calls are async-signal-safe and affect only this child.
 * Keep signals blocked until descriptor/session setup has completed.
 * @return Zero on success, or -1 if a signal operation fails. */
static int cai_process_child_signals(void) {
  static const int signals[] = {SIGHUP,  SIGINT,  SIGQUIT, SIGTERM,
                                SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN,
                                SIGTTOU, SIGCONT, SIGWINCH};
  struct sigaction action;
  sigset_t empty;
  size_t i;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  if (sigemptyset(&action.sa_mask) != 0 || sigemptyset(&empty) != 0)
    return -1;
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); ++i)
    if (sigaction(signals[i], &action, NULL) != 0)
      return -1;
  return sigprocmask(SIG_SETMASK, &empty, NULL);
}
#endif
