#ifndef CAI_CLI_PROCESS_H
#define CAI_CLI_PROCESS_H
#include <cai/app.h>
/* Executable-only process/signal/logging owner. The library never calls this
 * helper and never implicitly forks or installs these signal handlers. */
int cai_cli_headless_run(const cai_app_config *, int argc,
                         const char *const *argv, int *exit_code, cai_error *);
/* Private exec handoff. Owns descriptor on entry, including failure. */
int cai_cli_owner_run(const cai_app_config *, int descriptor, int argc,
                      const char *const *argv, int *exit_code, cai_error *);
#endif
