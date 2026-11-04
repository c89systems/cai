/** @file cai/app_client.h
 * Typed application input and transcript client. Owns no runtime, store or
 * terminal.
 */
#ifndef CAI_APP_CLIENT_H
#define CAI_APP_CLIENT_H
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
  CAI_APP_CLIENT_STATUS
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
/** Subscribe to the active session without opening stores or queue producers.
 * Epoch zero resolves the explicitly named session. A resume position requires
 * its original collection identity and last accepted sequence. Request IDs
 * increase per attachment, independently of input/control IDs. Replacing a
 * subscription discards its unfinished candidate, never model input. Replay
 * ends at the captured watermark, then ordered live events follow. ERROR
 * requires explicit resubscription; no automatic retry or missing-data skip. */
int cai_app_client_subscribe(cai_app_client *, uint64_t request,
                             uint64_t conversation, const char *session,
                             const char *expected_collection,
                             uint64_t after_position, uint64_t after_sequence,
                             cai_error *);
/** End only this transcript subscription, keeping input and attachment alive.
 * The terminal ERROR span targets its original stream identity. */
int cai_app_client_unsubscribe(cai_app_client *, cai_error *);
/** Drain DETACH using prepare/pump. Default nonpersistent servers stop after
 * their last attachment leaves; persistent servers keep running. */
int cai_app_client_detach(cai_app_client *, cai_error *);
/** Borrow the canonical workspace path until close. */
const char *cai_app_client_directory(const cai_app_client *);
/** Immediate local release; never flushes, retries or submits a draft. */
void cai_app_client_close(cai_app_client *);
#ifdef __cplusplus
}
#endif
#endif
