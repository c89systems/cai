#include <assert.h>
#include <cai/cbor.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void utf8_checks(void);

/* RFC 8949 Appendix A and binary64 boundaries. Tags, nonfinite values and
 * half/single precision are deliberately outside our profile. */
static const char *vectors[] = {
    "00",
    "01",
    "0a",
    "17",
    "1818",
    "1819",
    "1864",
    "1903e8",
    "1a000f4240",
    "1b000000e8d4a51000",
    "1bffffffffffffffff",
    "3bffffffffffffffff",
    "20",
    "29",
    "3863",
    "3903e7",
    "f4",
    "f5",
    "f6",
    "40",
    "4401020304",
    "60",
    "6161",
    "6449455446",
    "62225c",
    "62c3bc",
    "63e6b0b4",
    "64f0908591",
    "80",
    "83010203",
    "8301820203820405",
    "a0",
    "a201020304",
    "a26161016162820203",
    "826161a161626163",
    "8561616162616361646165",
    "5f42010243030405ff",
    "7f657374726561646d696e67ff",
    "9fff",
    "9f018202039f0405ffff",
    "9f01820203820405ff",
    "83018202039f0405ff",
    "83019f0203ff820405",
    "9f01820203820405ff",
    "bf61610161629f0203ffff",
    "826161bf61626163ff",
    "bf6346756ef563416d7421ff",
    /* Embedded NUL, empty chunks, and all header-size boundaries. */
    "63006100",
    "5f4040ff",
    "18ff",
    "190100",
    "19ffff",
    "1a00010000",
    "1affffffff",
    "1b0000000100000000",
    "fb0000000000000000",
    "fb8000000000000000",
    "fb3ff0000000000000",
    "fb3ff8000000000000",
    "fbbff8000000000000",
    "fb400921fb54442d18",
    "fb0000000000000001",
    "fb8000000000000001",
    "fb000fffffffffffff",
    "fb0010000000000000",
    "fb7fefffffffffffff",
    "fbffefffffffffffff",
    "a201fb3ff800000000000002fb8000000000000000",
};
typedef struct output {
  unsigned char bytes[512];
  size_t length, calls, fail_at;
  int failure;
} output;
static int collect(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  output *out = context;
  (void)error;
  ++out->calls;
  if (out->fail_at != 0U && out->calls == out->fail_at)
    return out->failure;
  assert(count <= sizeof(out->bytes) - out->length);
  memcpy(out->bytes + out->length, bytes, count);
  out->length += count;
  return CAI_OK;
}
static int encode(void *context, const cai_cbor_token *token,
                  cai_error *error) {
  return cai_cbor_encoder_write(context, token, error);
}
static size_t unhex(const char *text, unsigned char *bytes) {
  size_t i, n = strlen(text) / 2U;
  for (i = 0U; i < n; ++i) {
    const char *digits = "0123456789abcdef";
    const char *a = strchr(digits, text[i * 2U]);
    const char *b = strchr(digits, text[i * 2U + 1U]);
    assert(a != NULL && b != NULL);
    bytes[i] = (unsigned char)((a - digits) * 16 + (b - digits));
  }
  return n;
}
static void roundtrip(const char *text) {
  unsigned char bytes[512];
  size_t length = unhex(text, bytes), split;
  for (split = 0U; split <= length + 1U; ++split) {
    cai_sink_callbacks callbacks;
    cai_sink *sink = NULL;
    cai_cbor_encoder *encoder = NULL;
    cai_cbor_decoder *decoder = NULL;
    output out;
    size_t i;
    memset(&out, 0, sizeof(out));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = collect;
    callbacks.context = &out;
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
    assert(cai_cbor_decoder_open(encode, encoder, &decoder, NULL) == CAI_OK);
    if (split > length) {
      for (i = 0U; i < length; ++i)
        assert(cai_cbor_decoder_feed(decoder, bytes + i, 1U, NULL) == CAI_OK);
    } else {
      assert(cai_cbor_decoder_feed(decoder, bytes, split, NULL) == CAI_OK);
      assert(cai_cbor_decoder_feed(decoder, bytes + split, length - split,
                                   NULL) == CAI_OK);
    }
    assert(cai_cbor_decoder_finish(decoder, NULL) == CAI_OK);
    assert(cai_cbor_encoder_finish(encoder, NULL) == CAI_OK);
    assert(out.length == length && memcmp(out.bytes, bytes, length) == 0);
    cai_cbor_decoder_close(decoder);
    cai_cbor_encoder_close(encoder);
    cai_sink_close(sink);
  }
}
static int discard(void *context, const cai_cbor_token *token,
                   cai_error *error) {
  (void)context;
  (void)token;
  (void)error;
  return CAI_OK;
}
static void invalid(const char *text) {
  unsigned char bytes[512];
  size_t length = unhex(text, bytes), split;
  for (split = 0U; split <= length; ++split) {
    cai_cbor_decoder *decoder = NULL;
    cai_error error;
    int rc;
    cai_error_init(&error);
    assert(cai_cbor_decoder_open(discard, NULL, &decoder, &error) == CAI_OK);
    rc = cai_cbor_decoder_feed(decoder, bytes, split, &error);
    if (rc == CAI_OK)
      rc =
          cai_cbor_decoder_feed(decoder, bytes + split, length - split, &error);
    if (rc == CAI_OK)
      rc = cai_cbor_decoder_finish(decoder, &error);
    assert(rc == CAI_ERR_PROTOCOL || rc == CAI_ERR_LIMIT);
    assert(error.code == rc && error.message != NULL);
    /* A rejected stream cannot recover by feeding a new root item. */
    assert(cai_cbor_decoder_feed(decoder, "\0", 1U, NULL) == rc);
    cai_cbor_decoder_close(decoder);
    cai_error_cleanup(&error);
  }
}
static void truncations(void) {
  size_t i;
  for (i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    unsigned char bytes[512];
    size_t length = unhex(vectors[i], bytes), j;
    for (j = 0U; j < length; ++j) {
      cai_cbor_decoder *decoder = NULL;
      assert(cai_cbor_decoder_open(discard, NULL, &decoder, NULL) == CAI_OK);
      assert(cai_cbor_decoder_feed(decoder, bytes, j, NULL) == CAI_OK);
      assert(cai_cbor_decoder_finish(decoder, NULL) == CAI_ERR_PROTOCOL);
      cai_cbor_decoder_close(decoder);
    }
  }
}
static int write_token(cai_cbor_encoder *encoder, cai_cbor_type type,
                       uint64_t argument, int indefinite, const void *bytes,
                       size_t length) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.indefinite = indefinite;
  token.data = bytes;
  token.length = length;
  return cai_cbor_encoder_write(encoder, &token, NULL);
}
static void float_values(void) {
  static const uint64_t bits[] = {
      UINT64_C(0), UINT64_C(0x8000000000000000), UINT64_C(0x3ff8000000000000),
      UINT64_C(1), UINT64_C(0x8000000000000001), UINT64_C(0x7fefffffffffffff)};
  static const uint64_t nonfinite[] = {
      UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
      UINT64_C(0x7ff8000000000000), UINT64_C(0x7ff0000000000001)};
  size_t i;
  for (i = 0U; i < sizeof(bits) / sizeof(bits[0]); i++) {
    double original, value = 42.0;
    cai_cbor_token token;
    uint64_t result;
    memcpy(&original, &bits[i], sizeof(original));
    assert(cai_cbor_token_set_float64(&token, original, NULL) == CAI_OK);
    assert(token.type == CAI_CBOR_FLOAT64 && token.argument == bits[i]);
    assert(cai_cbor_token_get_float64(&token, &value, NULL) == CAI_OK);
    memcpy(&result, &value, sizeof(result));
    assert(result == bits[i]);
  }
  for (i = 0U; i < sizeof(nonfinite) / sizeof(nonfinite[0]); i++) {
    cai_cbor_token token, before;
    cai_sink_callbacks callbacks;
    cai_sink *sink;
    cai_cbor_encoder *encoder;
    output out;
    double bad, unchanged = 42.0;
    memset(&token, 0, sizeof(token));
    before = token;
    memcpy(&bad, &nonfinite[i], sizeof(bad));
    assert(cai_cbor_token_set_float64(&token, bad, NULL) == CAI_ERR_INVALID);
    assert(memcmp(&token, &before, sizeof(token)) == 0);
    token.type = CAI_CBOR_FLOAT64;
    token.argument = nonfinite[i];
    assert(cai_cbor_token_get_float64(&token, &unchanged, NULL) ==
               CAI_ERR_INVALID &&
           unchanged == 42.0);
    memset(&out, 0, sizeof(out));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = collect;
    callbacks.context = &out;
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
    assert(cai_cbor_encoder_write(encoder, &token, NULL) == CAI_ERR_PROTOCOL &&
           out.length == 0U);
    cai_cbor_encoder_close(encoder);
    cai_sink_close(sink);
  }
  {
    cai_cbor_token token;
    double value = 42.0;
    memset(&token, 0, sizeof(token));
    assert(cai_cbor_token_get_float64(&token, &value, NULL) ==
               CAI_ERR_INVALID &&
           value == 42.0);
    assert(cai_cbor_token_set_float64(NULL, 1.5, NULL) == CAI_ERR_INVALID);
    assert(cai_cbor_token_set_float64(&token, 1.5, NULL) == CAI_OK);
    assert(cai_cbor_token_get_float64(&token, &value, NULL) == CAI_OK &&
           value == 1.5);
    assert(cai_cbor_token_get_float64(&token, NULL, NULL) == CAI_ERR_INVALID);
    token.indefinite = 1;
    assert(cai_cbor_token_get_float64(&token, &value, NULL) == CAI_ERR_INVALID);
  }
}
static void encoder_failures(void) {
  unsigned int test;
  for (test = 0U; test < 14U; ++test) {
    cai_sink *sink = NULL;
    cai_sink_callbacks callbacks;
    cai_cbor_encoder *encoder = NULL;
    output out;
    memset(&out, 0, sizeof(out));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = collect;
    callbacks.context = &out;
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
    if (test == 0U)
      assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, "a", 1U) ==
             CAI_ERR_PROTOCOL);
    else if (test == 1U)
      assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_ARRAY, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    else if (test == 2U)
      assert(write_token(encoder, CAI_CBOR_BOOL, 2U, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    else if (test == 3U) {
      assert(write_token(encoder, CAI_CBOR_ARRAY, 0U, 0, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 4U) {
      assert(write_token(encoder, CAI_CBOR_MAP, 0U, 1, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_MAP, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 5U || test == 6U) {
      assert(write_token(encoder, CAI_CBOR_TEXT, 1U, 0, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0,
                         test == 5U ? "ab" : "\xff",
                         test == 5U ? 2U : 1U) == CAI_ERR_PROTOCOL);
    } else if (test == 7U) {
      assert(write_token(encoder, CAI_CBOR_TEXT, 1U, 0, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, "\xc2", 1U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_TEXT, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 8U) {
      assert(write_token(encoder, CAI_CBOR_TEXT, 0U, 1, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_TEXT, 0U, 1, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 9U) {
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 10U) {
      assert(write_token(encoder, CAI_CBOR_BYTES, 3U, 0, NULL, 0U) == CAI_OK);
      assert(cai_cbor_encoder_finish(encoder, NULL) == CAI_ERR_PROTOCOL);
    } else if (test == 11U) {
      out.fail_at = 1U;
      out.failure = CAI_ERR_CANCELLED;
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) ==
             CAI_ERR_CANCELLED);
      assert(write_token(encoder, CAI_CBOR_UINT, 0U, 0, NULL, 0U) ==
             CAI_ERR_CANCELLED);
    } else if (test == 13U) {
      cai_cbor_token token;
      assert(cai_cbor_token_set_float64(&token, 1.5, NULL) == CAI_OK);
      out.fail_at = 1U;
      out.failure = CAI_ERR_TRANSPORT;
      assert(cai_cbor_encoder_write(encoder, &token, NULL) ==
             CAI_ERR_TRANSPORT);
      assert(cai_cbor_encoder_write(encoder, &token, NULL) ==
             CAI_ERR_TRANSPORT);
    } else {
      assert(write_token(encoder, CAI_CBOR_BYTES, 3U, 0, NULL, 0U) == CAI_OK);
      out.fail_at = 2U;
      out.failure = CAI_ERR_TRANSPORT;
      assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, "abc", 3U) ==
             CAI_ERR_TRANSPORT);
    }
    cai_cbor_encoder_close(encoder);
    cai_sink_close(sink);
  }
}
typedef struct stream {
  cai_cbor_decoder *decoder;
  uint64_t produced, consumed;
  size_t largest, chunks;
  int ended;
} stream;
static int count_token(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  stream *value = context;
  size_t i;
  (void)error;
  if (token->type == CAI_CBOR_DATA) {
    const unsigned char *bytes = token->data;
    assert(!value->ended);
    for (i = 0U; i < token->length; ++i)
      assert(bytes[i] == (unsigned char)((value->consumed + i) & 255U));
    value->consumed += token->length;
    if (token->length > value->largest)
      value->largest = token->length;
    ++value->chunks;
  } else if (token->type == CAI_CBOR_END)
    value->ended = 1;
  return CAI_OK;
}
static int stream_write(void *context, const void *bytes, size_t count,
                        cai_error *error) {
  stream *value = context;
  return cai_cbor_decoder_feed(value->decoder, bytes, count, error);
}
static void *large_stream(void *unused) {
  const uint64_t total = UINT64_C(32) * 1024U * 1024U;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_cbor_encoder *encoder = NULL;
  unsigned char bytes[2048];
  stream value;
  size_t i;
  (void)unused;
  /* Numeric conversion also runs on the platform's default worker stack. */
  float_values();
  utf8_checks();
  memset(&value, 0, sizeof(value));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = stream_write;
  callbacks.context = &value;
  for (i = 0U; i < sizeof(bytes); ++i)
    bytes[i] = (unsigned char)(i & 255U);
  assert(cai_cbor_decoder_open(count_token, &value, &value.decoder, NULL) ==
         CAI_OK);
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
  assert(write_token(encoder, CAI_CBOR_BYTES, total, 0, NULL, 0U) == CAI_OK);
  while (value.produced < total) {
    assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, bytes, sizeof(bytes)) ==
           CAI_OK);
    value.produced += sizeof(bytes);
    /* This proves forwarding during production, without an end/sizing pass. */
    assert(value.consumed == value.produced);
    if (value.produced < total)
      assert(!value.ended);
  }
  assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_BYTES, 0, NULL, 0U) ==
         CAI_OK);
  assert(cai_cbor_encoder_finish(encoder, NULL) == CAI_OK);
  assert(cai_cbor_decoder_finish(value.decoder, NULL) == CAI_OK);
  assert(value.ended && value.largest == sizeof(bytes));
  assert(value.chunks == total / sizeof(bytes));
  cai_cbor_encoder_close(encoder);
  cai_cbor_decoder_close(value.decoder);
  cai_sink_close(sink);
  return NULL;
}
static int refuse(void *context, const cai_cbor_token *token,
                  cai_error *error) {
  (void)context;
  (void)token;
  (void)error;
  return CAI_ERR_CANCELLED;
}
typedef struct fragment_counter {
  size_t bytes, calls;
  int cancel;
} fragment_counter;
static int bounded_token(void *context, const cai_cbor_token *token,
                         cai_error *error) {
  fragment_counter *value = context;
  (void)error;
  if (token->type == CAI_CBOR_DATA) {
    assert(token->length <= CAI_CBOR_MAX_CHUNK_BYTES);
    value->bytes += token->length;
    ++value->calls;
    if (value->cancel)
      return CAI_ERR_CANCELLED;
  }
  return CAI_OK;
}
static int bounded_write(void *context, const void *bytes, size_t count,
                         cai_error *error) {
  fragment_counter *value = context;
  (void)bytes;
  (void)error;
  assert(count <= CAI_CBOR_MAX_CHUNK_BYTES);
  value->bytes += count;
  ++value->calls;
  return value->cancel ? CAI_ERR_CANCELLED : CAI_OK;
}
static void bounded_fragments(void) {
  size_t length = CAI_CBOR_MAX_CHUNK_BYTES * 2U;
  char *bytes = malloc(length);
  int cancel;
  assert(bytes != NULL);
  for (cancel = 0; cancel < 2; ++cancel) {
    cai_sink_callbacks callbacks;
    cai_sink *sink = NULL;
    cai_cbor_encoder *encoder = NULL;
    cai_cbor_decoder *decoder = NULL;
    fragment_counter value;
    memset(&value, 0, sizeof(value));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(bytes, 'x', length);
    bytes[CAI_CBOR_MAX_CHUNK_BYTES - 1U] = (char)0xc2;
    bytes[CAI_CBOR_MAX_CHUNK_BYTES] = (char)0xa2;
    if (cancel)
      bytes[length - 1U] = (char)0xff;
    value.cancel = cancel;
    assert(cai_cbor_decoder_open(bounded_token, &value, &decoder, NULL) ==
           CAI_OK);
    assert(cai_cbor_decoder_feed(decoder, "\x7a\0\x01\0\0", 5U, NULL) ==
           CAI_OK);
    /* Cancellation must interrupt validation before an invalid distant tail.
     * Successful delivery must split even one large caller-provided span. */
    assert(cai_cbor_decoder_feed(decoder, bytes, length, NULL) ==
           (cancel ? CAI_ERR_CANCELLED : CAI_OK));
    assert(value.bytes == (cancel ? length / 2U : length));
    assert(value.calls == (cancel ? 1U : 2U));
    if (!cancel)
      assert(cai_cbor_decoder_finish(decoder, NULL) == CAI_OK);
    cai_cbor_decoder_close(decoder);
    memset(&value, 0, sizeof(value));
    callbacks.write = bounded_write;
    callbacks.context = &value;
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
    assert(write_token(encoder, CAI_CBOR_TEXT, length, 0, NULL, 0U) == CAI_OK);
    value.bytes = value.calls = 0U;
    value.cancel = cancel;
    assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, bytes, length) ==
           (cancel ? CAI_ERR_CANCELLED : CAI_OK));
    assert(value.bytes == (cancel ? length / 2U : length));
    assert(value.calls == (cancel ? 1U : 2U));
    if (!cancel) {
      assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_TEXT, 0, NULL, 0U) ==
             CAI_OK);
      assert(cai_cbor_encoder_finish(encoder, NULL) == CAI_OK);
    }
    cai_cbor_encoder_close(encoder);
    cai_sink_close(sink);
  }
  {
    cai_sink_callbacks callbacks;
    cai_sink *sink = NULL;
    cai_cbor_encoder *encoder = NULL;
    fragment_counter value;
    memset(&value, 0, sizeof(value));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = bounded_write;
    callbacks.context = &value;
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
    assert(write_token(encoder, CAI_CBOR_BYTES, length / 2U, 0, NULL, 0U) ==
           CAI_OK);
    value.bytes = value.calls = 0U;
    assert(write_token(encoder, CAI_CBOR_DATA, 0U, 0, bytes, length) ==
           CAI_ERR_PROTOCOL);
    /* Known length overruns must fail before sending even a valid prefix. */
    assert(value.calls == 0U && value.bytes == 0U);
    cai_cbor_encoder_close(encoder);
    cai_sink_close(sink);
  }
  free(bytes);
}
static void reuse(void) {
  cai_cbor_decoder *decoder = NULL;
  cai_cbor_encoder *encoder = NULL;
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  output out;
  size_t i;
  memset(&out, 0, sizeof(out));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = collect;
  callbacks.context = &out;
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(sink, &encoder, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(encode, encoder, &decoder, NULL) == CAI_OK);
  for (i = 0U; i < 10000U; ++i) {
    assert(cai_cbor_decoder_feed(decoder, "\x81\x61\x61", 3U, NULL) == CAI_OK);
    assert(cai_cbor_decoder_finish(decoder, NULL) == CAI_OK);
    assert(cai_cbor_encoder_finish(encoder, NULL) == CAI_OK);
    assert(out.length == 3U && memcmp(out.bytes, "\x81\x61\x61", 3U) == 0);
    out.length = out.calls = 0U;
    assert(cai_cbor_decoder_reset(decoder, NULL) == CAI_OK);
    assert(cai_cbor_encoder_reset(encoder, NULL) == CAI_OK);
  }
  assert(cai_cbor_decoder_reset(decoder, NULL) == CAI_ERR_INVALID);
  assert(cai_cbor_encoder_reset(encoder, NULL) == CAI_ERR_INVALID);
  cai_cbor_encoder_close(encoder);
  cai_cbor_decoder_close(decoder);
  cai_sink_close(sink);
}
#include "cbor_utf8_test.inc"

