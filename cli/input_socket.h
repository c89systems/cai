#ifndef CAI_CLI_INPUT_SOCKET_H
#define CAI_CLI_INPUT_SOCKET_H
#include "input_adapter.h"
#include <cai/app_client.h>
typedef struct cai_cli_input_socket cai_cli_input_socket;
/* All operations run on the shared editor/application-client owner thread. */
/* One retained editor binding to a logical producer. client is borrowed and
 * pumped by the frontend owner, never by the queue callback. Construct before
 * calling client bind. No runtime/store/worker is created.
 * last_issued is the retained durable producer watermark from a previous
 * editor, zero for a fresh producer. Preserve it with producer/session when
 * recreating an editor; processed backend receipts may lag an uncertain offer.
 * Pending editor proposals themselves survive only with the retained editor. */
int cai_cli_input_socket_open(cai_app_client *, unsigned long long last_issued,
                              cai_cli_input_socket **, cai_error *);
/** Borrow a nonblocking wake descriptor. Successful editor handoffs signal
 * it without reentering Softline; the frontend drains it and updates socket
 * interests. Negative disables. NULL client at open permits a startup editor
 * before an owner is ready; reconnect attaches its first transport. */
void cai_cli_input_socket_set_wakeup(cai_cli_input_socket *, int descriptor);
/** Record canonical prompt attempts before native queue handoff, after
 * completion-prefix removal. Never reenter the editor. Recall is separate
 * from model admission and this callback does not claim durable storage. */
void cai_cli_input_socket_prompt(cai_cli_input_socket *,
                                 void (*)(void *, const cai_input_request *),
                                 void *);
/** Call only from Softline's native idle callback. Pending receipts and
 * deferred edit actions must settle before announcing remote delivery. */
int cai_cli_input_socket_idle(cai_cli_input_socket *, sl_t *, cai_error *);
cai_cli_input_adapter *cai_cli_input_socket_adapter(cai_cli_input_socket *);
/* Forward public client events; transcript spans use a separate callback.
 * The first accepted baseline establishes request-ID mapping. Later baselines
 * must identify that same session/producer, including after a server restart.
 * Opaque item/revision tokens stay stable while this editor is retained;
 * fresh requests still use the verified connection's live epoch/generation.
 * A different session view needs its own editor binding. */
int cai_cli_input_socket_observe(cai_cli_input_socket *,
                                 const cai_app_client_event *, cai_error *);
/* Abandon frontend routing to the current transport, mark outcomes unknown,
 * and release its borrowed reference. Stop routing its events and close/detach
 * that client; this method itself never closes it. Also abandons a pause. */
int cai_cli_input_socket_disconnect(cai_cli_input_socket *, cai_error *);
/** Pause editor observations while continuing to pump the borrowed client.
 * Pending outcomes become unknown; no observations are buffered or reapplied.
 * Keep the original client alive. Resume accepts only that same connection,
 * bound to the original session ID, epoch and producer. Follow resume
 * with SYNCHRONIZE and native receipt RESOLVE; never repeat uncertain offers.
 * A changed scope must instead disconnect and bind the appropriate view. */
int cai_cli_input_socket_pause(cai_cli_input_socket *, cai_error *);
int cai_cli_input_socket_resume(cai_cli_input_socket *, cai_app_client *,
                                cai_error *);
/* Explicit transport replacement after disconnect() released the old client.
 * Retains editor, mirror, request mapping and unresolved outcomes. This does
 * not open/connect/bind/resubmit anything; verify the new instance and resume
 * the exact producer through the public client API. */
int cai_cli_input_socket_reconnect(cai_cli_input_socket *, cai_app_client *,
                                   cai_error *);
/* Borrow the most recent submission failure for deferred frontend display.
 * The request callback must not synchronously reenter Softline to display it.
 */
const cai_error *cai_cli_input_socket_error(const cai_cli_input_socket *);
unsigned long long
cai_cli_input_socket_last_issued(const cai_cli_input_socket *);
/* Follow sl_destroy. Never closes the borrowed application client. */
void cai_cli_input_socket_close(cai_cli_input_socket *);
#endif
