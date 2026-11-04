#include "cai_internal.h"
#include <cai/cbor.h>
#include <float.h>
#include <string.h>

typedef struct cbor_frame {
  uint64_t remaining;
  uint32_t scalar, minimum;
  cai_cbor_type type;
  unsigned int utf8, indefinite, odd;
} cbor_frame;
typedef struct cbor_state {
  cbor_frame frames[CAI_CBOR_MAX_DEPTH];
  size_t depth;
  int complete, failed, busy, finished;
} cbor_state;
struct cai_cbor_decoder {
  cbor_state state;
  cai_cbor_token_fn callback;
  void *context;
  unsigned char header[9];
  size_t have, need;
};
struct cai_cbor_encoder {
  cbor_state state;
  cai_sink *sink;
};
static int finite_bits(uint64_t bits) {
  return ((bits >> 52U) & UINT64_C(0x7ff)) != UINT64_C(0x7ff);
}
int cai_cbor_token_set_float64(cai_cbor_token *out, double value,
                               cai_error *error) {
  uint64_t bits = 0U;
  cai_cbor_token token;
  if (out == NULL || sizeof(value) != sizeof(bits) || DBL_MANT_DIG != 53 ||
      DBL_MAX_EXP != 1024)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Binary64 native double and token output required");
  memcpy(&bits, &value, sizeof(bits));
  if (!finite_bits(bits))
    return cai_set_error(error, CAI_ERR_INVALID, "Finite CBOR float required");
  memset(&token, 0, sizeof(token));
  token.type = CAI_CBOR_FLOAT64;
  token.argument = bits;
  *out = token;
  return CAI_OK;
}
int cai_cbor_token_get_float64(const cai_cbor_token *token, double *out,
                               cai_error *error) {
  if (token == NULL || out == NULL || token->type != CAI_CBOR_FLOAT64 ||
      token->indefinite || !finite_bits(token->argument) ||
      sizeof(*out) != sizeof(token->argument) || DBL_MANT_DIG != 53 ||
      DBL_MAX_EXP != 1024)
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Finite binary64 token and native double output required");
  memcpy(out, &token->argument, sizeof(*out));
  return CAI_OK;
}

