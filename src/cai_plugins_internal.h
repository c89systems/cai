#ifndef CAI_PLUGINS_INTERNAL_H
#define CAI_PLUGINS_INTERNAL_H
#include <cai/plugins.h>
/** Reconcile a plugin catalog generation with the agent's registered tools.
 * Invoke at a runtime boundary where registry replacement is permitted. */
int cai_plugins_sync_agent(cai_agent *agent, cai_error *error);
/** Clear request tool definitions before rebuilding them from the registry. */
void cai_plugin_request_clear_tools(cai_response_create_params *params);
/** Prepare a process-backed plugin handler; registration does not run a tool.
 * The returned handler owns its prepared executor context. */
int cai_plugin_process_prepare(void *context,
                               const cai_plugin_definition *definition,
                               cai_plugin_handler *handler, cai_error *error);
#endif
