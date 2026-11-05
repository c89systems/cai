#ifndef CAI_CLI_UTILITY_H
#define CAI_CLI_UTILITY_H
#include "app_internal.h"

/** Synchronous maintenance dispatcher. Runs no agent loop, workspace server,
 * renderer or editor. Standalone export retains the shared helper's read-only
 * context restoration. Callbacks remain on this calling thread; STOP
 * uses the ordinary application control channel. Returns the CLI exit code.
 *
 * Process-owned logging/browser helpers run under the app's process-resource
 * wrapper so configured environment settings retain their established
 * semantics. This is a utility boundary,
 * not a second agent owner. */
int cai_cli_utility_run(int, char **, const cai_app_config *, cai_app_bridge *);
#endif
