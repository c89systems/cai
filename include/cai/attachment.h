/** @file cai/attachment.h
 * Owner-thread attachment authority and workspace-server lifetime.
 * No terminal, socket, runtime, worker thread or process-global state.
 */
#ifndef CAI_ATTACHMENT_H
#define CAI_ATTACHMENT_H
#include <cai/cai.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CAI_ATTACHMENT_LIMIT 16U
typedef struct cai_attachment_controller cai_attachment_controller;
typedef enum cai_attachment_phase {
  CAI_ATTACHMENT_STARTING = 0,
  CAI_ATTACHMENT_READY,
  CAI_ATTACHMENT_STOPPING
} cai_attachment_phase;
typedef enum cai_attachment_stop_reason {
  CAI_ATTACHMENT_NOT_STOPPED = 0,
  CAI_ATTACHMENT_LAST_DETACHED,
  CAI_ATTACHMENT_STARTUP_ABANDONED,
  CAI_ATTACHMENT_STOP_REQUESTED
} cai_attachment_stop_reason;
typedef struct cai_attachment_config {
  /** False by default. Headless executable startup explicitly selects true.
   * An attachment never changes the server's selected persistence. */
  int persist;
  /** Zero selects 30 seconds after READY to receive the first attachment.
   * Ignored for persistent servers. Uses caller-supplied monotonic time. */
  uint64_t first_attachment_timeout_ms;
} cai_attachment_config;
typedef struct cai_attachment_state {
  cai_attachment_phase phase;
  cai_attachment_stop_reason stop_reason;
  size_t count;
  int persist;
  uint64_t controller, generation;
  /** Zero means no timer. Stop/last detach never waits for this timer. */
  uint64_t deadline_ms;
} cai_attachment_state;

void cai_attachment_config_init(cai_attachment_config *config);
int cai_attachment_controller_open(const cai_attachment_config *config,
                                   cai_attachment_controller **out,
                                   cai_error *error);
/** Close only after the host has stopped its runtime/transport. All methods,
 * including close, require the creating thread; no callbacks are invoked. */
void cai_attachment_controller_close(cai_attachment_controller *controller);
/** Publish readiness only after successful startup. Starts the first-client
 * grace period; restoring context does not consume that period. */
int cai_attachment_controller_ready(cai_attachment_controller *controller,
                                    uint64_t now_ms, cai_error *error);
/** Admit an authenticated, handshaken attachment. Count observers as well as
 * the controller. The first attachment, or a new one arriving while control
 * is vacant, receives control. Existing observers must explicitly take over.
 * Never call for an incomplete/untrusted socket handshake. */
int cai_attachment_controller_attach(cai_attachment_controller *controller,
                                     uint64_t now_ms, uint64_t *id,
                                     cai_error *error);
/** Graceful and abrupt disconnects use this same method. The last disconnect
 * stops a nonpersistent server immediately. It cannot subsequently be revived.
 * Already accepted work is owned by the runtime, not this attachment handle. */
int cai_attachment_controller_detach(cai_attachment_controller *controller,
                                     uint64_t id, cai_error *error);
/** Explicit takeover advances generation, revoking the previous controller.
 * Idempotent when already controlling. Returns the current generation. */
int cai_attachment_controller_takeover(cai_attachment_controller *controller,
                                       uint64_t id, uint64_t *generation,
                                       cai_error *error);
/** Check at actual mutation admission, against the generation carried by the
 * request, not merely at socket receipt. Observer/stale/stopping requests fail.
 * Pending requests do not regain authority if their sender later takes over. */
int cai_attachment_controller_authorize(cai_attachment_controller *controller,
                                        uint64_t id, uint64_t generation,
                                        cai_error *error);
/** Host-owned orderly stop, also valid during startup. Does not exit a process
 * or stop a runtime itself. Stop is irreversible and idempotent. */
int cai_attachment_controller_stop(cai_attachment_controller *controller,
                                   cai_error *error);
/** Apply the first-client timer. now_ms must never decrease across ready,
 * attach and advance. A host schedules only deadline_ms; no idle polling. */
int cai_attachment_controller_advance(cai_attachment_controller *controller,
                                      uint64_t now_ms, cai_error *error);
int cai_attachment_controller_get_state(cai_attachment_controller *controller,
                                        cai_attachment_state *out,
                                        cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
