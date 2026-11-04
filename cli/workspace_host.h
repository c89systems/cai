#ifndef CAI_CLI_WORKSPACE_HOST_H
#define CAI_CLI_WORKSPACE_HOST_H
#include "app_internal.h"
#include <poll.h>
typedef struct cai_cli_workspace_host cai_cli_workspace_host;
/* Owner-thread application binding. No terminal, renderer or worker. Holds
 * endpoint ownership until close, after runtime/store shutdown. */
int cai_cli_workspace_host_open(const char *runtime_directory,
                                const char *workspace,
                                const cai_attachment_config *lifetime,
                                const cai_input_controller_config *admission,
                                cai_app_bridge *bridge,
                                cai_cli_workspace_host **out, cai_error *error);
int cai_cli_workspace_host_ready(cai_cli_workspace_host *, cai_error *);
/* Append socket interests; shorten timeout only for a real host deadline.
 * Capacity must cover CAI_WORKSPACE_SERVER_FDS. */
int cai_cli_workspace_host_prepare(cai_cli_workspace_host *, struct pollfd *,
                                   size_t capacity, size_t *count,
                                   long *timeout, cai_error *);
int cai_cli_workspace_host_pump(cai_cli_workspace_host *, int *stopping,
                                cai_error *);
void cai_cli_workspace_host_admission(cai_cli_workspace_host *,
                                      cai_input_controller_config *);
void cai_cli_workspace_host_stop(cai_cli_workspace_host *);
void cai_cli_workspace_host_close(cai_cli_workspace_host *);
#endif