int main(void) {
  static const char *bad[] = {"",
                              "18",
                              "19ff",
                              "1a000000",
                              "1b00000000000000",
                              "1c",
                              "1d",
                              "1e",
                              "1f",
                              "3f",
                              "ff",
                              "0000",
                              "c000",
                              "f7",
                              "f818",
                              "f90000",
                              "fa00000000",
                              "fb7ff0000000000000",
                              "fbfff0000000000000",
                              "fb7ff8000000000000",
                              "fb7ff0000000000001",
                              "fc",
                              "fd",
                              "fe",
                              "41",
                              "61",
                              "61ff",
                              "62c080",
                              "63eda080",
                              "64f4908080",
                              "64f0808080",
                              "62c261",
                              "61c2",
                              "62e080",
                              "7f61c261a2ff",
                              "5f6000ff",
                              "7f4000ff",
                              "5f5fff",
                              "7f7fff",
                              "bf00ff",
                              "a100",
                              "8100ff",
                              "8200",
                              "9f00",
                              "5f404000ff",
                              "5f4100",
                              "7f6161",
                              "bf8000ff00"};
  size_t i;
  unsigned char nesting[CAI_CBOR_MAX_DEPTH + 2U];
  cai_cbor_decoder *decoder = NULL;
  pthread_t worker;
  for (i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
    roundtrip(vectors[i]);
  invalid("5b0000000100000000");
  for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
    invalid(bad[i]);
  truncations();
  encoder_failures();
  reuse();
  bounded_fragments();
  memset(nesting, 0x81, sizeof(nesting));
  assert(cai_cbor_decoder_open(discard, NULL, &decoder, NULL) == CAI_OK);
  assert(cai_cbor_decoder_feed(decoder, nesting, CAI_CBOR_MAX_DEPTH, NULL) ==
         CAI_OK);
  assert(cai_cbor_decoder_feed(decoder, nesting, 1U, NULL) == CAI_ERR_LIMIT);
  cai_cbor_decoder_close(decoder);
  assert(cai_cbor_decoder_open(refuse, NULL, &decoder, NULL) == CAI_OK);
  assert(cai_cbor_decoder_feed(decoder, "\0", 1U, NULL) == CAI_ERR_CANCELLED);
  cai_cbor_decoder_close(decoder);
  assert(pthread_create(&worker, NULL, large_stream, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
