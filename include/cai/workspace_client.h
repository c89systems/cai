/** @file cai/workspace_client.h
 * Owner-loop streaming attachment client. No runtime, storage or renderer.
 */
#ifndef CAI_WORKSPACE_CLIENT_H
#define CAI_WORKSPACE_CLIENT_H
#include <cai/attachment.h>
#include <cai/protocol.h>
#include <poll.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_workspace_client cai_workspace_client;
typedef enum cai_workspace_client_phase {
  CAI_WORKSPACE_CONNECTING = 0,
  CAI_WORKSPACE_GREETING,
  CAI_WORKSPACE_CONNECTED,
  CAI_WORKSPACE_DETACHING,
  CAI_WORKSPACE_DISCONNECTED,
  CAI_WORKSPACE_FAILED,
  /** Successful terminal identity-only query, with no attachment. */
  CAI_WORKSPACE_IDENTIFIED
} cai_workspace_client_phase;
typedef struct cai_workspace_client_state {
  cai_workspace_client_phase phase;
  uint64_t attachment, generation, controller, deadline_ms;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  int has_welcome, persist, error_code;
  /** Set only after a validated identity-only reply; attachment, generation
   * and controller remain zero. Snapshot, not a subscription to readiness. */
  int has_identity;
  cai_attachment_phase server_phase;
} cai_workspace_client_state;
typedef struct cai_workspace_client_config {
  const char *runtime_directory;
  const char *workspace;
  /** Zero means no deadline: restoration may take time while this event loop
   * remains responsive. Otherwise bounds connect/WELCOME/initial ROLE. */
  uint64_t handshake_timeout_ms;
  size_t backlog_limit;
  /** False by default. Query canonical identity, instance, persistence and
   * startup phase without attaching, taking control or extending lifetime.
   * Completes during STARTING too; IDENTIFIED is terminal and closes transport.
   * No runtime/storage is opened. Controls and requests remain inadmissible.
   * Set handshake_timeout_ms to bound an unavailable/unresponsive owner. */
  int probe;
  /** Borrowed callback/context until close. WELCOME and ROLE are forwarded only
   * as COMPLETE after their entire identity/schema is validated. Identity-only
   * replies follow this same rule. Other records
   * arrive as provisional HEADER/PAYLOAD then validated COMPLETE, without a
   * payload accumulator. Callbacks may enqueue output, but must not reenter
   * pump, detach or close. Callback failures are terminal. */
  cai_protocol_event_fn event;
  void *context;
} cai_workspace_client_config;
void cai_workspace_client_config_init(cai_workspace_client_config *config);
/** Connect to an existing owner; never starts a process, acquires its lock or
 * opens session storage. Uses canonical workspace identity and peer UID checks.
 * All operations require this opening thread. now_ms is monotonic. */
int cai_workspace_client_open(const cai_workspace_client_config *config,
                              uint64_t now_ms, cai_workspace_client **out,
                              cai_error *error);
/** Borrow one poll interest and its absolute deadline. fd=-1/events=0 means
 * terminal. No POLLOUT interest when there is no connection/output work. */
int cai_workspace_client_prepare(cai_workspace_client *client,
                                 struct pollfd *fd, uint64_t *deadline_ms,
                                 cai_error *error);
/** Bounded nonblocking I/O, after readiness or the deadline. No worker or idle
 * polling. EOF closes this attachment; it never stops a server explicitly.
 * Errors are terminal. Reconnect requires a new handle, verification of its
 * server instance and reconciliation of uncertain work; no automatic reoffer.
 */
int cai_workspace_client_pump(cai_workspace_client *client, uint64_t now_ms,
                              cai_error *error);
int cai_workspace_client_get_state(cai_workspace_client *client,
                                   cai_workspace_client_state *out,
                                   cai_error *error);
/** Stream a REQUEST or SYNCHRONIZE payload from known fields/chunks. The
 * attachment is bound by the greeting. generation is the caller's original
 * scope and is NEVER refreshed to revive an older pending mutation. mutation
 * requires current controlling authority locally; the server must independently
 * classify/authorize requests at actual admission. No request is sent before
 * validated WELCOME. Read-only requests may precede the initial ROLE.
 * Success is only transport handoff, not acceptance/durability/application.
 */
int cai_workspace_client_begin(cai_workspace_client *client,
                               const cai_protocol_header *header,
                               uint32_t payload_length, int mutation,
                               cai_error *error);
int cai_workspace_client_write(cai_workspace_client *client,
                               const cai_cbor_token *token, cai_error *error);
int cai_workspace_client_end(cai_workspace_client *client, cai_error *error);
/** Explicit controls; original generation must still be current. */
int cai_workspace_client_takeover(cai_workspace_client *client,
                                  uint64_t request, uint64_t generation,
                                  cai_error *error);
int cai_workspace_client_shutdown(cai_workspace_client *client,
                                  uint64_t request, uint64_t generation,
                                  cai_error *error);
/** Gracefully enqueue DETACH and drain bounded output through pump. Before
 * WELCOME, or during a partial outgoing record, disconnects immediately rather
 * than sending an invalid record. Never blocks or waits for server shutdown.
 * The executable separately waits for nonpersistent server exit/temp cleanup.
 */
int cai_workspace_client_detach(cai_workspace_client *client, cai_error *error);
/** Borrow the canonical directory until close. */
const char *cai_workspace_client_directory(const cai_workspace_client *client);
/** Immediate local release; never implicitly flushes or retries output. */
void cai_workspace_client_close(cai_workspace_client *client);
#ifdef __cplusplus
}
#endif
#endif