static int fail(cbor_state *state, cai_error *error, int code,
                const char *message) {
  state->failed = code;
  return cai_set_error(error, code, message);
}
static int enter(cbor_state *state, cai_error *error) {
  if (state->failed)
    return cai_set_error(error, state->failed, "CBOR codec has failed");
  if (state->busy || state->finished)
    return fail(state, error, CAI_ERR_INVALID,
                "CBOR codec is busy or already finished");
  state->busy = 1;
  return CAI_OK;
}
static int leave(cbor_state *state, int rc, cai_error *error) {
  state->busy = 0;
  if (rc != CAI_OK) {
    state->failed = rc;
    if (error != NULL && error->code == CAI_OK)
      cai_set_error(error, rc, "CBOR callback failed");
  }
  return rc;
}
static cbor_frame *top(cbor_state *state) {
  return state->depth == 0U ? NULL : &state->frames[state->depth - 1U];
}
static void value_done(cbor_state *state) {
  cbor_frame *parent = top(state);
  if (parent == NULL)
    state->complete = 1;
  else if (parent->type == CAI_CBOR_ARRAY || parent->type == CAI_CBOR_MAP) {
    parent->odd ^= 1U;
    if (!parent->indefinite && (parent->type == CAI_CBOR_ARRAY || !parent->odd))
      --parent->remaining;
  }
}
static int begin(cbor_state *state, const cai_cbor_token *token,
                 cai_error *error) {
  cbor_frame *parent = top(state), *frame;
  if (state->complete)
    return fail(state, error, CAI_ERR_PROTOCOL, "CBOR has multiple root items");
  if (parent != NULL) {
    if (parent->type == CAI_CBOR_TEXT || parent->type == CAI_CBOR_BYTES) {
      if (!parent->indefinite || token->type != parent->type ||
          token->indefinite)
        return fail(state, error, CAI_ERR_PROTOCOL,
                    "CBOR string requires a definite chunk of its own type");
    } else if (!parent->indefinite && parent->remaining == 0U)
      return fail(state, error, CAI_ERR_PROTOCOL,
                  "CBOR container has too many items");
  }
  if (token->type >= CAI_CBOR_BYTES && token->type <= CAI_CBOR_MAP) {
    if (state->depth == CAI_CBOR_MAX_DEPTH)
      return fail(state, error, CAI_ERR_LIMIT, "CBOR nesting limit exceeded");
    frame = &state->frames[state->depth++];
    memset(frame, 0, sizeof(*frame));
    frame->type = token->type;
    frame->indefinite = token->indefinite != 0;
    frame->remaining = token->argument;
  } else {
    if (token->indefinite ||
        (token->type != CAI_CBOR_UINT && token->type != CAI_CBOR_NEGATIVE &&
         token->type != CAI_CBOR_BOOL && token->type != CAI_CBOR_NULL &&
         token->type != CAI_CBOR_FLOAT64) ||
        (token->type == CAI_CBOR_BOOL && token->argument > 1U) ||
        (token->type == CAI_CBOR_FLOAT64 && !finite_bits(token->argument)))
      return fail(state, error, CAI_ERR_PROTOCOL, "Unsupported CBOR token");
    value_done(state);
  }
  return CAI_OK;
}
static int utf8(cbor_frame *frame, const unsigned char *data, size_t count) {
  size_t i;
  for (i = 0U; i < count; ++i) {
    unsigned int byte = data[i];
    if (frame->utf8) {
      if ((byte & 0xc0U) != 0x80U)
        return 0;
      frame->scalar = (frame->scalar << 6U) | (byte & 0x3fU);
      if (--frame->utf8 == 0U &&
          (frame->scalar < frame->minimum || frame->scalar > 0x10ffffU ||
           (frame->scalar >= 0xd800U && frame->scalar <= 0xdfffU)))
        return 0;
    } else if (byte >= 0xc2U && byte <= 0xdfU) {
      frame->utf8 = 1U;
      frame->scalar = byte & 0x1fU;
      frame->minimum = 0x80U;
    } else if (byte >= 0xe0U && byte <= 0xefU) {
      frame->utf8 = 2U;
      frame->scalar = byte & 0x0fU;
      frame->minimum = 0x800U;
    } else if (byte >= 0xf0U && byte <= 0xf4U) {
      frame->utf8 = 3U;
      frame->scalar = byte & 0x07U;
      frame->minimum = 0x10000U;
    } else if (byte >= 0x80U)
      return 0;
  }
  return 1;
}
static int data_room(cbor_state *state, const void *bytes, size_t count,
                     cai_error *error) {
  cbor_frame *frame = top(state);
  if (frame == NULL ||
      (frame->type != CAI_CBOR_BYTES && frame->type != CAI_CBOR_TEXT) ||
      frame->indefinite || (count != 0U && bytes == NULL) ||
      (uint64_t)count > frame->remaining)
    return fail(state, error, CAI_ERR_PROTOCOL, "Invalid CBOR string data");
  return CAI_OK;
}
static int data(cbor_state *state, const void *bytes, size_t count,
                cai_error *error) {
  cbor_frame *frame;
  int rc = data_room(state, bytes, count, error);
  if (rc != CAI_OK)
    return rc;
  frame = top(state);
  if (frame->type == CAI_CBOR_TEXT && !utf8(frame, bytes, count))
    return fail(state, error, CAI_ERR_PROTOCOL, "Invalid CBOR UTF-8 text");
  frame->remaining -= (uint64_t)count;
  return CAI_OK;
}
static int end(cbor_state *state, uint64_t type, cai_error *error) {
  cbor_frame *frame = top(state);
  if (frame == NULL || type != (uint64_t)frame->type ||
      (!frame->indefinite && frame->remaining != 0U) || frame->utf8 != 0U ||
      (frame->type == CAI_CBOR_MAP && frame->odd))
    return fail(state, error, CAI_ERR_PROTOCOL,
                "Invalid or incomplete CBOR container end");
  --state->depth;
  value_done(state);
  return CAI_OK;
}
static int finish(cbor_state *state, cai_error *error) {
  if (!state->complete || state->depth != 0U)
    return fail(state, error, CAI_ERR_PROTOCOL, "Incomplete CBOR item");
  state->finished = 1;
  return CAI_OK;
}
static int reset(cbor_state *state, cai_error *error) {
  if (state->busy || !state->finished || state->failed)
    return fail(state, error, CAI_ERR_INVALID,
                "CBOR reset requires a successfully finished item");
  state->depth = 0U;
  state->complete = state->finished = 0;
  return CAI_OK;
}
static int emit(cai_cbor_decoder *decoder, cai_cbor_type type,
                uint64_t argument, int indefinite, const void *bytes,
                size_t length, cai_error *error) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.indefinite = indefinite;
  token.data = bytes;
  token.length = length;
  return decoder->callback(decoder->context, &token, error);
}
static int close_ready(cai_cbor_decoder *decoder, cai_error *error) {
  cbor_frame *frame;
  int rc;
  while ((frame = top(&decoder->state)) != NULL && !frame->indefinite &&
         frame->remaining == 0U) {
    cai_cbor_type type = frame->type;
    rc = end(&decoder->state, (uint64_t)type, error);
    if (rc == CAI_OK)
      rc = emit(decoder, CAI_CBOR_END, (uint64_t)type, 0, NULL, 0U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int header(cai_cbor_decoder *decoder, cai_error *error) {
  unsigned int major = decoder->header[0] >> 5U;
  unsigned int extra = decoder->header[0] & 31U;
  cai_cbor_token token;
  cbor_frame *frame;
  size_t i;
  int rc;
  memset(&token, 0, sizeof(token));
  if (decoder->header[0] == 0xffU) {
    frame = top(&decoder->state);
    if (frame == NULL || !frame->indefinite)
      return fail(&decoder->state, error, CAI_ERR_PROTOCOL,
                  "Unexpected CBOR break");
    token.type = CAI_CBOR_END;
    token.argument = (uint64_t)frame->type;
    rc = end(&decoder->state, token.argument, error);
  } else {
    if (extra >= 28U && extra != 31U)
      return fail(&decoder->state, error, CAI_ERR_PROTOCOL,
                  "Reserved CBOR additional information");
    if (major == 6U ||
        (major == 7U && (extra < 20U || extra > 22U) && extra != 27U) ||
        (extra == 31U && (major < 2U || major > 5U)))
      return fail(&decoder->state, error, CAI_ERR_PROTOCOL,
                  "Unsupported CBOR major type or simple value");
    token.type = major == 7U ? (extra == 27U   ? CAI_CBOR_FLOAT64
                                : extra == 22U ? CAI_CBOR_NULL
                                               : CAI_CBOR_BOOL)
                             : (cai_cbor_type)major;
    token.indefinite = extra == 31U;
    if (major == 7U && extra != 27U)
      token.argument = extra == 21U;
    else if (extra < 24U)
      token.argument = extra;
    else
      for (i = 1U; i < decoder->have; ++i)
        token.argument = (token.argument << 8U) | decoder->header[i];
    rc = begin(&decoder->state, &token, error);
  }
  if (rc == CAI_OK)
    rc = decoder->callback(decoder->context, &token, error);
  if (rc == CAI_OK)
    rc = close_ready(decoder, error);
  return rc;
}
int cai_cbor_decoder_open(cai_cbor_token_fn callback, void *context,
                          cai_cbor_decoder **out, cai_error *error) {
  cai_cbor_decoder *decoder;
  if (out != NULL)
    *out = NULL;
  if (callback == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "CBOR decoder requires callback and output");
  decoder = cai_alloc(NULL, sizeof(*decoder));
  if (decoder == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate CBOR decoder");
  memset(decoder, 0, sizeof(*decoder));
  decoder->callback = callback;
  decoder->context = context;
  *out = decoder;
  return CAI_OK;
}
int cai_cbor_decoder_feed(cai_cbor_decoder *decoder, const void *bytes,
                          size_t length, cai_error *error) {
  const unsigned char *input = bytes;
  int rc;
  if (decoder == NULL || (bytes == NULL && length != 0U))
    return cai_set_error(error, CAI_ERR_INVALID, "Invalid CBOR decoder input");
  rc = enter(&decoder->state, error);
  if (rc != CAI_OK)
    return rc;
  while (length != 0U && rc == CAI_OK) {
    cbor_frame *frame = top(&decoder->state);
    if (frame != NULL && !frame->indefinite &&
        (frame->type == CAI_CBOR_BYTES || frame->type == CAI_CBOR_TEXT)) {
      size_t take = frame->remaining < (uint64_t)length
                        ? (size_t)frame->remaining
                        : length;
      if (take > CAI_CBOR_MAX_CHUNK_BYTES)
        take = CAI_CBOR_MAX_CHUNK_BYTES;
      rc = data(&decoder->state, input, take, error);
      if (rc == CAI_OK)
        rc = emit(decoder, CAI_CBOR_DATA, 0U, 0, input, take, error);
      input += take;
      length -= take;
      if (rc == CAI_OK)
        rc = close_ready(decoder, error);
    } else {
      unsigned int extra;
      if (decoder->state.complete) {
        rc = fail(&decoder->state, error, CAI_ERR_PROTOCOL,
                  "CBOR has multiple root items");
        break;
      }
      decoder->header[decoder->have++] = *input++;
      --length;
      if (decoder->have == 1U) {
        extra = decoder->header[0] & 31U;
        decoder->need = extra >= 24U && extra <= 27U
                            ? 1U + ((size_t)1U << (extra - 24U))
                            : 1U;
      }
      if (decoder->have == decoder->need) {
        rc = header(decoder, error);
        decoder->have = decoder->need = 0U;
      }
    }
  }
  return leave(&decoder->state, rc, error);
}
int cai_cbor_decoder_finish(cai_cbor_decoder *decoder, cai_error *error) {
  int rc;
  if (decoder == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "CBOR decoder is required");
  rc = enter(&decoder->state, error);
  if (rc != CAI_OK)
    return rc;
  rc = decoder->have != 0U ? fail(&decoder->state, error, CAI_ERR_PROTOCOL,
                                  "Incomplete CBOR header")
                           : finish(&decoder->state, error);
  return leave(&decoder->state, rc, error);
}
void cai_cbor_decoder_close(cai_cbor_decoder *decoder) {
  cai_free_mem(NULL, decoder);
}
int cai_cbor_decoder_reset(cai_cbor_decoder *decoder, cai_error *error) {
  return decoder != NULL ? reset(&decoder->state, error)
                         : cai_set_error(error, CAI_ERR_INVALID,
                                         "CBOR decoder is required");
}
int cai_cbor_encoder_open(cai_sink *sink, cai_cbor_encoder **out,
                          cai_error *error) {
  cai_cbor_encoder *encoder;
  if (out != NULL)
    *out = NULL;
  if (sink == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "CBOR encoder requires sink and output");
  encoder = cai_alloc(NULL, sizeof(*encoder));
  if (encoder == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate CBOR encoder");
  memset(encoder, 0, sizeof(*encoder));
  encoder->sink = sink;
  *out = encoder;
  return CAI_OK;
}
int cai_cbor_encoder_write(cai_cbor_encoder *encoder,
                           const cai_cbor_token *token, cai_error *error) {
  unsigned char bytes[9];
  size_t length = 1U, size, i;
  uint64_t argument;
  int rc, indefinite;
  if (encoder == NULL || token == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "Invalid CBOR encoder input");
  rc = enter(&encoder->state, error);
  if (rc != CAI_OK)
    return rc;
  if (token->type == CAI_CBOR_DATA) {
    size_t offset = 0U;
    /* Reject a known length overrun before emitting any part of this call.
     * UTF-8 validation and sink delivery then progress in bounded pieces. */
    rc = data_room(&encoder->state, token->data, token->length, error);
    while (rc == CAI_OK && offset < token->length) {
      size_t take = token->length - offset;
      const unsigned char *input = token->data;
      if (take > CAI_CBOR_MAX_CHUNK_BYTES)
        take = CAI_CBOR_MAX_CHUNK_BYTES;
      rc = data(&encoder->state, input + offset, take, error);
      if (rc == CAI_OK)
        rc = cai_sink_write(encoder->sink, input + offset, take, error);
      offset += take;
    }
  } else if (token->type == CAI_CBOR_END) {
    cbor_frame *frame = top(&encoder->state);
    indefinite = frame != NULL && frame->indefinite;
    rc = end(&encoder->state, token->argument, error);
    if (rc == CAI_OK && indefinite) {
      bytes[0] = 0xffU;
      rc = cai_sink_write(encoder->sink, bytes, 1U, error);
    }
  } else {
    rc = begin(&encoder->state, token, error);
    if (rc == CAI_OK) {
      argument = token->argument;
      if (token->type == CAI_CBOR_FLOAT64) {
        bytes[0] = 0xfbU;
        for (i = 0U; i < 8U; ++i)
          bytes[8U - i] = (unsigned char)(argument >> (8U * i));
        length = 9U;
      } else if (token->type == CAI_CBOR_BOOL || token->type == CAI_CBOR_NULL)
        bytes[0] = token->type == CAI_CBOR_NULL
                       ? 0xf6U
                       : (unsigned char)(0xf4U + argument);
      else if (token->indefinite)
        bytes[0] = (unsigned char)(((unsigned int)token->type << 5U) | 31U);
      else if (argument < 24U)
        bytes[0] =
            (unsigned char)(((unsigned int)token->type << 5U) | argument);
      else {
        size = argument <= UINT8_MAX    ? 1U
               : argument <= UINT16_MAX ? 2U
               : argument <= UINT32_MAX ? 4U
                                        : 8U;
        bytes[0] = (unsigned char)(((unsigned int)token->type << 5U) |
                                   (size == 1U   ? 24U
                                    : size == 2U ? 25U
                                    : size == 4U ? 26U
                                                 : 27U));
        for (i = 0U; i < size; ++i)
          bytes[size - i] = (unsigned char)(argument >> (8U * i));
        length += size;
      }
      rc = cai_sink_write(encoder->sink, bytes, length, error);
    }
  }
  return leave(&encoder->state, rc, error);
}
int cai_cbor_encoder_finish(cai_cbor_encoder *encoder, cai_error *error) {
  int rc;
  if (encoder == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "CBOR encoder is required");
  rc = enter(&encoder->state, error);
  return rc == CAI_OK
             ? leave(&encoder->state, finish(&encoder->state, error), error)
             : rc;
}
void cai_cbor_encoder_close(cai_cbor_encoder *encoder) {
  cai_free_mem(NULL, encoder);
}
int cai_cbor_encoder_reset(cai_cbor_encoder *encoder, cai_error *error) {
  return encoder != NULL ? reset(&encoder->state, error)
                         : cai_set_error(error, CAI_ERR_INVALID,
                                         "CBOR encoder is required");
}
