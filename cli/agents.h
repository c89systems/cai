/** @file agents.h
 * Stored-agent configuration and bootstrap ownership. No session selection,
 * inference, authentication login or frontend state belongs to this adapter.
 */
#ifndef CAI_CLI_AGENTS_H
#define CAI_CLI_AGENTS_H
#include "log.h"
#include "options.h"
#include <cai/app_types.h>
#include <cai/config.h>

/** Host-owned fields needed before opening a repository. Never stored as
 * agent behavior, and never accepted from an agent's configuration attachment.
 */
int cai_cli_agent_bootstrap_field(const char *section, const char *key);
/** Project host settings for a pure attachment without opening any store.
 * Agent seed entries never become requirements on an existing backend. */
int cai_cli_agent_bootstrap_configuration(const cai_config *bootstrap,
                                          const cai_config_context *startup,
                                          cai_config **out, cai_error *error);
/** Load or atomically seed default. The returned configuration owns a shared
 * repository reference until close, allowing session/auth roles to reuse it. */
int cai_cli_agent_configuration(cai_config *bootstrap, cai_config *seed,
                                const cai_cli_options *options,
                                const cai_app_config *application,
                                const cai_config_context *startup,
                                cai_config **out, cai_error *error);
/** Retain the bootstrap logger so the store and runtime share one live sink. */
cai_cli_log *cai_cli_agent_configuration_log(cai_config *, int interactive);
/** End preflight storage ownership before an executable forks. Configuration
 * strings remain valid. Also used after a native controller stops, before
 * any frontend-only cleanup confirmation. */
void cai_cli_agent_configuration_detach(cai_config *configuration);
/** Internal streaming producer for the configuration attachment. */
int cai_cli_agent_configuration_write(void *, cai_sink *, cai_error *);
#endif
