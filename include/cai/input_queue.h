#ifndef CAI_INPUT_QUEUE_H
#define CAI_INPUT_QUEUE_H
#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_input_queue cai_input_queue;
typedef struct cai_input_queue_client cai_input_queue_client;

/** One session's accepted pending work. No editor or transport owns it. */
typedef enum cai_input_intent {
  CAI_INPUT_QUEUED = 0,
  CAI_INPUT_STEERING = 1
} cai_input_intent;
typedef enum cai_input_operation {
  CAI_INPUT_OFFER = 0,
  CAI_INPUT_BEGIN_EDIT,
  CAI_INPUT_END_EDIT,
  CAI_INPUT_ABORT_EDIT,
  CAI_INPUT_APPROVE,
  CAI_INPUT_WITHDRAW,
  CAI_INPUT_SET_INTENT,
  CAI_INPUT_STOP,
  CAI_INPUT_RESUME,
  CAI_INPUT_CLEAR,
  CAI_INPUT_AUTO_APPROVAL,
  CAI_INPUT_SYNCHRONIZE,
  CAI_INPUT_RESOLVE,
  /** Trusted producer acknowledges its consumed/withdrawn provenance receipt.
   * Frontend adapters expose only the ordinary operations above. */
  CAI_INPUT_FORGET_ORIGIN
} cai_input_operation;
typedef enum cai_input_outcome {
  CAI_INPUT_ACCEPTED = 0,
  CAI_INPUT_REJECTED,
  CAI_INPUT_UNKNOWN
} cai_input_outcome;

/** Copied by an asynchronous adapter before its submission callback returns. */
typedef struct cai_input_request {
  unsigned long long request_id;
  cai_input_operation operation;
  unsigned long long item_id, item_revision, queue_revision, edit_token;
  const char *text;
  cai_input_intent intent;
  /** OFFER: require approval. AUTO_APPROVAL: enable automatic approval.
   * Otherwise unused. A fresh host action does not restart stopped backlog. */
  int enabled;
  unsigned long long target_request_id;
  /** Optional producer idempotency key, for example an incoming message ID.
   * Empty for human input. Retained through claim until producer
   * acknowledgement. */
  char origin[128];
} cai_input_request;
/** Value result. Only ordered queue observations change a frontend mirror. */
typedef struct cai_input_result {
  unsigned long long request_id, item_id, item_revision, edit_token;
  cai_input_outcome outcome;
  int code;
  char reason[192];
} cai_input_result;

/** Borrowed during inspection/change callbacks. */
typedef struct cai_input_entry {
  unsigned long long id, revision, client_id, request_id, edit_owner,
      edit_token;
  const char *text;
  cai_input_intent intent;
  int held, edit_held, ready, stopped, action, independent;
  char origin[128];
} cai_input_entry;
/** Successful consumption owns text separately from the borrowed entry view.
 * Claimed entries are no longer editable pending work. */
typedef struct cai_input_claim {
  cai_input_entry entry;
  char *text;
  unsigned long long position;
  /** Owned payload-bearing queue event identities. Associate these with the
   * delivered history entry so trimming also removes its earlier draft edits.
   */
  unsigned long long *input_events;
  size_t input_event_count;
} cai_input_claim;
typedef struct cai_input_state {
  size_t count;
  unsigned long long revision, view_revision;
  int stopped, auto_approval, uncertain;
} cai_input_state;

/** Backend classification; core never parses slash commands. */
typedef struct cai_input_policy {
  cai_input_intent intent;
  /** Fresh unheld offers may resume the entire backlog when resume_backlog is
   * set. An explicit steering promotion releases only the promoted item. */
  int action, independent, resume_backlog;
} cai_input_policy;

