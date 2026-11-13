/** @file cai/app_client.h
 * Typed application input and transcript client. Owns no runtime, store or
 * terminal.
 */
#ifndef CAI_APP_CLIENT_H
#define CAI_APP_CLIENT_H
#include <cai/app_catalog.h>
#include <cai/app_view.h>
#include <cai/execution.h>
#include <cai/input_controller.h>
#include <cai/transcript.h>
#include <cai/workspace_client.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct cai_app_client cai_app_client;
typedef struct cai_app_transcript_state {
  char session[129], collection[21];
  uint64_t stream, conversation, sequence, position, watermark, visibility;
  int subscribing, active, replaying;
} cai_app_transcript_state;
typedef struct cai_app_client_state {
  cai_workspace_client_state transport;
  /** Last validated baseline identity; retained after disconnect for explicit
   * reconciliation. Neither a greeting nor a partial snapshot replaces it. */
  char session[129];
  unsigned long long conversation, producer, last_request;
  cai_input_state queue;
  int binding, bound, ready;
  cai_app_transcript_state transcript;
} cai_app_client_state;
typedef enum cai_app_client_event_type {
  CAI_APP_CLIENT_CONNECTION = 0,
  CAI_APP_CLIENT_INPUT,
  CAI_APP_CLIENT_BIND_REJECTED,
  CAI_APP_CLIENT_STATUS,
  /** input->result is a connection control outcome, independent of the queue.
   */
  CAI_APP_CLIENT_CONTROL_RESULT,
  /** Durable prompt save outcome; request IDs are independent of queue IDs.
   * input->result.item_id is the committed prompt collection position. */
  CAI_APP_CLIENT_HISTORY_RESULT,
  /** Shell capture outcome. item_id identifies its connection-local recording;
   * this is not a durable input-queue receipt. */
  CAI_APP_CLIENT_SHELL_RESULT
} cai_app_client_event_type;
typedef struct cai_app_client_event {
  cai_app_client_event_type type;
  uint64_t request;
  const cai_app_client_state *state;
  /** Borrowed typed row/receipt; NULL for CONNECTION. Do not call the owned
   * observation destructor. BEGIN/ITEM remain provisional until END. A reset
   * or transport loss invalidates that baseline and all editor capabilities.
   * The client retains only baseline metadata and one bounded wire row. */
  const cai_input_observation *input;
  /** Borrowed completed view for STATUS, otherwise NULL. */
  const cai_app_view *status;
} cai_app_client_event;
typedef struct cai_app_execution_event {
  /** Authenticated EVENT_END envelope, borrowed for this callback. */
  const cai_transcript_event *boundary;
  /** One decoded execution chunk, including its original source attribution.
   * Borrowed only for this callback; do not clean up or retain its pointers. */
  const cai_execution_record *record;
  /** Semantic historical warning: nonzero code, borrowed bounded reason and
   * NULL record. Accept to display the warning and advance past bad content;
   * raw transcript data remains available unchanged. Transport errors and
   * local allocation failures still terminate the attachment. */
  int code;
  const char *reason;
  int replay, subagent;
} cai_app_execution_event;
typedef struct cai_app_client_config {
  const char *runtime_directory;
  const char *workspace;
  /** Optional 16-byte owner identity copied at open, checked by the server
   * before admitting an attachment. Set from verified selector discovery. */
  const unsigned char *expected_instance;
  /** Optional application startup requirements, copied by the transport.
   * A mismatch fails before acquiring controller authority. */
  const cai_workspace_claim *requirements;
  size_t requirement_count;
  uint64_t handshake_timeout_ms;
  size_t backlog_limit;
  /** Borrowed through close, invoked on the opening thread. May query state
   * and send requests/controls; must not reenter pump, detach or close. */
  int (*event)(void *, const cai_app_client_event *, cai_error *);
  void *context;
  /** Independent borrowed streaming presentation callback. No complete event
   * strings or transcript mirror are built by the client. Return CAI_OK only
   * after accepting a span; failures terminate this attachment. Same owner and
   * reentry rules as event above. Transport loss aborts a partial event. */
  int (*transcript)(void *, const cai_transcript_event *, cai_error *);
  void *transcript_context;
  /** Optional typed projection of execution/subagent transcript entries. Uses
   * the core incremental decoder: one bounded decoded chunk, no encoded JSON
   * accumulator, SDK client, worker or complete logical tool result. Called
   * at authenticated END before the raw END callback and cursor advancement.
   * Both consumers must accept END. Callback failure terminates this
   * attachment. Same owner/reentry rules as event; context is borrowed through
   * close. */
  int (*execution)(void *, const cai_app_execution_event *, cai_error *);
  void *execution_context;
  /** Prompt recall only, outside transcript/model context. Ordered bounded
   * spans use the transcript event shapes; visibility on BEGIN is the retained
   * entry limit. ENTRY_END accepts one input; REPLAY_END completes the load.
   * No complete history or prompt body is retained by this client. */
  int (*prompt_history)(void *, const cai_transcript_event *, cai_error *);
  void *prompt_history_context;
  /** Owner-pushed presentation metadata, including the actual loaded skills.
   * Independent of producer binding and model context; observers receive it
   * too. Borrowed only during this owner-thread callback. Partial candidates
   * must not be published; transport loss invalidates availability. Same
   * callback reentry rules as event. No SDK client or resource discovery. */
  int (*catalog)(void *, const cai_app_catalog_event *, cai_error *);
  void *catalog_context;
} cai_app_client_config;
void cai_app_client_config_init(cai_app_client_config *);
/** Explicit connect only: never forks, opens stores or submits input. All
 * operations require the opening thread. now_ms is monotonic. */
