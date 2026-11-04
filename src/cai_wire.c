#include "cai_internal.h"
#include <cai/wire.h>
#include <string.h>

/** Four-byte length framing over the incremental CBOR decoder.
 * A record is accepted only after its declared bytes and CBOR value end. */
struct cai_wire_decoder {
  cai_wire_event_fn callback;
  void *context;
  cai_cbor_decoder *cbor;
  uint32_t length, remaining;
  unsigned int have;
  int failed, busy, finished;
};
/** Framed CBOR output borrowing sink and tracking the declared byte budget.
 * counter observes bytes written; it is not a sizing/materialization pass. */
struct cai_wire_encoder {
  cai_sink *sink, *counter;
  cai_cbor_encoder *cbor;
  uint32_t remaining;
  int failed, busy, active;
};
/** Deliver a borrowed framing/token observation to the decoder consumer. */
static int event(cai_wire_decoder *decoder, cai_wire_event_type type,
                 const cai_cbor_token *token, cai_error *error) {
  cai_wire_event value;
  value.type = type;
  value.length = decoder->length;
  value.token = token;
  return decoder->callback(decoder->context, &value, error);
}
static int token(void *context, const cai_cbor_token *value, cai_error *error) {
  return event(context, CAI_WIRE_TOKEN, value, error);
}
int cai_wire_decoder_open(cai_wire_event_fn callback, void *context,
                          cai_wire_decoder **out, cai_error *error) {
  cai_wire_decoder *decoder;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || callback == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire decoder requires callback and output");
  decoder = cai_alloc(NULL, sizeof(*decoder));
  if (decoder == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate wire decoder");
  memset(decoder, 0, sizeof(*decoder));
  decoder->callback = callback;
  decoder->context = context;
  {
    int rc = cai_cbor_decoder_open(token, decoder, &decoder->cbor, error);
    if (rc != CAI_OK) {
      cai_free_mem(NULL, decoder);
      return rc;
    }
  }
  *out = decoder;
  return CAI_OK;
}
int cai_wire_decoder_feed(cai_wire_decoder *decoder, const void *bytes,
                          size_t length, cai_error *error) {
  const unsigned char *input = bytes;
  int rc = CAI_OK;
  if (decoder == NULL || (bytes == NULL && length != 0U))
    return cai_set_error(error, CAI_ERR_INVALID, "Invalid wire decoder input");
  if (decoder->failed)
    return cai_set_error(error, decoder->failed, "Wire decoder has failed");
  if (decoder->busy || decoder->finished) {
    decoder->failed = CAI_ERR_INVALID;
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire decoder is busy or already finished");
  }
  decoder->busy = 1;
  while (length != 0U && rc == CAI_OK) {
    if (decoder->have < 4U) {
      decoder->length = (decoder->length << 8U) | *input++;
      --length;
      if (++decoder->have == 4U) {
        if (decoder->length == 0U || decoder->length > CAI_WIRE_MAX_RECORD)
          rc = cai_set_error(error, CAI_ERR_LIMIT,
                             "Wire record length must be 1..128 KiB");
        else {
          decoder->remaining = decoder->length;
          rc = event(decoder, CAI_WIRE_BEGIN, NULL, error);
        }
      }
    } else {
      size_t take = length < (size_t)decoder->remaining
                        ? length
                        : (size_t)decoder->remaining;
      rc = cai_cbor_decoder_feed(decoder->cbor, input, take, error);
      input += take;
      length -= take;
      decoder->remaining -= (uint32_t)take;
      if (rc == CAI_OK && decoder->remaining == 0U) {
        rc = cai_cbor_decoder_finish(decoder->cbor, error);
        if (rc == CAI_OK)
          rc = event(decoder, CAI_WIRE_END, NULL, error);
        if (rc == CAI_OK)
          rc = cai_cbor_decoder_reset(decoder->cbor, error);
        decoder->have = 0U;
        decoder->length = 0U;
      }
    }
  }
  decoder->busy = 0;
  if (rc != CAI_OK) {
    decoder->failed = rc;
    if (error != NULL && error->code == CAI_OK)
      cai_set_error(error, rc, "Wire callback failed");
  }
  return rc;
}
int cai_wire_decoder_finish(cai_wire_decoder *decoder, cai_error *error) {
  if (decoder == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "Wire decoder is required");
  if (decoder->failed)
    return cai_set_error(error, decoder->failed, "Wire decoder has failed");
  if (decoder->busy || decoder->finished) {
    decoder->failed = CAI_ERR_INVALID;
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire decoder is busy or already finished");
  }
  if (decoder->have != 0U) {
    decoder->failed = CAI_ERR_PROTOCOL;
    return cai_set_error(error, CAI_ERR_PROTOCOL, "Truncated wire record");
  }
  decoder->finished = 1;
  return CAI_OK;
}
void cai_wire_decoder_close(cai_wire_decoder *decoder) {
  if (decoder != NULL) {
    cai_cbor_decoder_close(decoder->cbor);
    cai_free_mem(NULL, decoder);
  }
}
static int count_write(void *context, const void *bytes, size_t length,
                       cai_error *error) {
  cai_wire_encoder *encoder = context;
  int rc;
  if (length > encoder->remaining)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Wire payload exceeds declared record length");
  rc = cai_sink_write(encoder->sink, bytes, length, error);
  if (rc == CAI_OK)
    encoder->remaining -= (uint32_t)length;
  return rc;
}
int cai_wire_encoder_open(cai_sink *sink, cai_wire_encoder **out,
                          cai_error *error) {
  cai_wire_encoder *encoder;
  cai_sink_callbacks callbacks;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || sink == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire encoder requires sink and output");
  encoder = cai_alloc(NULL, sizeof(*encoder));
  if (encoder == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate wire encoder");
  memset(encoder, 0, sizeof(*encoder));
  memset(&callbacks, 0, sizeof(callbacks));
  encoder->sink = sink;
  callbacks.write = count_write;
  callbacks.context = encoder;
  rc = cai_sink_from_callbacks(&callbacks, &encoder->counter, error);
  if (rc == CAI_OK)
    rc = cai_cbor_encoder_open(encoder->counter, &encoder->cbor, error);
  if (rc == CAI_OK)
    *out = encoder;
  else
    cai_wire_encoder_close(encoder);
  return rc;
}
static int encoder_enter(cai_wire_encoder *encoder, cai_error *error) {
  if (encoder == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "Wire encoder is required");
  if (encoder->failed)
    return cai_set_error(error, encoder->failed, "Wire encoder has failed");
  if (encoder->busy) {
    encoder->failed = CAI_ERR_INVALID;
    return cai_set_error(error, CAI_ERR_INVALID, "Wire encoder is busy");
  }
  encoder->busy = 1;
  return CAI_OK;
}
static int encoder_leave(cai_wire_encoder *encoder, int rc, cai_error *error) {
  encoder->busy = 0;
  if (rc != CAI_OK) {
    encoder->failed = rc;
    if (error != NULL && error->code == CAI_OK)
      cai_set_error(error, rc, "Wire output failed");
  }
  return rc;
}
int cai_wire_encoder_begin(cai_wire_encoder *encoder, uint32_t length,
                           cai_error *error) {
  unsigned char header[4];
  int rc = encoder_enter(encoder, error);
  if (rc != CAI_OK)
    return rc;
  if (encoder->active || length == 0U || length > CAI_WIRE_MAX_RECORD)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "Wire begin requires an idle encoder and 1..128 KiB");
  else {
    encoder->active = 1;
    encoder->remaining = length;
    header[0] = (unsigned char)(length >> 24U);
    header[1] = (unsigned char)(length >> 16U);
    header[2] = (unsigned char)(length >> 8U);
    header[3] = (unsigned char)length;
    rc = cai_sink_write(encoder->sink, header, sizeof(header), error);
  }
  return encoder_leave(encoder, rc, error);
}
int cai_wire_encoder_write(cai_wire_encoder *encoder,
                           const cai_cbor_token *token, cai_error *error) {
  int rc = encoder_enter(encoder, error);
  if (rc != CAI_OK)
    return rc;
  rc = !encoder->active
           ? cai_set_error(error, CAI_ERR_INVALID, "Wire record not begun")
           : cai_cbor_encoder_write(encoder->cbor, token, error);
  return encoder_leave(encoder, rc, error);
}
int cai_wire_encoder_end(cai_wire_encoder *encoder, cai_error *error) {
  int rc = encoder_enter(encoder, error);
  if (rc != CAI_OK)
    return rc;
  rc = !encoder->active
           ? cai_set_error(error, CAI_ERR_INVALID, "Wire record not begun")
           : cai_cbor_encoder_finish(encoder->cbor, error);
  if (rc == CAI_OK && encoder->remaining != 0U)
    rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                       "Wire payload is shorter than declared record length");
  if (rc == CAI_OK) {
    rc = cai_cbor_encoder_reset(encoder->cbor, error);
    encoder->active = 0;
  }
  return encoder_leave(encoder, rc, error);
}
void cai_wire_encoder_close(cai_wire_encoder *encoder) {
  if (encoder != NULL) {
    cai_cbor_encoder_close(encoder->cbor);
    cai_sink_close(encoder->counter);
    cai_free_mem(NULL, encoder);
  }
}