typedef enum cai_input_record_type {
  CAI_INPUT_RECORD_CLIENT = 0,
  CAI_INPUT_RECORD_REQUEST,
  CAI_INPUT_RECORD_CLAIM,
  /** Controller-origin disconnect cleanup; no frontend request identity. */
  CAI_INPUT_RECORD_RELEASE
} cai_input_record_type;
/** One incremental durable transition, never a whole-queue snapshot. Text and
 * request pointers are borrowed through persist(). The callback must append
 * atomically or report failure; a failed append fences further mutations until
 * authoritative recovery. Readiness changes are ephemeral and are not stored.
 */
typedef struct cai_input_record {
  cai_input_record_type type;
  unsigned long long before_revision, after_revision, client_id;
  cai_input_request request;
  cai_input_policy policy;
  cai_input_result result;
  cai_input_entry entry;
  /** Borrowed payload event identities for CLAIM; callbacks may copy them. */
  const unsigned long long *input_events;
  size_t input_event_count;
} cai_input_record;

typedef enum cai_input_change_type {
  CAI_INPUT_UPSERT = 0,
  CAI_INPUT_REMOVE,
  CAI_INPUT_STATE
} cai_input_change_type;
typedef struct cai_input_change {
  cai_input_change_type type;
  size_t index;
  unsigned long long previous_view_revision, client_id, request_id;
  cai_input_entry entry;
  cai_input_state state;
} cai_input_change;
typedef int (*cai_input_classify_fn)(void *context, const char *text,
                                     cai_input_policy *policy,
                                     cai_error *error);
typedef void (*cai_input_changed_fn)(void *context,
                                     const cai_input_change *change);

typedef struct cai_input_queue_config {
  /** Zero selects 128 entries and 66048 bytes per input. */
  size_t max_entries, max_text_bytes;
  /** NULL explicitly selects volatile operation. A runtime binds its existing
   * session journal here before admitting frontend work. */
  int (*persist)(void *context, const cai_input_record *record,
                 unsigned long long *position, cai_error *error);
  void *persist_context;
  /** Optional controller policy, called only for new/replaced text or intent.
   * Must not re-enter the queue. Default is ordinary model input. */
  cai_input_classify_fn classify;
  void *classify_context;
  /** Notification follows commit. Failure to observe cannot undo acceptance.
   * Copy borrowed fields before handing them to a frontend. Never re-enter. */
  cai_input_changed_fn changed;
  void *changed_context;
} cai_input_queue_config;

/** Queue mutations/inspection run on its controller owner thread. Async hosts
 * copy requests into a bounded mailbox and signal the controller's wakeup fd.
 * Never call these methods from an editor's synchronous submission callback.
 */
void cai_input_queue_config_init(cai_input_queue_config *config);
int cai_input_queue_open(const cai_input_queue_config *config,
                         cai_input_queue **out, cai_error *error);
/** Release controller ownership and reject further operations. Client handles
 * retain allocation only until their close; they do not retain journal access.
 */
void cai_input_queue_close(cai_input_queue *queue);
/** Owner-thread allocation reference for controllers/adapters. This does not
 * retain journal authority: close still shuts the queue down immediately.
 * Balance successful retain with release, never a second close. */
int cai_input_queue_retain(cai_input_queue *queue, cai_error *error);
void cai_input_queue_release(cai_input_queue *queue);
int cai_input_queue_client_open(cai_input_queue *queue,
                                cai_input_queue_client **out, cai_error *error);
/** Resume an existing logical producer after transport loss. No new producer
 * is reserved and no journal record is appended. Only one live handle may own
 * an identity. The queue retains receipts for the 16 most recently used
 * detached producers (16 results per producer); journal replay restores this
 * bounded cache. If unavailable, fail rather than readmit an uncertain offer.
 * A trimmed/export initialization baseline can deliberately omit old receipts.
 * Inspect/reconcile accepted work; never automatically reoffer an unknown
 * request with a new producer or request ID. */
int cai_input_queue_client_resume(cai_input_queue *queue, unsigned long long id,
                                  cai_input_queue_client **out,
                                  cai_error *error);
