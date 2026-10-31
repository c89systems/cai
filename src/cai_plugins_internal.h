#ifndef CAI_PLUGINS_INTERNAL_H
#define CAI_PLUGINS_INTERNAL_H
#include <cai/plugins.h>
int cai_plugins_sync_agent(cai_agent *agent, cai_error *error);
void cai_plugin_request_clear_tools(cai_response_create_params *params);
int cai_plugin_process_prepare(void *context,
                               const cai_plugin_definition *definition,
                               cai_plugin_handler *handler, cai_error *error);
#endif
