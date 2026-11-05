#ifndef CAI_CLI_RUN_TERMINAL_H
#define CAI_CLI_RUN_TERMINAL_H
#include "app_internal.h"
/** Synchronous application composition: the calling thread owns the common
 * controller and host callbacks; a default-stack terminal thread attaches via
 * the public socket client. The controller installs no process environment,
 * locale or signal state; temporary terminal modes belong to the frontend.
 * Sets handled only for an interactive, non-temporary agent invocation (or a
 * utility already completed while resolving its configuration). Other modes
 * remain with the caller's batch/utility composition. Arguments are borrowed.
 */
int cai_cli_run_terminal(const cai_app_config *, cai_app_bridge *, int, char **,
                         int *handled, int *exit_code, cai_error *);
#endif
