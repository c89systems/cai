/** @file cai/network_tools.h Host-pluggable network tools for libcai agents. */
#ifndef CAI_NETWORK_TOOLS_H
#define CAI_NETWORK_TOOLS_H
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Host operations execute on the calling agent's tool worker. Hosts retain
 * config/context through agent destruction and marshal frontend work onto their
 * controller's ordinary event loop. This interface owns no editor or store. */
typedef int (*cai_network_tool_host_fn)(void *, const char *operation,
                                        const char *arguments, cai_sink *,
                                        cai_error *);
typedef struct cai_network_tools_config {
  int disable_event_stream_listener;
  int network_tools;
  cai_network_tool_host_fn call;
  void *context;
} cai_network_tools_config;
/** Current tool worker's runtime cancellation scope; false outside tool work.
 */
int cai_network_tool_cancel_requested(void);
/** NULL config enables only on-demand multicast observation. It opens no socket
 * until a tool invocation. Config is borrowed, never copied into global state.
 */
int cai_network_tools_register(cai_agent *, cai_network_tools_config *,
                               cai_error *);
int cai_network_tools_register_registry(cai_tool_registry *,
                                        cai_network_tools_config *,
                                        cai_error *);
#ifdef __cplusplus
}
#endif
#endif