/** Detaches observations; accepted entries and edit holds remain. The logical
 * producer's bounded receipts may be resumed independently of this handle. */
void cai_input_queue_client_close(cai_input_queue_client *client);
/** Owner-only durable release of this client's edit holds. Retains all text,
 * approval/stopped state and receipt IDs. It never reserves an internal
 * frontend request ID. No journal append when no holds belong to the client. */
int cai_input_queue_client_release_edits(cai_input_queue_client *, cai_error *);
unsigned long long
cai_input_queue_client_id(const cai_input_queue_client *client);
/** Highest processed request ID known to this producer. On reconnect, keep
 * the larger of this and the frontend's own last issued ID. Read-only IDs need
 * not survive a server restart; uncertain submissions keep their original ID.
 */
unsigned long long
cai_input_queue_client_last_request(const cai_input_queue_client *client);
int cai_input_queue_request(cai_input_queue_client *client,
                            const cai_input_request *request,
                            cai_input_result *result, cai_error *error);
int cai_input_queue_get_state(const cai_input_queue *queue,
                              cai_input_state *out, cai_error *error);
/** Replace controller callbacks without replacing accepted work or its journal.
 * NULL clears the corresponding callback. Existing entries keep their policy
 * until the controller deliberately submits a revision-checked change. */
int cai_input_queue_set_policy(cai_input_queue *queue,
                               cai_input_classify_fn classify, void *context,
                               cai_error *error);
int cai_input_queue_set_observer(cai_input_queue *queue,
                                 cai_input_changed_fn changed, void *context,
                                 cai_error *error);
/** Trusted controller inspection, including the edit token. Never expose this
 * view to a frontend: use client-filtered entry_at() there instead. */
int cai_input_queue_lookup(cai_input_queue *queue, unsigned long long id,
                           cai_input_entry *out, cai_error *error);
typedef enum cai_input_origin_status {
  CAI_INPUT_ORIGIN_PENDING = 0,
  CAI_INPUT_ORIGIN_CLAIMED,
  CAI_INPUT_ORIGIN_WITHDRAWN
} cai_input_origin_status;
/** Trusted producer reconciliation. A consumed receipt has no text payload.
 * Missing keys return CAI_ERR_INVALID. Never infer delivery from rendered text.
 */
int cai_input_queue_find_origin(cai_input_queue *queue, const char *origin,
                                cai_input_entry *out,
                                cai_input_origin_status *status,
                                cai_error *error);
/** Trusted borrowed provenance, valid until the next mutation. */
int cai_input_queue_input_events(cai_input_queue *queue, unsigned long long id,
                                 const unsigned long long **events,
                                 size_t *count, cai_error *error);
/** Hide other clients' edit tokens. Borrowed text lasts until next mutation. */
int cai_input_queue_entry_at(cai_input_queue_client *client, size_t index,
                             cai_input_entry *out, cai_error *error);
int cai_input_queue_set_ready(cai_input_queue *queue, unsigned long long id,
                              int ready, cai_error *error);
/** Commit consumption before transferring ownership of text to the consumer.
 * position is the durable claim record's journal position (zero if volatile).
 * The caller must prepare any fallible execution resources before claiming.
 */
int cai_input_queue_claim(cai_input_queue *queue, unsigned long long id,
                          unsigned long long revision, cai_input_claim *out,
                          cai_error *error);
void cai_input_claim_cleanup(cai_input_claim *claim);
/** Replay one committed transition while restoring a session. Does no
 * I/O or notifications and never claims work or executes model/host actions. */
int cai_input_queue_restore(cai_input_queue *queue,
                            const cai_input_record *record,
                            unsigned long long position, cai_error *error);
/** After authoritative storage inspection proves the failed append absent,
 * reject that original request and release the uncertainty fence. A committed
 * append must instead be recovered with restore(). Never infer absence from a
 * timeout, transport error or stale cache. */
int cai_input_queue_confirm_absent(cai_input_queue *queue, cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
