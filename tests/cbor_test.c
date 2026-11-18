/** Native public API integration: borrowed fragments, RFC vectors, malformed
 * input, bounded streaming and default-stack workers. CAI profile exclusions
 * are tested in protocol_test.c, not imposed on libcbor. */
#include "cbor_vectors.inc"
#include <assert.h>
#include <c89cbor/cbor.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  c89cbor_encoder encoder;
  c89cbor_decoder decoder;
  unsigned char output[512];
  size_t used, bytes, records, largest;
  int roundtrip, sequence;
} fixture;
static fixture *open_fixture(void) {
  fixture *f = calloc(1U, sizeof(*f));
  assert(f != NULL);
  assert(c89cbor_encoder_init(NULL, &f->encoder, NULL, 0U, 0U) == C89CBOR_OK);
  assert(c89cbor_decoder_init(NULL, &f->decoder, NULL, 0U, 0U) == C89CBOR_OK);
  return f;
}
static void close_fixture(fixture *f) {
  assert(c89cbor_encoder_cleanup(&f->encoder) == C89CBOR_OK);
  assert(c89cbor_decoder_cleanup(&f->decoder) == C89CBOR_OK);
  free(f);
}
static size_t unhex(const char *text, unsigned char *out) {
  const char *digits = "0123456789abcdef";
  size_t i, n = strlen(text) / 2U;
  for (i = 0U; i < n; ++i) {
    const char *a = strchr(digits, text[i * 2U]);
    const char *b = strchr(digits, text[i * 2U + 1U]);
    assert(a != NULL && b != NULL);
    out[i] = (unsigned char)((a - digits) * 16 + b - digits);
  }
  return n;
}
/** Drain validated borrows before submitting another token. */
static int collect(fixture *f, const c89cbor_token *token) {
  int rc = c89cbor_encoder_put(&f->encoder, token);
  while (rc == C89CBOR_OK) {
    const void *bytes;
    size_t length;
    rc = c89cbor_encoder_peek(&f->encoder, &bytes, &length);
    if (rc != C89CBOR_OUTPUT)
      return rc;
    assert(length <= sizeof(f->output) - f->used);
    memcpy(f->output + f->used, bytes, length);
    f->used += length;
    rc = c89cbor_encoder_advance(&f->encoder, length);
    if (rc == C89CBOR_OUTPUT)
      rc = C89CBOR_OK;
  }
  return rc;
}
static int feed(fixture *f, const void *bytes, size_t length) {
  const unsigned char *at = bytes;
  for (;;) {
    c89cbor_token token;
    size_t consumed;
    int rc = c89cbor_decoder_next(&f->decoder, at, length, &consumed, &token);
    if (consumed != 0U)
      at += consumed;
    length -= consumed;
    if (rc == C89CBOR_INPUT || rc == C89CBOR_DONE)
      return length == 0U ? C89CBOR_OK : C89CBOR_MALFORMED;
    if (rc != C89CBOR_TOKEN)
      return rc;
    if (token.type == C89CBOR_DATA) {
      size_t i;
      const unsigned char *p = token.data;
      assert(token.data == at - consumed && token.length == consumed);
      assert(token.length <= C89CBOR_CHUNK);
      if (f->sequence)
        for (i = 0U; i < token.length; ++i)
          assert(p[i] == (unsigned char)((f->bytes + i) & 255U));
      f->bytes += token.length;
      if (token.length > f->largest)
        f->largest = token.length;
    } else if (token.type == C89CBOR_END)
      ++f->records;
    if (f->roundtrip) {
      rc = collect(f, &token);
      if (rc != C89CBOR_OK)
        return rc;
    }
  }
}
static void roundtrip(const char *vector) {
  unsigned char input[512];
  size_t length = unhex(vector, input), split;
  for (split = 0U; split <= length; ++split) {
    fixture *f = open_fixture();
    f->roundtrip = 1;
    assert(feed(f, input, split) == C89CBOR_OK);
    assert(feed(f, input + split, length - split) == C89CBOR_OK);
    assert(c89cbor_decoder_finish(&f->decoder) == C89CBOR_OK);
    assert(c89cbor_encoder_finish(&f->encoder) == C89CBOR_OK);
    assert(f->used == length && memcmp(input, f->output, length) == 0);
    assert(c89cbor_decoder_reset(&f->decoder) == C89CBOR_OK);
    assert(c89cbor_encoder_reset(&f->encoder) == C89CBOR_OK);
    close_fixture(f);
  }
  for (split = 0U; split < length; ++split) {
    fixture *f = open_fixture();
    assert(feed(f, input, split) == C89CBOR_OK);
    assert(c89cbor_decoder_finish(&f->decoder) == C89CBOR_MALFORMED);
    close_fixture(f);
  }
}
static void malformed(const char *vector) {
  unsigned char input[512];
  size_t length = unhex(vector, input), split;
  for (split = 0U; split <= length; ++split) {
    fixture *f = open_fixture();
    int rc = feed(f, input, split);
    if (rc == C89CBOR_OK)
      rc = feed(f, input + split, length - split);
    if (rc == C89CBOR_OK)
      rc = c89cbor_decoder_finish(&f->decoder);
    assert(rc == C89CBOR_MALFORMED);
    assert(c89cbor_decoder_reset(&f->decoder) < 0);
    close_fixture(f);
  }
}
static void reuse(void) {
  fixture *f = open_fixture();
  size_t i;
  f->roundtrip = 1;
  for (i = 0U; i < 10000U; ++i) {
    assert(feed(f, "\x81\x61\x61", 3U) == C89CBOR_OK);
    assert(c89cbor_decoder_finish(&f->decoder) == C89CBOR_OK);
    assert(c89cbor_encoder_finish(&f->encoder) == C89CBOR_OK);
    assert(f->used == 3U && memcmp(f->output, "\x81\x61\x61", 3U) == 0);
    assert(c89cbor_decoder_reset(&f->decoder) == C89CBOR_OK);
    assert(c89cbor_encoder_reset(&f->encoder) == C89CBOR_OK);
    f->used = 0U;
  }
  close_fixture(f);
}
static void streaming(void) {
  fixture *f = open_fixture();
  c89cbor_token token;
  unsigned char data[C89CBOR_CHUNK];
  const size_t total = 32U * 1024U * 1024U;
  size_t produced = 0U, i;
  f->sequence = 1;
  memset(&token, 0, sizeof(token));
  token.type = C89CBOR_BYTES;
  token.argument = total;
  for (i = 0U; i < sizeof(data); ++i)
    data[i] = (unsigned char)(i & 255U);
  for (;;) {
    assert(c89cbor_encoder_put(&f->encoder, &token) == C89CBOR_OK);
    for (;;) {
      const void *bytes;
      size_t length;
      int rc = c89cbor_encoder_peek(&f->encoder, &bytes, &length);
      if (rc == C89CBOR_OK)
        break;
      assert(rc == C89CBOR_OUTPUT && length <= C89CBOR_CHUNK);
      if (token.type == C89CBOR_DATA)
        assert(bytes == data); /* Encoder DATA is a borrow, not a copy. */
      assert(feed(f, bytes, length) == C89CBOR_OK);
      assert(c89cbor_encoder_advance(&f->encoder, length) >= 0);
    }
    if (token.type == C89CBOR_END)
      break;
    if (token.type == C89CBOR_DATA) {
      produced += sizeof(data);
      assert(f->bytes == produced); /* Delivery before finalization. */
    }
    memset(&token, 0, sizeof(token));
    token.type = produced == total ? C89CBOR_END : C89CBOR_DATA;
    if (token.type == C89CBOR_DATA) {
      token.data = data;
      token.length = sizeof(data);
    } else
      token.argument = C89CBOR_BYTES;
  }
  assert(f->bytes == total && f->largest == C89CBOR_CHUNK && f->records == 1U);
  assert(c89cbor_encoder_finish(&f->encoder) == C89CBOR_OK);
  assert(c89cbor_decoder_finish(&f->decoder) == C89CBOR_OK);
  close_fixture(f);
}
#include "cbor_utf8_test.inc"
static void limits(void) {
  unsigned char input[C89CBOR_DEPTH + 1U];
  fixture *f = open_fixture();
  c89cbor_token token;
  size_t i;
  memset(input, 0x81, sizeof(input));
  input[C89CBOR_DEPTH] = 0U;
  assert(feed(f, input, sizeof(input)) == C89CBOR_OK);
  assert(c89cbor_decoder_finish(&f->decoder) == C89CBOR_OK);
  assert(c89cbor_decoder_reset(&f->decoder) == C89CBOR_OK);
  input[C89CBOR_DEPTH] = 0x81U;
  assert(feed(f, input, sizeof(input)) == C89CBOR_LIMIT);
  memset(&token, 0, sizeof(token));
  token.type = C89CBOR_ARRAY;
  token.argument = 1U;
  for (i = 0U; i < C89CBOR_DEPTH; ++i)
    assert(collect(f, &token) == C89CBOR_OK);
  assert(collect(f, &token) == C89CBOR_LIMIT);
  close_fixture(f);
}
static void *worker(void *context) {
  static const char *extra[] = {"c000",
                                "f7",
                                "f820",
                                "f90000",
                                "fa00000000",
                                "fb7ff0000000000000",
                                "fb7ff8000000000000"};
  static const char *bad[] = {"",     "18",           "1c",       "ff",
                              "61ff", "62c080",       "63eda080", "64f4908080",
                              "61c2", "7f61c261a2ff", "5f6000ff", "bf00ff",
                              "a100", "8200",         "9f00"};
  size_t i;
  (void)context;
  for (i = 0U; i < sizeof(vectors) / sizeof(vectors[0]); ++i)
    roundtrip(vectors[i]);
  for (i = 0U; i < sizeof(extra) / sizeof(extra[0]); ++i)
    roundtrip(extra[i]);
  for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
    malformed(bad[i]);
  reuse();
  limits();
  utf8_checks();
  streaming();
  return NULL;
}
int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
