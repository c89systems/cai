#ifndef CAI_CLI_SPAWN_H
#define CAI_CLI_SPAWN_H
#include "bootstrap.h"
#include <poll.h>
#include <sys/types.h>
typedef struct cai_cli_spawn cai_cli_spawn;
typedef enum cai_cli_spawn_phase {
  CAI_CLI_SPAWN_STARTING = 0,
  CAI_CLI_SPAWN_READY,
  CAI_CLI_SPAWN_STOPPING,
  CAI_CLI_SPAWN_EXITED,
  CAI_CLI_SPAWN_FAILED
} cai_cli_spawn_phase;
typedef struct cai_cli_spawn_state {
  cai_cli_spawn_phase phase;
  int was_ready, exit_code, signal;
  cai_cli_bootstrap_message observation;
} cai_cli_spawn_state;
/** Runs only in the detached child, after closing inherited frontend file
 * descriptors and resetting signals. Construct a new app here. The returned
 * exit code is passed to _exit; no parent exit handlers run. */
typedef int (*cai_cli_spawn_entry_fn)(const void *, int descriptor, int argc,
                                      const char *const *argv);
/** Standalone executable only, before creating workers or installing SIGCHLD
 * handlers. A threadless editor may already be open. The child calls entry
 * directly; there is no executable lookup or reexec. Context and argv are
 * inherited by value through fork and borrowed in the child. Zero startup
 * timeout selects 60 seconds. No runtime/store is opened in the parent.
 * The supplied clock is monotonic milliseconds; ordinary child failures are
 * observations, not errors from pump. All operations have one owner thread. */
int cai_cli_spawn_open(cai_cli_spawn_entry_fn entry, const void *context,
                       int argc, const char *const *argv, uint64_t now_ms,
                       uint64_t timeout_ms, cai_cli_spawn **out, cai_error *);
int cai_cli_spawn_prepare(cai_cli_spawn *, struct pollfd *,
                          uint64_t *deadline_ms, cai_error *);
int cai_cli_spawn_pump(cai_cli_spawn *, uint64_t now_ms, cai_error *);
/* Cancel only before validated READY. Does not kill a ready workspace owner. */
int cai_cli_spawn_cancel(cai_cli_spawn *, uint64_t now_ms, cai_error *);
const cai_cli_spawn_state *cai_cli_spawn_get_state(const cai_cli_spawn *);
pid_t cai_cli_spawn_pid(const cai_cli_spawn *);
/* Close refuses an unreaped child. Keep this watch until process exit, even
 * after attaching/switching workspace. abandon is only for immediate parent
 * process exit; it closes local resources without signalling a ready owner. */
int cai_cli_spawn_close(cai_cli_spawn *, cai_error *);
void cai_cli_spawn_abandon(cai_cli_spawn *);
#endif
