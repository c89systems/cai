/** @file cai/app_terminal.h
 * Synchronous Softline/libmdf attachment to an existing workspace owner.
 */
#ifndef CAI_APP_TERMINAL_H
#define CAI_APP_TERMINAL_H
#include <cai/app.h>
#include <cai/app_client.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Run the terminal adapter on the calling thread. Requires a terminal on
 * standard input/output and exclusive ownership of that terminal until return.
 * Opens no runtime, SDK client, authentication store or conversation writer;
 * never forks or starts a server. The transport target supplies runtime
 * directory, canonical workspace and optional verified instance identity.
 * Its event/transcript callbacks are replaced by this adapter's consumers.
 *
 * argv excludes the program name. Frontend preferences use normal app config,
 * settings and argv precedence. Explicit startup inputs/settings are submitted
 * through the native backend queue once control is available. Workspace
 * redirection and utility/batch/server modes are rejected; use another explicit
 * attachment for another workspace. NULL application selects defaults.
 * Configuration is borrowed until return. An orderly detach sets exit_code=0;
 * a terminal/transport failure returns its error and exit_code=1. Persistent
 * owner lifetime remains independent of this terminal receiver.
 */
int cai_app_terminal_run(const cai_app_config *application,
                         const cai_app_client_config *target, int argc,
                         const char *const *argv, int *exit_code,
                         cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
