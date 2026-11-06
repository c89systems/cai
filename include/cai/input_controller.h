#ifndef CAI_INPUT_CONTROLLER_H
#define CAI_INPUT_CONTROLLER_H
#include <cai/input_queue.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_input_controller cai_input_controller;
typedef struct cai_input_connection cai_input_connection;
/** Ephemeral request envelope, separate from durable queue transitions. */
typedef struct cai_input_scope {
  unsigned long long conversation;
  uint64_t generation;
} cai_input_scope;
/** Trusted owner-thread admission view, borrowed for the authorization call.
 * Attachment/producer come from the connection, never from request text.
 * entry is the current target when found, including trusted ownership/edit
 * metadata; do not forward it to a frontend. NULL request denotes availability
 * metadata or a separately authorized host control, rather than a queue
 * mutation; entry is then NULL.
 */
typedef struct cai_input_admission {
  uint64_t attachment, generation;
  unsigned long long producer;
  const cai_input_request *request;
  const cai_input_entry *entry;
} cai_input_admission;
typedef struct cai_input_controller_config {
  /** Optional owner-thread admission gate, called immediately before queue
   * mutation. Receives authenticated connection identity, original generation,
   * request and current target. Hosts can admit read-only commands without
   * granting general queue authority. Must not reenter the input controller.
   * Return CAI_OK or an actionable rejection. SYNCHRONIZE and RESOLVE remain
   * read-only and bypass this gate. NULL is a trusted in-process controller.
   * Borrowed context must outlive the controller and all owner dispatch. */
  int (*authorize)(void *context, const cai_input_admission *,
                   cai_error *error);
  void *authorize_context;
} cai_input_controller_config;
typedef enum cai_input_observation_type {
  CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN = 0,
  CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM,
  CAI_INPUT_OBSERVATION_SNAPSHOT_END,
  CAI_INPUT_OBSERVATION_CHANGE,
  CAI_INPUT_OBSERVATION_RESULT,
  CAI_INPUT_OBSERVATION_DISCONNECTED
} cai_input_observation_type;
/** Owned copied observation. No strings borrow the runtime/editor. */
typedef struct cai_input_observation {
  cai_input_observation_type type;
  unsigned long long conversation;
  /** Bound logical producer and its processed high watermark. Present on
   * snapshots/changes and current-conversation results. Keep the larger of
   * last_request and the frontend's own last-issued ID on reconnect. */
  unsigned long long producer, last_request;
  cai_input_state state;
  cai_input_change change;
  cai_input_result result;
  /** Snapshot items arrive individually between BEGIN and END, with a common
   * conversation/view revision. No complete snapshot is built by the producer.
   * DISCONNECTED invalidates an unfinished baseline; a later BEGIN restarts
   * it. A consumer publishes only after END. Its bounded frontend mirror is
   * explicit consumer state, not a transport accumulator. */
  cai_input_entry entry;
  size_t index;
} cai_input_observation;
/** Create on the controller owner thread. No worker, terminal, HTTP or storage
 * dependency. The application schedules pump from its owner event loop. */
int cai_input_controller_open(const cai_input_controller_config *config,
                              cai_input_controller **out, cai_error *error);
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
/** Copy the selected conversation epoch without reserving a producer.
 * Thread-safe; unavailable while deselected or closed. */
int cai_input_controller_conversation(cai_input_controller *,
                                      unsigned long long *out, cai_error *);
/** Thread-safe frontend attachment. Initial snapshot arrives after pump. */
int cai_input_controller_connect(cai_input_controller *controller,
                                 uint64_t attachment,
                                 cai_input_connection **out, cai_error *error);
/** Thread-safe reconnect to a retained producer in the current conversation.
 * No new producer is reserved. conversation must be the verified epoch from
 * this controller instance; after a server restart the host first verifies
 * the persistent session identity and obtains its new epoch. Producer lookup
 * happens on owner dispatch. An unavailable/already-attached producer emits
 * DISCONNECTED with result.code/reason, without failing other connections or
 * silently opening a new producer. Such a connection is terminal. */
int cai_input_controller_resume(cai_input_controller *controller,
                                uint64_t attachment,
                                unsigned long long conversation,
                                unsigned long long producer,
                                cai_input_connection **out, cai_error *error);
/** Thread-safe observation detach; never stops/clears accepted work. */
void cai_input_connection_close(cai_input_connection *connection);
/** Owner-only transport cleanup: durably release this producer's edit holds
 * without advancing its request IDs or submitting its local draft. Call
 * outside socket callbacks, before close/deselect/runtime destruction.
 * No-op if no producer was bound; journal failure is returned, never hidden. */
int cai_input_connection_release_edits(cai_input_connection *, cai_error *);
/** Owner-only authority cleanup for all producer connections belonging to one
 * authenticated attachment. Uses durable releases, without changing receipts
 * or closing caller-owned handles. Run after authority is revoked, before
 * admitting requests under the new generation. */
int cai_input_controller_release_edits(cai_input_controller *,
                                       uint64_t attachment, cai_error *);
/** Owner-only: bind a producer's ephemeral delivery gate before its initial
 * snapshot. Every subsequent request closes the gate until settled.
 * Call again when this producer gains a new authority generation. Repeating
 * the same scope preserves an already-received settlement. Does not change
 * accepted input, durable receipts or edit holds. */
int cai_input_connection_manage_delivery(cai_input_connection *,
                                         const cai_input_scope *, cai_error *);
/** Owner-only: announce that producer receipts and dependent requests have
 * settled. after_request is the observed processed watermark, not a new ID.
 * Delivery stays gated until all handed-off requests have been processed at
 * exactly that watermark. Stale authority/epoch announcements do not release
 * the gate. No acknowledgement, journal record or automatic retry is created.
 */
int cai_input_connection_settled(cai_input_connection *,
                                 const cai_input_scope *,
                                 unsigned long long after_request, cai_error *);
/** Owner-only: inspect one producer's delivery gate. Unmanaged, disconnected
 * or revoked producers do not hold accepted work. No draft state crosses this
 * boundary; this readiness is independent of frontend editing policies. */
int cai_input_controller_producer_ready(cai_input_controller *,
                                        unsigned long long producer,
                                        int *settled, cai_error *);
/** Copy and signal only. Success means transport handoff, not acceptance.
 * scope is copied from the frontend's latest snapshot/role. Conversation zero
 * is valid only for initial SYNCHRONIZE. Request IDs never repeat on one
 * connection. Generation is validated at dispatch, even after a takeover and
 * return of control to the same attachment. A trusted local caller uses zero.
 * Full admission is bounded to 16 requests plus SYNC/RESOLVE/STOP reserves. */
int cai_input_connection_request(cai_input_connection *connection,
                                 const cai_input_scope *scope,
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
