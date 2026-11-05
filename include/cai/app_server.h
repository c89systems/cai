/** @file cai/app_server.h Terminal-independent application workspace owner. */
#ifndef CAI_APP_SERVER_H
#define CAI_APP_SERVER_H
#include <cai/app_types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_app_server cai_app_server;
/** Copy and validate configuration without opening stores, starting workers or
 * changing process state. NULL config selects the application defaults. */
int cai_app_server_open(const cai_app_config *, cai_app_server **, cai_error *);
/** Start one asynchronous workspace owner on a default-stack worker. Arguments
 * exclude the program name and are copied. Directory/environment are captured
 * once; context may explicitly supply either. READY follows restoration and is
 * observed through status. NULL lifetime uses configured persistence; an
 * explicit lifetime overrides it. No fork, terminal or renderer is involved.
 * Runtime/tool/replay/control callbacks execute on that owner thread. */
int cai_app_server_start(cai_app_server *, const cai_config_context *, int argc,
                         const char *const *argv,
                         const cai_attachment_config *lifetime, cai_error *);
/** Thread-safe bounded control admission. STOP is reserved even when the
 * ordinary FIFO is full. Application acknowledgement is delivered separately
 * through config.control_callback/status, with the same lifetime as cai_app. */
int cai_app_server_control(cai_app_server *, cai_app_control_type, const char *,
                           unsigned long long *id, cai_error *);
/** Copy an owned snapshot; initialize/clean it using cai_app_status_init and
 * cai_app_status_cleanup. Safe independently of the owner's callbacks. */
int cai_app_server_get_status(const cai_app_server *, cai_app_status *,
                              cai_error *);
/** Borrow the nonblocking status notification descriptor, or -1 for NULL.
 * Observe drains its notification and atomically copies the current snapshot.
 */
int cai_app_server_status_fd(const cai_app_server *);
int cai_app_server_observe_status(const cai_app_server *, cai_app_status *,
                                  cai_error *);
/** Connect/reconcile native input through the same owner queue as sockets.
 * These carry no renderer state and do not open another SDK client. */
int cai_app_server_connect_input(cai_app_server *, uint64_t attachment,
                                 cai_input_connection **, cai_error *);
int cai_app_server_resume_input(cai_app_server *, uint64_t attachment,
                                unsigned long long conversation,
                                unsigned long long producer,
                                cai_input_connection **, cai_error *);
/** Join only from a different thread; explicitly blocking. STOP first when
 * terminating persistent ownership. Returns the owner's application exit code;
 * after joining the same server object can be started again. */
int cai_app_server_join(cai_app_server *, int *exit_code, cai_error *);
/** Release an inactive/joined owner. NULL is accepted. An active owner remains
 * owned by the caller: this never implicitly blocks, cancels or frees it. */
void cai_app_server_close(cai_app_server *);
#ifdef __cplusplus
}
#endif
#endif
