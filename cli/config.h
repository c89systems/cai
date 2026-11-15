#ifndef CAI_CLI_CONFIG_H
#define CAI_CLI_CONFIG_H
#include "options.h"
#include <cai/app.h>
#include <cai/config.h>
/* Help/version and default dumps bypass normal config loading
 * and creation. A dump writes only its explicit destination or stdout.
 * Owned configuration outlives
 * clients and runtimes borrowing its strings. Return 1=start, 0=utility,
 * -1=error. Successful startup options borrow a configuration-owned canonical
 * workspace directory. This function never changes the host cwd or installs
 * [env] entries into the process environment. */
/* Theme validation/listing belongs to the calling frontend/utility adapter. */
int cai_cli_configure(int argc, char *const *argv, cai_cli_options *options,
                      cai_config **configuration,
                      const cai_app_config *application, cai_error *error);
/* Explicit startup context for a controller owner. Does not consult cwd or
 * process environment when both context members are supplied. */
int cai_cli_configure_at(int argc, char *const *argv, cai_cli_options *options,
                         cai_config **configuration,
                         const cai_app_config *application,
                         const cai_config_context *startup, cai_error *error);
/** Production bootstrap + stored-agent resolution. The parser above remains
 * usable independently for bootstrap diagnostics and configuration utilities.
 * Agent behavior is loaded from the repository; only host settings use env. */
int cai_cli_configure_agent_at(int argc, char *const *, cai_cli_options *,
                               cai_config **, const cai_app_config *,
                               const cai_config_context *, cai_error *);
/** Host/attachment configuration only: no repository or session store opens,
 * no automatic INI creation, and seed values do not constrain a live owner. */
int cai_cli_configure_bootstrap_at(int argc, char *const *, cai_cli_options *,
                                   cai_config **, const cai_app_config *,
                                   const cai_config_context *, cai_error *);
#endif
