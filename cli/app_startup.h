#ifndef CAI_APP_STARTUP_H
#define CAI_APP_STARTUP_H
#include "temp.h"
#include <cai/config.h>

/** Immutable resolved startup for a composed owner and frontend. The caller
 * retains configuration and path storage until both have closed; no runtime,
 * terminal handle, journal or storage client crosses this boundary.
 *
 * normal_options retain the original auth/messaging/store selections before
 * any temporary-path substitution. temporary is NULL for ordinary workspaces;
 * otherwise its caller owns cleanup after joining both participants. */
typedef struct cai_cli_prepared {
  cai_config *configuration;
  cai_cli_options options, normal_options;
  const cai_cli_temp *temporary;
} cai_cli_prepared;
#endif
