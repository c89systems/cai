#ifndef CAI_CLI_WORKSPACE_HOST_H
#define CAI_CLI_WORKSPACE_HOST_H
#include "app_internal.h"
#include <cai/workspace_server.h>
#include <poll.h>
#define CAI_CLI_WORKSPACE_HOST_FDS                                             \
  (CAI_WORKSPACE_SERVER_FDS + CAI_ATTACHMENT_LIMIT + 1U)
typedef struct cai_cli_workspace_host cai_cli_workspace_host;
struct cai_cli_pouch;
/* Owner-thread socket binding, without a terminal or renderer. Its transcript
 * publisher owns one retained context reader worker. Holds endpoint ownership
 * until close, after runtime/store shutdown. */
int cai_cli_workspace_host_open(const char *runtime_directory,
                                const char *workspace,
                                const cai_attachment_config *lifetime,
                                const cai_input_controller_config *admission,
                                cai_app_bridge *bridge,
                                struct pslog_logger *logger,
                                cai_cli_workspace_host **out, cai_error *error);
int cai_cli_workspace_host_ready(cai_cli_workspace_host *, cai_error *);
/* Bind only after queue restoration; unbind before runtime destruction.
 * Unbind durably releases disconnected editors' holds without draft delivery.
 */
int cai_cli_workspace_host_input(cai_cli_workspace_host *,
                                 cai_input_controller *, const char *session,
                                 cai_error *);
int cai_cli_workspace_host_history(cai_cli_workspace_host *,
                                   struct cai_cli_pouch *, cai_error *);
/* Append socket interests; shorten timeout for deadlines or credit-ready work.
 * Capacity must cover CAI_CLI_WORKSPACE_HOST_FDS. */
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
