/** @file cai/wire.h
 * Incremental length-delimited CBOR records. No socket or application policy.
 */
#ifndef CAI_WIRE_H
#define CAI_WIRE_H
#include <cai/cbor.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CAI_WIRE_MAX_RECORD (128U * 1024U)
#define CAI_WIRE_CHUNK_SIZE (32U * 1024U)
typedef enum cai_wire_event_type {
  CAI_WIRE_BEGIN = 0,
  CAI_WIRE_TOKEN,
  CAI_WIRE_END
} cai_wire_event_type;
typedef struct cai_wire_event {
  cai_wire_event_type type;
  uint32_t length;
  /** Borrowed during TOKEN only. BEGIN/TOKEN are provisional until END. */
  const cai_cbor_token *token;
} cai_wire_event;
typedef int (*cai_wire_event_fn)(void *context, const cai_wire_event *event,
                                 cai_error *error);
typedef struct cai_wire_decoder cai_wire_decoder;
typedef struct cai_wire_encoder cai_wire_encoder;

/** Four-byte unsigned big-endian length, then exactly one CBOR item. Accepts
 * fragmented/coalesced records; callbacks run as bytes arrive, including DATA
 * before a complete record. No record buffer. finish accepts an empty stream
 * or any record boundary, and rejects a partial header/body. Failures are
 * terminal. Duplicate/schema keys are the typed consumer's responsibility.
 */
int cai_wire_decoder_open(cai_wire_event_fn callback, void *context,
                          cai_wire_decoder **out, cai_error *error);
int cai_wire_decoder_feed(cai_wire_decoder *decoder, const void *bytes,
                          size_t length, cai_error *error);
int cai_wire_decoder_finish(cai_wire_decoder *decoder, cai_error *error);
void cai_wire_decoder_close(cai_wire_decoder *decoder);

/** Borrow the sink. begin writes the known payload length immediately; write
 * forwards CBOR tokens immediately. end requires one complete item and exactly
 * the declared bytes. Derive lengths from known fields/chunks, never a sizing
 * serialization. Unknown-length producers send successive chunk records.
 * A sink error after partial delivery invalidates this stream; do not retry
 * the same token/record against the existing encoder. close never writes.
 */
int cai_wire_encoder_open(cai_sink *sink, cai_wire_encoder **out,
                          cai_error *error);
int cai_wire_encoder_begin(cai_wire_encoder *encoder, uint32_t length,
                           cai_error *error);
int cai_wire_encoder_write(cai_wire_encoder *encoder,
                           const cai_cbor_token *token, cai_error *error);
int cai_wire_encoder_end(cai_wire_encoder *encoder, cai_error *error);
void cai_wire_encoder_close(cai_wire_encoder *encoder);

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
int cai_wire_peer_begin(cai_wire_peer *peer, uint32_t length, cai_error *error);
int cai_wire_peer_write(cai_wire_peer *peer, const cai_cbor_token *token,
                        cai_error *error);
int cai_wire_peer_end(cai_wire_peer *peer, cai_error *error);
void cai_wire_peer_close(cai_wire_peer *peer);

#ifdef __cplusplus
}
#endif
#endif
