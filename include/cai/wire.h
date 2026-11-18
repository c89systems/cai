/** @file cai/wire.h
 * Nonblocking socket peers using native libcbor LENGTH32 records and tokens.
 */
#ifndef CAI_WIRE_H
#define CAI_WIRE_H
#include <c89cbor/io.h>
#include <cai/cai.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CAI_WIRE_MAX_RECORD (128U * 1024U)
#define CAI_WIRE_CHUNK_SIZE (32U * 1024U)
/** Receives libcbor's native framing events without token conversion.
 * Tokens remain provisional until RECORD_END. Schema and supported-value
 * policies belong to the application consumer, not to this transport. */
typedef int (*cai_wire_event_fn)(void *context, const c89cbor_event *event,
                                 cai_error *error);
typedef struct cai_wire_peer cai_wire_peer;
/** Nonblocking Unix-stream adapter, owned by the caller's event-loop thread.
 * Transfers descriptor ownership on success only. It must already be a
 * connected Unix stream socket. Applies O_NONBLOCK and FD_CLOEXEC. Queueing
 * output never waits for the reader; a bounded copied backlog defaults to
 * 1 MiB (0), maximum 16 MiB. On overflow/I/O/protocol failure discard this
 * peer and reconcile on reconnect; a partially written record is not retryable.
 * No socket worker is created. Callbacks may enqueue replies but must not
 * reenter read/flush or close the peer. Core application work must continue
 * independently if a subscriber is disconnected for backpressure.
 */
int cai_wire_peer_open(int descriptor, size_t backlog_limit,
                       cai_wire_event_fn callback, void *context,
                       cai_wire_peer **out, cai_error *error);
int cai_wire_peer_fd(const cai_wire_peer *peer);
/** Bytes awaiting POLLOUT. Never poll for write readiness when this is zero. */
size_t cai_wire_peer_pending(const cai_wire_peer *peer);
/** Perform bounded ready I/O. budget == 0 uses 64 KiB. A clean inbound EOF
 * sets *eof to 1; a partial record at EOF fails. EAGAIN is successful progress
 * exhaustion, not an error. Bytes are decoded as read; no full-record buffer.
 * flush suppresses SIGPIPE on supported Unix targets. */
int cai_wire_peer_read(cai_wire_peer *peer, size_t budget, int *eof,
                       cai_error *error);
int cai_wire_peer_flush(cai_wire_peer *peer, size_t budget, cai_error *error);
/** Queue the native LENGTH32 prefix using a payload size derived from known
 * fields/chunks, never a preliminary serialization. write consumes native
 * tokens and drains their borrowed spans before returning. end requires one
 * complete item and exactly the declared byte count. Errors are terminal;
 * close never finishes a partial record. Unknown-size values use successive
 * application chunk records, not a full-value buffer or backpatching. */
int cai_wire_peer_begin(cai_wire_peer *peer, uint32_t length, cai_error *error);
int cai_wire_peer_write(cai_wire_peer *peer, const c89cbor_token *token,
                        cai_error *error);
int cai_wire_peer_end(cai_wire_peer *peer, cai_error *error);
void cai_wire_peer_close(cai_wire_peer *peer);

#ifdef __cplusplus
}
#endif
#endif
