#ifndef CAI_CLI_POUCH_H
#define CAI_CLI_POUCH_H

#include "options.h"
#include "pouch_stream.h"
#include "transcript.h"
#include <cai/agent_runtime.h>
#include <cai/blob_store.h>
#include <cai/session_store.h>
#include <cai/tools/todo.h>
#include <limits.h>
#include <pthread.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct cai_cli_session {
  char id[CAI_AGENT_SESSION_ID_MAX];
  char workspace[PATH_MAX];
  char first_prompt[512];
  char history_backend[16];
  lonejson_uint64 commit_position;
  lonejson_uint64 commit_ns;
  lonejson_uint64 applied_sequence;
  lonejson_int64 published;
  lonejson_int64 retention_override, retention_days;
} cai_cli_session;

typedef struct cai_cli_pouch {
  lc_client *sessions;
  struct cai_cli_pouch_client *connection;
  /* Keep the active journal open; operation receivers are independently owned.
   */
  lc_collection *history_anchor;
  /* Owner-thread prompt recall; shares the session client, never model input.
   */
  lc_collection *prompt_history;
  int prompt_history_read_only;
  long conversation_retention_seconds;
  pthread_mutex_t history_lock;
  int history_lock_initialized;
  /* Write contexts live through the active conversation and its children.
   * Collection appends never acquire a state-object lease per observation. */
  pthread_mutex_t journal_lock;
  int journal_lock_initialized;
  struct cai_cli_journal_context *journal_contexts;
  /* Borrowed nonblocking observer, called under journal_lock after durable
   * publication. NULL event announces an atomic history cut. No SDK calls,
   * waiting, rendering or reentry is allowed from this hook. */
  void (*journal_observer)(void *, const char *, unsigned long long,
                           const cai_agent_session_event *);
  void *journal_observer_context;
  /* One renewal service for this store context, independent of journal writes.
   * Receivers retain their SDK lease until the context exits. */
  struct cai_cli_collection_owner *collection_owners;
  pthread_mutex_t ownership_lock;
  pthread_mutex_t ownership_health_lock;
  pthread_cond_t ownership_condition;
  pthread_t ownership_thread;
  int ownership_initialized, ownership_running, ownership_stopping;
  int ownership_failed;
  int ownership_pipe[2];
  char ownership_error[512];
  char program_name[33], queue_prefix[33];
  char sessions_namespace[129], auth_namespace[129], scopes_namespace[129];
  char todo_namespace[129], messages_namespace[129];
  cai_agent_session_store store;
  cai_blob_store credentials;
  pslog_logger *logger;
  /* Borrowed, synchronous replay observer; installed only during CLI open. */
  int (*replay_begin)(void *, const char *, cai_error *);
  cai_agent_session_event_fn replay_event;
  void *replay_context;
  const char *replay_scope;
  pthread_t owner_thread;
  char state_directory[PATH_MAX];
  char config_directory[PATH_MAX];
  char key_path[PATH_MAX];
  char root_directory[PATH_MAX];
  struct cai_cli_session_guard *use_guard;
  int capture_session;
  /* Sparse, transient sequence -> native collection-position hints. No text,
   * snapshots, extra store or persisted cache. One point per 128 events. */
  struct cai_cli_event_position *event_positions;
  size_t event_position_count, event_position_capacity, event_position_stride;
  char event_position_session[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long event_last_sequence, event_last_position;
} cai_cli_pouch;

int cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                               const cai_cli_options *options,
                               pslog_logger *logger, cai_error *error);
int cai_cli_pouch_open(cai_cli_pouch *pouch, const char *endpoint,
                       const char *client_pem, pslog_logger *logger,
                       cai_error *error);
int cai_cli_pouch_open_at(cai_cli_pouch *pouch, const char *state_directory,
                          const char *endpoint, const char *client_pem,
                          pslog_logger *logger, cai_error *error);
void cai_cli_pouch_close(cai_cli_pouch *pouch);
/* Exact canonical workspace or nearest approved canonical ancestor; empty
 * root means no approval. Public committed metadata only, no journal scan. */
int cai_cli_pouch_workspace_approval(cai_cli_pouch *pouch,
                                     const char *workspace, char *root,
                                     size_t capacity, cai_error *error);
int cai_cli_pouch_approve_workspace(cai_cli_pouch *pouch, const char *workspace,
                                    const char *session_id, cai_error *error);
/* Read committed history without acquiring writable runtime ownership. */
int cai_cli_pouch_read_id(cai_cli_pouch *pouch, const char *scope,
                          const char *id, cai_source **out,
                          unsigned long long *sequence, cai_error *error);
int cai_cli_pouch_prompt_open(cai_cli_pouch *pouch, const char *id,
                              cai_error *error);
/* Public committed recall without mutation authority. Missing storage reads
 * as an empty history; it is never created by this opener. */
int cai_cli_pouch_prompt_open_read(cai_cli_pouch *pouch, const char *id,
                                   cai_error *error);
void cai_cli_pouch_prompt_close(cai_cli_pouch *pouch);
/* SDK maintenance owns oldest-first pruning; zero clears the stored count cap.
 * Applying an unchanged limit does not write metadata. */
int cai_cli_pouch_prompt_set_limit(cai_cli_pouch *pouch, unsigned int limit,
                                   cai_error *error);
