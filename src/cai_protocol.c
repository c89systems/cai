#include "cai_internal.h"
#include <cai/protocol.h>
#include <string.h>

/** Envelope parsing phases. Payload nesting is tracked, never collected. */
enum decode_stage { IDLE = 0, ROOT, KEY, VALUE, PAYLOAD, ROOT_END, RECORD_END };
/** One strict workspace-envelope decoder over framed CBOR tokens.
 * Header storage is fixed; the application consumes borrowed payload tokens. */
struct cai_protocol_decoder {
  cai_protocol_event_fn callback;
  void *context;
  cai_protocol_header header;
  enum decode_stage stage;
  unsigned int field, depth;
  int failed, busy;
};
static int notify(cai_protocol_decoder *decoder, cai_protocol_event_type type,
                  const cai_cbor_token *token, cai_error *error) {
  cai_protocol_event event;
  event.type = type;
  event.header = &decoder->header;
  event.token = token;
  return decoder->callback(decoder->context, &event, error);
}
static int bad(cai_error *error) {
  return cai_set_error(error, CAI_ERR_PROTOCOL,
                       "invalid workspace protocol envelope");
}
/** Validate envelope field order and forward payload tokens without a DOM. */
static int decode_token(cai_protocol_decoder *decoder,
                        const cai_cbor_token *token, cai_error *error) {
  int rc;
  if (token == NULL)
    return bad(error);
  switch (decoder->stage) {
  case ROOT:
    if (token->type != CAI_CBOR_MAP || token->indefinite ||
        token->argument != 8U)
      return bad(error);
    decoder->stage = KEY;
    return CAI_OK;
  case KEY:
    if (token->type != CAI_CBOR_UINT || token->argument != decoder->field)
      return bad(error);
    if (decoder->field == 7U) {
      decoder->stage = PAYLOAD;
      return notify(decoder, CAI_PROTOCOL_HEADER, NULL, error);
    }
    decoder->stage = VALUE;
    return CAI_OK;
  case VALUE:
    if (token->type != CAI_CBOR_UINT)
      return bad(error);
    switch (decoder->field) {
    case 0:
      if (token->argument > CAI_PROTOCOL_HISTORY_APPEND)
        return bad(error);
      decoder->header.kind = (cai_protocol_kind)token->argument;
      break;
    case 1:
      decoder->header.request = token->argument;
      break;
    case 2:
      decoder->header.conversation = token->argument;
      break;
    case 3:
      decoder->header.generation = token->argument;
      break;
    case 4:
      decoder->header.sequence = token->argument;
      break;
    case 5:
      decoder->header.stream = token->argument;
      break;
    case 6:
      decoder->header.attachment = token->argument;
      break;
    default:
      return bad(error);
    }
    ++decoder->field;
    decoder->stage = KEY;
    return CAI_OK;
  case PAYLOAD:
    if (token->type == CAI_CBOR_BYTES || token->type == CAI_CBOR_TEXT ||
        token->type == CAI_CBOR_ARRAY || token->type == CAI_CBOR_MAP) {
      if (decoder->depth == CAI_CBOR_MAX_DEPTH)
        return bad(error);
      ++decoder->depth;
    } else if (token->type == CAI_CBOR_END) {
      if (decoder->depth == 0U)
        return bad(error);
      --decoder->depth;
    } else if (token->type == CAI_CBOR_DATA && decoder->depth == 0U)
      return bad(error);
    rc = notify(decoder, CAI_PROTOCOL_PAYLOAD, token, error);
    if (decoder->depth == 0U)
      decoder->stage = ROOT_END;
    return rc;
  case ROOT_END:
    if (token->type != CAI_CBOR_END || token->argument != CAI_CBOR_MAP)
      return bad(error);
    decoder->stage = RECORD_END;
    return CAI_OK;
  default:
    return bad(error);
  }
}
int cai_protocol_decoder_open(cai_protocol_event_fn callback, void *context,
                              cai_protocol_decoder **out, cai_error *error) {
  cai_protocol_decoder *decoder;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || callback == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "protocol decoder requires callback and output");
  decoder = cai_alloc(NULL, sizeof(*decoder));
  if (decoder == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate protocol decoder");
  memset(decoder, 0, sizeof(*decoder));
  decoder->callback = callback;
  decoder->context = context;
  *out = decoder;
  return CAI_OK;
}
int cai_protocol_decoder_event(void *context, const cai_wire_event *event,
                               cai_error *error) {
  cai_protocol_decoder *decoder = context;
  int rc;
  if (decoder == NULL || event == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "protocol decoder and wire event are required");
  if (decoder->failed)
    return cai_set_error(error, decoder->failed, "protocol decoder has failed");
  if (decoder->busy) {
    decoder->failed = CAI_ERR_INVALID;
    return cai_set_error(error, CAI_ERR_INVALID, "protocol decoder is busy");
  }
  decoder->busy = 1;
  switch (event->type) {
  case CAI_WIRE_BEGIN:
    rc = decoder->stage != IDLE ? bad(error) : CAI_OK;
    if (rc == CAI_OK) {
      memset(&decoder->header, 0, sizeof(decoder->header));
      decoder->field = decoder->depth = 0U;
      decoder->stage = ROOT;
    }
    break;
  case CAI_WIRE_TOKEN:
    rc = decode_token(decoder, event->token, error);
    break;
  case CAI_WIRE_END:
    rc = decoder->stage != RECORD_END
             ? bad(error)
             : notify(decoder, CAI_PROTOCOL_COMPLETE, NULL, error);
    if (rc == CAI_OK)
      decoder->stage = IDLE;
    break;
  default:
    rc = bad(error);
    break;
  }
  decoder->busy = 0;
  if (decoder->failed != 0)
    rc = decoder->failed;
  if (rc != CAI_OK) {
    decoder->failed = rc;
    if (error != NULL && error->code == CAI_OK)
      cai_set_error(error, rc, "protocol consumer failed");
  }
  return rc;
}
void cai_protocol_decoder_close(cai_protocol_decoder *decoder) {
  cai_free_mem(NULL, decoder);
}
static uint32_t uint_length(uint64_t value) {
  return value < 24U            ? 1U
         : value <= 0xffU       ? 2U
         : value <= 0xffffU     ? 3U
         : value <= 0xffffffffU ? 5U
                                : 9U;
}
static int write_token(cai_wire_peer *peer, cai_cbor_type type, uint64_t value,
                       cai_error *error) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = value;
  return cai_wire_peer_write(peer, &token, error);
}
int cai_protocol_peer_begin(cai_wire_peer *peer,
                            const cai_protocol_header *header,
                            uint32_t payload_length, cai_error *error) {
  uint64_t values[7];
  uint32_t length = 9U; /* eight one-byte keys and the map header */
  unsigned int i;
  int rc;
  if (peer == NULL || header == NULL || header->kind < CAI_PROTOCOL_HELLO ||
      header->kind > CAI_PROTOCOL_HISTORY_APPEND || payload_length == 0U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "protocol writer requires a header and payload size");
  values[0] = (uint64_t)header->kind;
  values[1] = header->request;
  values[2] = header->conversation;
  values[3] = header->generation;
  values[4] = header->sequence;
  values[5] = header->stream;
  values[6] = header->attachment;
  for (i = 0U; i < 7U; ++i)
    length += uint_length(values[i]);
  if (payload_length > CAI_WIRE_MAX_RECORD - length)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "protocol payload exceeds one wire record");
  rc = cai_wire_peer_begin(peer, length + payload_length, error);
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_MAP, 8U, error);
  for (i = 0U; rc == CAI_OK && i < 7U; ++i) {
    rc = write_token(peer, CAI_CBOR_UINT, i, error);
    if (rc == CAI_OK)
      rc = write_token(peer, CAI_CBOR_UINT, values[i], error);
  }
  if (rc == CAI_OK)
    rc = write_token(peer, CAI_CBOR_UINT, 7U, error);
  return rc;
}
int cai_protocol_peer_end(cai_wire_peer *peer, cai_error *error) {
  int rc = write_token(peer, CAI_CBOR_END, CAI_CBOR_MAP, error);
  return rc == CAI_OK ? cai_wire_peer_end(peer, error) : rc;
}
