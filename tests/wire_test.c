#include <assert.h>
#include <cai/wire.h>
#include <string.h>

typedef struct fixture {
  cai_wire_decoder *decoder;
  size_t begun, ended, tokens, bytes, produced, fail_at, calls;
  int fail_kind;
} fixture;
static int observe(void *context, const cai_wire_event *event,
                   cai_error *error) {
  fixture *state = context;
  (void)error;
  if (state->fail_kind == (int)event->type + 1)
    return CAI_ERR_CANCELLED;
  assert(event->length >= 1U && event->length <= CAI_WIRE_MAX_RECORD);
  if (event->type == CAI_WIRE_BEGIN) {
    assert(state->begun == state->ended && event->token == NULL);
    ++state->begun;
  } else if (event->type == CAI_WIRE_END) {
    assert(state->begun == state->ended + 1U && event->token == NULL);
    ++state->ended;
  } else {
    assert(state->begun == state->ended + 1U && event->token != NULL);
    ++state->tokens;
    if (event->token->type == CAI_CBOR_DATA)
      state->bytes += event->token->length;
  }
  return CAI_OK;
}
static int forward(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  fixture *state = context;
  ++state->calls;
  if (state->fail_at != 0U && state->calls == state->fail_at)
    return CAI_ERR_TRANSPORT;
  return cai_wire_decoder_feed(state->decoder, bytes, count, error);
}
static void fragments(void) {
  static const unsigned char records[] = {
      0,   0,   0, 1,    0,    0, 0, 0, 9, 0xa2, 0,    0x63, 'a',
      'b', 'c', 1, 0x81, 0xf5, 0, 0, 0, 4, 0x7f, 0x61, 'a',  0xff};
  size_t split;
  for (split = 0U; split <= sizeof(records) + 1U; ++split) {
    fixture state;
    memset(&state, 0, sizeof(state));
    assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
           CAI_OK);
    if (split > sizeof(records)) {
      size_t i;
      for (i = 0U; i < sizeof(records); ++i)
        assert(cai_wire_decoder_feed(state.decoder, records + i, 1U, NULL) ==
               CAI_OK);
    } else {
      assert(cai_wire_decoder_feed(state.decoder, records, split, NULL) ==
             CAI_OK);
      assert(cai_wire_decoder_feed(state.decoder, records + split,
                                   sizeof(records) - split, NULL) == CAI_OK);
    }
    assert(cai_wire_decoder_finish(state.decoder, NULL) == CAI_OK);
    assert(state.begun == 3U && state.ended == 3U && state.bytes == 4U);
    cai_wire_decoder_close(state.decoder);
  }
  for (split = 1U; split < sizeof(records); ++split) {
    fixture state;
    int rc;
    memset(&state, 0, sizeof(state));
    assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
           CAI_OK);
    assert(cai_wire_decoder_feed(state.decoder, records, split, NULL) ==
           CAI_OK);
    rc = cai_wire_decoder_finish(state.decoder, NULL);
    assert(rc == (split == 5U || split == 18U ? CAI_OK : CAI_ERR_PROTOCOL));
    cai_wire_decoder_close(state.decoder);
  }
}
static void bad_records(void) {
  static const unsigned char records[][8] = {
      {0, 0, 0, 0},       {0, 2, 0, 1},       {0xff, 0xff, 0xff, 0xff},
      {0, 0, 0, 2, 0, 0}, {0, 0, 0, 1, 0x81}, {0, 0, 0, 1, 0x18}};
  static const size_t sizes[] = {4U, 4U, 4U, 6U, 5U, 5U};
  size_t test, split;
  for (test = 0U; test < sizeof(sizes) / sizeof(sizes[0]); ++test)
    for (split = 0U; split <= sizes[test]; ++split) {
      fixture state;
      int rc;
      memset(&state, 0, sizeof(state));
      assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
             CAI_OK);
      rc = cai_wire_decoder_feed(state.decoder, records[test], split, NULL);
      if (rc == CAI_OK)
        rc = cai_wire_decoder_feed(state.decoder, records[test] + split,
                                   sizes[test] - split, NULL);
      assert(rc == (test < 3U ? CAI_ERR_LIMIT : CAI_ERR_PROTOCOL));
      assert(state.ended == 0U);
      assert(cai_wire_decoder_feed(state.decoder, "", 0U, NULL) == rc);
      cai_wire_decoder_close(state.decoder);
    }
}
static int write_token(cai_wire_encoder *encoder, cai_cbor_type type,
                       uint64_t argument, const void *bytes, size_t length) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = bytes;
  token.length = length;
  return cai_wire_encoder_write(encoder, &token, NULL);
}
static void streaming(void) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_wire_encoder *encoder = NULL;
  fixture state;
  char bytes[2048];
  size_t record, i;
  memset(&state, 0, sizeof(state));
  memset(&callbacks, 0, sizeof(callbacks));
  memset(bytes, 'x', sizeof(bytes));
  callbacks.write = forward;
  callbacks.context = &state;
  assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
         CAI_OK);
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  assert(cai_wire_encoder_open(sink, &encoder, NULL) == CAI_OK);
  for (record = 0U; record < 1024U; ++record) {
    assert(cai_wire_encoder_begin(encoder, CAI_WIRE_CHUNK_SIZE + 3U, NULL) ==
           CAI_OK);
    assert(state.begun == record + 1U && state.ended == record);
    assert(write_token(encoder, CAI_CBOR_BYTES, CAI_WIRE_CHUNK_SIZE, NULL,
                       0U) == CAI_OK);
    for (i = 0U; i < CAI_WIRE_CHUNK_SIZE / sizeof(bytes); ++i) {
      assert(write_token(encoder, CAI_CBOR_DATA, 0U, bytes, sizeof(bytes)) ==
             CAI_OK);
      state.produced += sizeof(bytes);
      assert(state.bytes == state.produced);
    }
    assert(write_token(encoder, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U) ==
           CAI_OK);
    assert(cai_wire_encoder_end(encoder, NULL) == CAI_OK);
    assert(state.ended == state.begun);
  }
  assert(state.bytes == 32U * 1024U * 1024U);
  assert(cai_wire_decoder_finish(state.decoder, NULL) == CAI_OK);
  cai_wire_encoder_close(encoder);
  cai_sink_close(sink);
  cai_wire_decoder_close(state.decoder);
}
static void failures(void) {
  int test;
  for (test = 0; test < 11; ++test) {
    cai_sink_callbacks callbacks;
    cai_sink *sink = NULL;
    cai_wire_encoder *encoder = NULL;
    fixture state;
    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = forward;
    callbacks.context = &state;
    assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
           CAI_OK);
    assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
    assert(cai_wire_encoder_open(sink, &encoder, NULL) == CAI_OK);
    if (test == 0)
      assert(cai_wire_encoder_begin(encoder, 0U, NULL) == CAI_ERR_INVALID);
    else if (test == 1)
      assert(cai_wire_encoder_begin(encoder, CAI_WIRE_MAX_RECORD + 1U, NULL) ==
             CAI_ERR_INVALID);
    else if (test == 2)
      assert(cai_wire_encoder_end(encoder, NULL) == CAI_ERR_INVALID);
    else if (test == 3)
      assert(write_token(encoder, CAI_CBOR_NULL, 0U, NULL, 0U) ==
             CAI_ERR_INVALID);
    else if (test == 4) {
      assert(cai_wire_encoder_begin(encoder, 2U, NULL) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_NULL, 0U, NULL, 0U) == CAI_OK);
      assert(cai_wire_encoder_end(encoder, NULL) == CAI_ERR_PROTOCOL);
    } else if (test == 5) {
      assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_OK);
      assert(write_token(encoder, CAI_CBOR_UINT, 24U, NULL, 0U) ==
             CAI_ERR_PROTOCOL);
    } else if (test == 6) {
      assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_OK);
      assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_ERR_INVALID);
    } else if (test == 7) {
      state.fail_at = 1U;
      assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_ERR_TRANSPORT);
    } else {
      state.fail_kind = test - 7;
      if (test == 8)
        assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_ERR_CANCELLED);
      else {
        assert(cai_wire_encoder_begin(encoder, 1U, NULL) == CAI_OK);
        assert(write_token(encoder, CAI_CBOR_UINT, 0U, NULL, 0U) ==
               CAI_ERR_CANCELLED);
      }
    }
    cai_wire_encoder_close(encoder);
    cai_sink_close(sink);
    cai_wire_decoder_close(state.decoder);
  }
  {
    fixture state;
    memset(&state, 0, sizeof(state));
    assert(cai_wire_decoder_open(observe, &state, &state.decoder, NULL) ==
           CAI_OK);
    assert(cai_wire_decoder_feed(state.decoder, NULL, 0U, NULL) == CAI_OK);
    assert(cai_wire_decoder_finish(state.decoder, NULL) == CAI_OK);
    assert(cai_wire_decoder_feed(state.decoder, "", 0U, NULL) ==
           CAI_ERR_INVALID);
    cai_wire_decoder_close(state.decoder);
  }
}
int main(void) {
  fragments();
  bad_records();
  streaming();
  failures();
  return 0;
}
