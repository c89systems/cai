#ifndef CAI_INPUT_CONTROLLER_H
#define CAI_INPUT_CONTROLLER_H
#include <cai/input_queue.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_input_controller cai_input_controller;
typedef struct cai_input_connection cai_input_connection;
typedef enum cai_input_observation_type {
  CAI_INPUT_OBSERVATION_SNAPSHOT = 0,
  CAI_INPUT_OBSERVATION_CHANGE,
  CAI_INPUT_OBSERVATION_RESULT,
  CAI_INPUT_OBSERVATION_DISCONNECTED
} cai_input_observation_type;
/** Owned copied observation. No strings borrow the runtime/editor. */
typedef struct cai_input_observation {
  cai_input_observation_type type;
  unsigned long long conversation;
  cai_input_state state;
  cai_input_change change;
  cai_input_result result;
  cai_input_entry *entries;
  size_t count;
} cai_input_observation;
/** Create on the controller owner thread. No worker, terminal, HTTP or storage
 * dependency. The application schedules pump from its owner event loop. */
int cai_input_controller_open(cai_input_controller **out, cai_error *error);
/** Owner-only: detach all borrowed queues before closing their runtimes.
 * Existing connections receive disconnect and retain outstanding results.
 * Connections may outlive close and must still be closed by their callers. */
void cai_input_controller_close(cai_input_controller *controller);
/** Owner-only: bind a conversation's queue, or NULL while changing context.
 * Never moves accepted work between conversations. Changing selection rejects
 * still-unadmitted old-context requests; it cannot redirect them to the new
 * queue. Selection retains allocation, not journal authority; closing its
 * runtime still disables admission immediately. Deselect before shutdown. */
int cai_input_controller_select(cai_input_controller *controller,
                                cai_input_queue *queue, cai_error *error);
/** Thread-safe frontend attachment. Initial snapshot arrives after pump. */
int cai_input_controller_connect(cai_input_controller *controller,
                                 cai_input_connection **out, cai_error *error);
/** Thread-safe observation detach; never stops/clears accepted work. */
void cai_input_connection_close(cai_input_connection *connection);
/** Copy and signal only. Success means transport handoff, not acceptance.
 * conversation is copied from the frontend's latest snapshot; zero is valid
 * only for initial SYNCHRONIZE. Request IDs never repeat on one connection.
 * Full admission is bounded to 16 requests plus SYNC/RESOLVE/STOP reserves. */
int cai_input_connection_request(cai_input_connection *connection,
                                 unsigned long long conversation,
                                 const cai_input_request *request,
                                 cai_error *error);
/** Owner-only, bounded dispatch; no frontend callbacks or rendering. */
int cai_input_controller_pump(cai_input_controller *controller,
                              size_t max_requests, cai_error *error);
/** Wakeup descriptors are borrowed; never read/close them outside pump/poll. */
int cai_input_controller_fd(const cai_input_controller *controller);
int cai_input_connection_fd(const cai_input_connection *connection);
/** Thread-safe: dequeue one owned observation. CAI_OK with *out=NULL means
 * empty. A slow reader is disconnected from deltas and must SYNCHRONIZE;
 * bounded result receipts are retained for explicit reconciliation. */
int cai_input_connection_poll(cai_input_connection *connection,
                              cai_input_observation **out, cai_error *error);
void cai_input_observation_close(cai_input_observation *observation);
#ifdef __cplusplus
}
#endif
#endif
