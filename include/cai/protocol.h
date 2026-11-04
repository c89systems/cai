/** @file cai/protocol.h
 * Streaming semantic envelopes for workspace attachments. No application/UI.
 */
#ifndef CAI_PROTOCOL_H
#define CAI_PROTOCOL_H
#include <cai/wire.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CAI_WORKSPACE_SCHEMA "cai.workspace"
#define CAI_WORKSPACE_INSTANCE_BYTES 16U

typedef enum cai_protocol_kind {
  CAI_PROTOCOL_HELLO = 0,
  CAI_PROTOCOL_WELCOME,
  CAI_PROTOCOL_DETACH,
  CAI_PROTOCOL_TAKEOVER,
  CAI_PROTOCOL_ROLE,
  CAI_PROTOCOL_REQUEST,
  CAI_PROTOCOL_RESULT,
  CAI_PROTOCOL_STATUS,
  CAI_PROTOCOL_QUEUE_BEGIN,
  CAI_PROTOCOL_QUEUE_ITEM,
  CAI_PROTOCOL_QUEUE_END,
  CAI_PROTOCOL_QUEUE_CHANGE,
  CAI_PROTOCOL_EVENT,
  CAI_PROTOCOL_REPLAY_BEGIN,
  CAI_PROTOCOL_REPLAY_END,
  CAI_PROTOCOL_STREAM_BEGIN,
  CAI_PROTOCOL_STREAM_CHUNK,
  CAI_PROTOCOL_STREAM_END,
  CAI_PROTOCOL_SYNCHRONIZE,
  CAI_PROTOCOL_SHUTDOWN,
  CAI_PROTOCOL_SUBSCRIBE,
  CAI_PROTOCOL_UNSUBSCRIBE,
  CAI_PROTOCOL_SUBSCRIPTION_ERROR,
  CAI_PROTOCOL_PROBE,
  CAI_PROTOCOL_IDENTITY,
  CAI_PROTOCOL_CANCEL,
  CAI_PROTOCOL_HISTORY_GET,
  CAI_PROTOCOL_HISTORY_APPEND
} cai_protocol_kind;
/** Each record is a definite eight-pair map, with ascending integer keys:
 * 0 kind, 1 request, 2 conversation, 3 generation, 4 sequence, 5 stream,
 * 6 attachment, 7 payload. All seven metadata values are unsigned integers;
 * payload is one arbitrary CBOR item in the supported wire profile. Zero is
 * used for inapplicable metadata. Required keys, order and shape are exact;
 * unknown kinds/keys fail, without compatibility negotiation.
 *
 * Identity/role validation and the kind-specific payload schema belong to the
 * host. An untrusted header's attachment field does not authenticate its
 * sender. Use the identity bound by the completed connection handshake at
 * admission.
 */
typedef struct cai_protocol_header {
  cai_protocol_kind kind;
  uint64_t request, conversation, generation, sequence, stream, attachment;
} cai_protocol_header;
typedef enum cai_protocol_event_type {
  CAI_PROTOCOL_HEADER = 0,
  CAI_PROTOCOL_PAYLOAD,
  CAI_PROTOCOL_COMPLETE
} cai_protocol_event_type;
typedef struct cai_protocol_event {
  cai_protocol_event_type type;
  /** Borrowed until callback returns. The same metadata accompanies every
   * payload token and COMPLETE. Copy only what the consumer actually needs. */
  const cai_protocol_header *header;
  /** Borrowed PAYLOAD token, including BEGIN/DATA/END for nested values. */
  const cai_cbor_token *token;
} cai_protocol_event;
typedef int (*cai_protocol_event_fn)(void *context,
                                     const cai_protocol_event *event,
                                     cai_error *error);
typedef struct cai_protocol_decoder cai_protocol_decoder;

/** Heap-owned fixed parser state. Never gathers a record, map or payload.
 * HEADER/PAYLOAD are provisional. COMPLETE means the record is validated;
 * it is not a durable-admission receipt. The host checks authority and admits
 * a request only after this notification.
 * A bounded input command may be copied by its handler; replay/export/large
 * result handlers must consume successive chunks without gathering the value.
 * Handles have one owner. Callbacks must not reenter or close this decoder.
 */
int cai_protocol_decoder_open(cai_protocol_event_fn callback, void *context,
                              cai_protocol_decoder **out, cai_error *error);
/** Pass directly as cai_wire_peer_open/cai_wire_decoder_open's callback, with
 * the decoder as context. Records arrive incrementally from the wire decoder;
 * its successful END is required before COMPLETE. Protocol/consumer failures
 * are terminal. */
int cai_protocol_decoder_event(void *decoder, const cai_wire_event *event,
                               cai_error *error);
void cai_protocol_decoder_close(cai_protocol_decoder *decoder);

/** Emit framing and metadata immediately. payload_length is the encoded size
 * of one bounded payload item, derived from its known fields/chunk lengths,
 * never from a preliminary serialization. Write payload tokens directly with
 * cai_wire_peer_write, then call end. Large/unknown-length values use
 * successive STREAM_CHUNK records, with a stable stream identity, rather than
 * one huge record. The peer's bounded backlog/terminal-error contract still
 * applies.
 */
int cai_protocol_peer_begin(cai_wire_peer *peer,
                            const cai_protocol_header *header,
                            uint32_t payload_length, cai_error *error);
int cai_protocol_peer_end(cai_wire_peer *peer, cai_error *error);
#ifdef __cplusplus
}
#endif
#endif
