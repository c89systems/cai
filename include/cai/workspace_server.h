/** @file cai/workspace_server.h
 * Owner-loop Unix workspace host. No runtime, renderer or worker thread.
 */
#ifndef CAI_WORKSPACE_SERVER_H
#define CAI_WORKSPACE_SERVER_H
#include <cai/attachment.h>
#include <cai/protocol.h>
#include <poll.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_workspace_server cai_workspace_server;
#define CAI_WORKSPACE_SERVER_FDS (CAI_ATTACHMENT_LIMIT + 1U)
typedef enum cai_workspace_server_event_type {
  CAI_WORKSPACE_ATTACHED = 0,
  CAI_WORKSPACE_DETACHED,
  CAI_WORKSPACE_MESSAGE,
  CAI_WORKSPACE_PEER_ERROR
} cai_workspace_server_event_type;
typedef struct cai_workspace_server_event {
  cai_workspace_server_event_type type;
  /** Authenticated connection identity; zero for an unfinished handshake.
   * Never derive authority from an untrusted protocol header. */
  uint64_t attachment;
  /** Borrowed MESSAGE event. Payload remains incremental/provisional until
   * COMPLETE. Admission and durable receipts belong to the application.
   * Inspect authority again at actual application dispatch. */
  const cai_protocol_event *message;
  /** Borrowed for PEER_ERROR only, valid until callback returns. */
  const cai_error *error;
} cai_workspace_server_event;
typedef int (*cai_workspace_server_event_fn)(
    void *context, const cai_workspace_server_event *event, cai_error *error);
typedef struct cai_workspace_server_config {
  const char *runtime_directory;
  const char *workspace;
  /** Optional unique discovery name. Copied into the publication at open.
   * NULL/empty leaves the owner discoverable by canonical path and stable ID.
   */
  const char *name;
  cai_attachment_config lifetime;
  /** Zero selects 10 seconds for an incomplete greeting. Completed greetings
   * can wait through STARTING; READY begins the first-attachment grace period.
   */
  uint64_t handshake_timeout_ms;
  size_t backlog_limit;
  /** Optional callback; context is borrowed until server close. */
  cai_workspace_server_event_fn event;
  void *context;
} cai_workspace_server_config;
void cai_workspace_server_config_init(cai_workspace_server_config *config);
/** Canonicalize identity, take endpoint ownership and listen. No agent or
 * storage is opened. All calls require the opening thread. A host restores
 * its runtime asynchronously before publishing ready. Identity-only PROBE
 * requests may complete during startup without admitting an attachment. */
int cai_workspace_server_open(const cai_workspace_server_config *config,
                              cai_workspace_server **out, cai_error *error);
int cai_workspace_server_ready(cai_workspace_server *server, uint64_t now_ms,
                               cai_error *error);
/** Borrow poll interests, including POLLOUT only for queued output. Capacity
 * must be CAI_WORKSPACE_SERVER_FDS. deadline is an absolute monotonic time,
 * zero if none. Use it to schedule a single timer; no idle polling required.
 * Descriptors remain borrowed and may change after pump. */
int cai_workspace_server_prepare(cai_workspace_server *server,
                                 struct pollfd *fds, size_t capacity,
                                 size_t *count, uint64_t *deadline_ms,
                                 cai_error *error);
/** Bounded nonblocking accept/read/flush and timer expiry. Call after readiness
 * or the scheduled deadline, using nondecreasing monotonic timestamps.
 * Peer failures disconnect only that peer and notify PEER_ERROR; they never
 * stop accepted application work. No host callback may reenter pump/close.
 * Callbacks may send replies or hand off bounded controls. Run inference,
 * restoration and storage outside socket dispatch. Callback failure
 * disconnects its peer. */
int cai_workspace_server_pump(cai_workspace_server *server, uint64_t now_ms,
                              cai_error *error);
int cai_workspace_server_get_state(cai_workspace_server *server,
                                   cai_attachment_state *out, cai_error *error);
int cai_workspace_server_authorize(cai_workspace_server *server,
                                   uint64_t attachment, uint64_t generation,
                                   cai_error *error);
/** Mark irreversible STOPPING. The host still owns cancelling/joining its
 * runtime, closing stores and finally releasing this server. */
int cai_workspace_server_stop(cai_workspace_server *server, cai_error *error);
/** Current bounded output backlog for a live attachment. Use it to gate
 * additional producer work; readiness still comes from prepare(). */
int cai_workspace_server_pending(cai_workspace_server *, uint64_t attachment,
                                 size_t *out, cai_error *);
/** Owner-only immediate detach outside pump callbacks. Revokes authority
 * before returning and broadcasts the resulting role. No runtime is stopped
 * here; the application observes the ordinary last-client lifetime. */
int cai_workspace_server_disconnect(cai_workspace_server *, uint64_t attachment,
                                    cai_error *);
/** Stream one output record to a connected attachment. begin replaces the
 * attachment/generation metadata with server-owned values. Known bounded
 * payload sizes must come from fields/chunks, never a sizing serialization.
 * pump may flush a partial record. Role notifications wait until end, in a
 * bounded queue; overflow disconnects only this subscriber.
 * Failure invalidates this subscriber; pump disconnects it. Other peers and
 * accepted work remain independent. */
int cai_workspace_server_begin(cai_workspace_server *server,
                               uint64_t attachment,
                               const cai_protocol_header *header,
                               uint32_t payload_length, cai_error *error);
int cai_workspace_server_write(cai_workspace_server *server,
                               uint64_t attachment, const cai_cbor_token *token,
                               cai_error *error);
int cai_workspace_server_end(cai_workspace_server *server, uint64_t attachment,
                             cai_error *error);
/** Immutable borrowed identities, valid until server close. */
const char *cai_workspace_server_directory(const cai_workspace_server *server);
const unsigned char *
cai_workspace_server_instance(const cai_workspace_server *server);
/** Close only after application workers and stores have stopped. Detaches
 * peers and releases endpoint ownership; never exits an embedding process. */
void cai_workspace_server_close(cai_workspace_server *server);
#ifdef __cplusplus
}
#endif
#endif
