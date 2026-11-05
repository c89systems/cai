#ifndef CAI_CLI_UTILITY_H
#define CAI_CLI_UTILITY_H
#include "app_internal.h"

/** Classify process utilities with the ordinary argv parser, including help,
 * invalid options and Chrome's explicit first-argument native-host entry.
 * Values of other flags cannot accidentally select a utility. */
int cai_cli_utility_invocation(int argc, char **argv);
/** Synchronous maintenance dispatcher. Runs no agent loop, workspace server,
 * renderer or editor. Standalone export retains the shared helper's read-only
 * context restoration. Callbacks remain on this calling thread; STOP
 * uses the ordinary application control channel. Returns the CLI exit code.
 *
 * Until libpslog accepts an environment lookup callback, run under the app's
 * process-resource wrapper: configured LOG_* and browser-launch settings
 * retain their established environment semantics. This is a utility boundary,
 * not a second agent owner. */
int cai_cli_utility_run(int, char **, const cai_app_config *, cai_app_bridge *);
#endif
