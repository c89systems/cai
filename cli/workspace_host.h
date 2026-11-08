#ifndef CAI_CLI_WORKSPACE_HOST_H
#define CAI_CLI_WORKSPACE_HOST_H
#include "app_internal.h"
#include <cai/app_view.h>
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
                                const char *workspace, const char *name,
                                const cai_workspace_claim *, size_t claim_count,
                                const cai_attachment_config *lifetime,
                                const cai_input_controller_config *admission,
                                cai_app_bridge *bridge,
                                struct pslog_logger *logger,
                                cai_cli_workspace_host **out, cai_error *error);
int cai_cli_workspace_host_ready(cai_cli_workspace_host *, cai_error *);
/** Borrow a synchronous owner action through close/unbind. Authorization is
 * checked immediately before invocation, never in a later trusted mailbox. */
void cai_cli_workspace_host_cancel(cai_cli_workspace_host *,
                                   int (*)(void *, cai_error *), void *);
/** Select before producer binding, outside the socket decoder callback.
 * NULL selector means a new conversation. Borrow callback/context to close. */
void cai_cli_workspace_host_selection(cai_cli_workspace_host *,
                                      int (*)(void *, const char *,
                                              cai_error *),
                                      void *);
void cai_cli_workspace_host_prompt_limit(cai_cli_workspace_host *,
                                         unsigned int);
/** Borrow immutable runtime catalog metadata through context unbind. Publishes
 * one bounded completion row at a time; does no discovery or model work. */
int cai_cli_workspace_host_catalog(cai_cli_workspace_host *,
                                   cai_agent_runtime *, cai_error *);
/* Borrow the owner snapshot only for this call. Cache semantic changes; no
 * socket I/O, subscriber callbacks or elapsed-only publication here. */
int cai_cli_workspace_host_status(cai_cli_workspace_host *,
                                  const cai_app_view *, cai_error *);
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
/** Bounded output-only progress from an application document producer. Never
 * dispatch socket input or reenter application commands. Slow subscribers
 * retain the ordinary bounded backlog and native catch-up behavior. */
int cai_cli_workspace_host_progress(cai_cli_workspace_host *, cai_error *);
int cai_cli_workspace_host_terminal(cai_cli_workspace_host *,
                                    const cai_agent_runtime_event *,
                                    cai_error *);
/** Publish bounded backend bang output without journaling raw bytes. */
int cai_cli_workspace_host_shell(cai_cli_workspace_host *, uint64_t sequence,
                                 uint64_t offset, const void *, size_t,
                                 int final, int *accepted, cai_error *);
void cai_cli_workspace_host_admission(cai_cli_workspace_host *,
                                      cai_input_controller_config *);
void cai_cli_workspace_host_stop(cai_cli_workspace_host *);
void cai_cli_workspace_host_close(cai_cli_workspace_host *);
#endif