/* Apply recall's stored-entry cap, retaining an enabled receiver. Zero clears
 * an existing cap and closes recall without creating a missing collection. */
int cai_cli_pouch_prompt_configure(cai_cli_pouch *pouch, const char *id,
                                   unsigned int limit, cai_error *error);
int cai_cli_pouch_prompt_load(cai_cli_pouch *pouch, unsigned int limit,
                              int (*emit)(void *, const char *, cai_error *),
                              void *context, cai_error *error);
int cai_cli_pouch_prompt_append(cai_cli_pouch *pouch, const char *prompt,
                                cai_error *error);
int cai_cli_pouch_retention(cai_cli_pouch *pouch, const char *id, long *days,
                            cai_error *error);
int cai_cli_pouch_set_retention(cai_cli_pouch *pouch, const char *id, long days,
                                cai_error *error);
/* Console tail only: bounded collection range, selection before rendering,
 * without invoking recovery or startup observers a second time. */
int cai_cli_pouch_replay(cai_cli_pouch *pouch, const char *scope,
                         const char *id, unsigned long long after,
                         unsigned long long through,
                         int (*select_event)(void *, unsigned long long),
                         cai_agent_session_event_fn visit, void *context,
                         cai_error *error);
typedef struct cai_cli_pouch_transcript {
  lc_collection *collection;
  int reading;
} cai_cli_pouch_transcript;
/* One public collection receiver through a transcript context's lifetime.
 * Reuses pouch's existing SDK client; never acquires a write lease or restores
 * model context. Serialize calls, and close before closing the parent pouch. */
int cai_cli_pouch_transcript_open(cai_cli_pouch *, const char *scope,
                                  const char *id,
                                  cai_cli_pouch_transcript **out, cai_error *);
/* Synchronous true streaming; schedule on the context reader, not a UI/owner
 * thread. Native bounds/cursors/cancellation apply. Query selectors and
 * on_metadata callbacks are unavailable: no LQL decode/filter pass is used.
 * out follows the SDK's init/cleanup contract, including progress on errors.
 * BEGIN/DATA remain provisional on failure/cancellation; caller abandons the
 * candidate and resumes only from an accepted record cursor. No auto retry. */
int cai_cli_pouch_transcript_read(cai_cli_pouch_transcript *,
                                  const lc_collection_read_opts *,
                                  lc_collection_read_res *out,
                                  cai_cli_transcript_fn, void *, cai_error *);
void cai_cli_pouch_transcript_close(cai_cli_pouch_transcript *);
int cai_cli_pouch_seed_auth(cai_cli_pouch *pouch, const char *path,
                            cai_error *error);
int cai_cli_pouch_list(cai_cli_pouch *pouch, const char *scope,
                       cai_cli_session **out, size_t *count, cai_error *error);
int cai_cli_pouch_session(cai_cli_pouch *pouch, const char *id,
                          cai_cli_session *out, cai_error *error);
int cai_cli_pouch_publish(cai_cli_pouch *pouch, const char *id, int published,
                          cai_error *error);
/* Capture the root runtime's identity and acquire SDK ownership before writable
 * restoration. A shared filesystem pin prevents deletion; a separate native
 * liveness guard enables recovery of an abandoned SDK lease. All close after
 * runtime/prompt writers stop at end_use. */
void cai_cli_pouch_begin_use(cai_cli_pouch *pouch);
void cai_cli_pouch_end_use(cai_cli_pouch *pouch);
/* A sticky renewal failure stops the context; never silently reacquire while
 * its restored runtime may be stale. The fd wakes an interactive owner. */
int cai_cli_pouch_ownership_check(cai_cli_pouch *pouch, cai_error *error);
int cai_cli_pouch_ownership_fd(cai_cli_pouch *pouch);
int cai_cli_pouch_begin_read(cai_cli_pouch *pouch, const char *id,
                             struct cai_cli_session_guard **guard,
                             cai_error *error);
void cai_cli_pouch_end_read(struct cai_cli_session_guard *guard);
int cai_cli_pouch_deletable(cai_cli_pouch *pouch, const char *id, int *eligible,
                            cai_error *error);
int cai_cli_pouch_delete(cai_cli_pouch *pouch, const char *workspace,
                         const char *id,
                         int (*cleanup)(void *, const char *, cai_error *),
                         void *context, cai_error *error);
int cai_cli_pouch_lock(cai_cli_pouch *pouch, const char *id, lc_lease **out,
                       cai_error *error);
int cai_cli_pouch_unlock(lc_lease *lease, int result, cai_error *error);
/* Global kanban document, encrypted in cai.todo. Context is the borrowed pouch.
 * Intermediate rewrites stay in encrypted attachments until transaction commit.
 */
const cai_todo_store_callbacks *cai_cli_pouch_todo_callbacks(void);
/* Stream visible observation events and one retained-context initialization
 * commit. Trimmed content is excluded; no inference or tool execution. */
int cai_cli_pouch_export(cai_cli_pouch *pouch, const char *id,
                         cai_agent_runtime *runtime, cai_sink *sink,
                         cai_error *error);
int cai_cli_pouch_import(cai_cli_pouch *pouch, const char *path,
                         const char *workspace, char *id, size_t capacity,
                         cai_error *error);

#endif