int cai_app_client_open(const cai_app_client_config *, uint64_t now_ms,
                        cai_app_client **out, cai_error *);
/** Borrow one descriptor and absolute monotonic deadline; do not read or close
 * the descriptor. Terminal states return fd=-1, events=0 and deadline=0. */
int cai_app_client_prepare(cai_app_client *, struct pollfd *,
                           uint64_t *deadline_ms, cai_error *);
/** Bounded nonblocking progress after readiness/deadline. No idle polling or
 * automatic reconnect. Protocol and callback failures are terminal. */
int cai_app_client_pump(cai_app_client *, uint64_t now_ms, cai_error *);
/** Copy fixed metadata; no allocation, file access or frontend mirror. */
int cai_app_client_get_state(cai_app_client *, cai_app_client_state *,
                             cai_error *);
/** Borrow the latest completed view until the next pump/close; NULL before the
 * first view. No queue producer is needed. A disconnected view remains
 * inspectable with stale=1. Callback consumers may query this getter. */
int cai_app_client_get_view(cai_app_client *, const cai_app_view **,
                            cai_error *);
/** Explicit producer selection after CONNECTED. Zero producer creates a new
 * producer; nonzero requires the exact session. After verifying the instance
 * on a new connection, epoch zero resolves that session's current epoch.
 * A failed bind never falls back to a fresh producer. Request IDs for binding
 * and queue operations are separate. Await END before ordinary submissions. */
int cai_app_client_bind(cai_app_client *, uint64_t request,
                        unsigned long long epoch, unsigned long long producer,
                        const char *session, cai_error *);
/** Select the workspace session before creating a producer or subscribing
 * to its transcript. selector is a session ID or listing number; NULL creates
 * a new session. Requires the controlling attachment and an unbound
 * client. Selection runs outside socket callbacks and rejects busy contexts.
 * Success arrives as the ordinary completed input baseline; failure is
 * BIND_REJECTED. No synthetic prompt, command or history entry is generated. */
int cai_app_client_select(cai_app_client *, uint64_t request,
                          const char *selector, cai_error *);
/** Opt this unbound producer into acknowledged delivery before bind. It stays
 * gated until settled; producers without dependent asynchronous requests keep
 * ordinary delivery. No frontend draft or editing state is transmitted. */
int cai_app_client_manage_delivery(cai_app_client *, cai_error *);
/** Announce that receipts and dependent producer requests have settled.
 * Scope is explicit; after_request is the observed processed watermark.
 * Ephemeral availability only: no request identity, receipt or queue mutation.
 * Announce again after each request or role change. */
int cai_app_client_settled(cai_app_client *, const cai_input_scope *,
                           unsigned long long after_request, cai_error *);
typedef enum cai_app_shell_operation {
  CAI_APP_SHELL_BEGIN = 0,
  CAI_APP_SHELL_WRITE,
  CAI_APP_SHELL_FINISH
} cai_app_shell_operation;
/** Explicit opt-in capture of a frontend-owned shell. BEGIN supplies a command
 * (1..8192 bytes, no NUL); its request ID becomes recording. WRITE supplies at
 * most 4096 raw bytes, including NUL. FINISH has no data. Sequences start at 1
 * after BEGIN and increase by exactly one. exit_code is 0..255. No execution
 * occurs in the server; the existing runtime retains only its bounded tail.
 * One request may be outstanding per recording. Wait for SHELL_RESULT before
 * sending the next chunk. Failed/unknown operations are not replayed/retried;
 * disconnect, takeover and context closure finalize the interrupted capture.
 */
typedef struct cai_app_shell_request {
  cai_app_shell_operation operation;
  uint64_t recording, sequence;
  const void *data;
  size_t length;
  unsigned int exit_code;
  unsigned long long duration_ms;
} cai_app_shell_request;
/** Owner-thread-only. Original scope/authority must still be current. A
 * successful return queues one bounded record, not an acknowledgement. */
int cai_app_client_shell(cai_app_client *, uint64_t request,
                         const cai_input_scope *, const cai_app_shell_request *,
                         cai_error *);
typedef struct cai_app_shell_result {
  uint64_t request, recording;
  unsigned long long first_sequence, last_sequence;
  int code;
  char reason[192];
} cai_app_shell_result;
/** Copy the latest shell acknowledgement, including its exact journal range.
 * Called on the owner thread, usually from SHELL_RESULT. The range lets a
 * frontend avoid repeating output it just displayed locally during catch-up.
 * Before the first acknowledgement, returns CAI_ERR_INVALID. */
int cai_app_client_get_shell_result(cai_app_client *, cai_app_shell_result *,
                                    cai_error *);
/** Send original scope and request unchanged. Authority is checked by the
 * server after durable receipt lookup, including retries from an observer or
 * old generation. No blind retries, ID allocation or optimistic queue changes.
 * While bound after a reset, only SYNCHRONIZE/RESOLVE are allowed until a new
 * baseline. Terminal resets require an explicit bind instead.
 * Transport handoff is distinct from the eventual durable RESULT. */
int cai_app_client_request(cai_app_client *, const cai_input_scope *,
                           const cai_input_request *, cai_error *);
int cai_app_client_takeover(cai_app_client *, uint64_t request,
                            uint64_t original_generation, cai_error *);
int cai_app_client_shutdown(cai_app_client *, uint64_t request,
                            uint64_t original_generation, cai_error *);
/** Cancel foreground work without stopping the server. Connection-scoped IDs
 * are independent of input IDs. Only repeat the last identical control. */
int cai_app_client_cancel(cai_app_client *, uint64_t request,
                          uint64_t original_generation, cai_error *);
/** Load a bounded prompt-history suffix for an explicit session. Zero limit
 * uses server policy. Shares the subscription request-ID namespace; independent
 * of the ordinary transcript stream. Observers may read without taking over. */
int cai_app_client_prompt_history(cai_app_client *, uint64_t request,
                                  uint64_t conversation, const char *session,
                                  unsigned int limit, cai_error *);
/** Save one bounded input without submitting it to the model. Await
 * HISTORY_RESULT before claiming durability or exiting after a draft save.
 * Controller only; unchanged repeats of the most recent request are idempotent.
 * Saving the same text as the last durable entry returns its existing position.
 * No auto retry. */
int cai_app_client_save_prompt(cai_app_client *, uint64_t request,
                               uint64_t original_generation, const char *,
                               cai_error *);
/** Subscribe to the active session without opening stores or queue producers.
 * Epoch zero resolves the explicitly named session. A resume position requires
 * its original collection identity and last accepted sequence. Request IDs
 * increase per attachment, independently of input/control IDs. Replacing a
 * subscription discards its unfinished candidate, never model input. Replay
 * ends at the captured watermark, then ordered live events follow. ERROR
 * requires explicit resubscription; no automatic retry or missing-data skip.
 * replay_turns selects the newest initial presentation entries: -1 all, 0
 * none, positive count. Cursor continuation requires -1 and never reapplies a
 * tail limit, so work performed while detached is not silently skipped. */
int cai_app_client_subscribe(cai_app_client *, uint64_t request,
                             uint64_t conversation, const char *session,
                             const char *expected_collection,
                             uint64_t after_position, uint64_t after_sequence,
                             long long replay_turns, cai_error *);
/** End only this transcript subscription, keeping input and attachment alive.
 * The terminal ERROR span targets its original stream identity. */
int cai_app_client_unsubscribe(cai_app_client *, cai_error *);
/** Drain DETACH using prepare/pump. A nonpersistent last-client server finishes
 * its current turn before stopping; reconnecting cancels that pending stop.
 * Persistent servers keep running. */
int cai_app_client_detach(cai_app_client *, cai_error *);
/** Immediate last-client detach; skips waiting for an active turn to finish.
 * Persistent servers and other attachments retain their existing lifetime. */
int cai_app_client_detach_now(cai_app_client *, cai_error *);
/** Borrow the canonical workspace path until close. */
const char *cai_app_client_directory(const cai_app_client *);
/** Immediate local release; never flushes, retries or submits a draft. */
void cai_app_client_close(cai_app_client *);
#ifdef __cplusplus
}
#endif
#endif
