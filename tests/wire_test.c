#include "wire_fixture.h"
#include <assert.h>
#include <cai/wire.h>
#include <string.h>

typedef struct fixture {
  test_wire_input *decoder;
  size_t begun, ended, tokens, bytes, produced, fail_at, calls;
  int fail_kind;
} fixture;
static int observe(void *context, const c89cbor_event *event,
                   cai_error *error) {
  fixture *state = context;
  (void)error;
  if (state->fail_kind == (int)event->type + 1)
    return CAI_ERR_CANCELLED;
  assert(event->length >= 1U && event->length <= CAI_WIRE_MAX_RECORD);
  if (event->type == C89CBOR_RECORD_BEGIN) {
    assert(state->begun == state->ended);
    ++state->begun;
  } else if (event->type == C89CBOR_RECORD_END) {
    assert(state->begun == state->ended + 1U);
    ++state->ended;
  } else {
    assert(state->begun == state->ended + 1U);
    ++state->tokens;
    if (event->token.type == C89CBOR_DATA)
      state->bytes += event->token.length;
  }
  return CAI_OK;
}
static void fragments(void) {
  static const unsigned char records[] = {
      0,   0,   0, 1,    0,    0, 0, 0, 9, 0xa2, 0,    0x63, 'a',
      'b', 'c', 1, 0x81, 0xf5, 0, 0, 0, 4, 0x7f, 0x61, 'a',  0xff};
  size_t split;
  for (split = 0U; split <= sizeof(records) + 1U; ++split) {
    fixture state;
    memset(&state, 0, sizeof(state));
    assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
    if (split > sizeof(records)) {
      size_t i;
      for (i = 0U; i < sizeof(records); ++i)
        assert(test_wire_feed(state.decoder, records + i, 1U, NULL) == CAI_OK);
    } else {
      assert(test_wire_feed(state.decoder, records, split, NULL) == CAI_OK);
      assert(test_wire_feed(state.decoder, records + split,
                            sizeof(records) - split, NULL) == CAI_OK);
    }
    assert(test_wire_finish(state.decoder, NULL) == CAI_OK);
    assert(state.begun == 3U && state.ended == 3U && state.bytes == 4U);
    test_wire_close(state.decoder);
  }
  for (split = 1U; split < sizeof(records); ++split) {
    fixture state;
    int rc;
    memset(&state, 0, sizeof(state));
    assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
    assert(test_wire_feed(state.decoder, records, split, NULL) == CAI_OK);
    rc = test_wire_finish(state.decoder, NULL);
    assert(rc == (split == 5U || split == 18U ? CAI_OK : CAI_ERR_PROTOCOL));
    test_wire_close(state.decoder);
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
      assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
      rc = test_wire_feed(state.decoder, records[test], split, NULL);
      if (rc == CAI_OK)
        rc = test_wire_feed(state.decoder, records[test] + split,
                            sizes[test] - split, NULL);
      assert(rc == (test < 3U ? CAI_ERR_LIMIT : CAI_ERR_PROTOCOL));
      assert(state.ended == 0U);
      assert(test_wire_feed(state.decoder, "", 0U, NULL) == rc);
      test_wire_close(state.decoder);
    }
}
/** Move bounded queued bytes through the real socket peer, with no codec shim.
 */
static void pump(cai_wire_peer *out, test_wire_input *in) {
  int eof;
  assert(cai_wire_peer_flush(out, 0U, NULL) == CAI_OK);
  assert(cai_wire_peer_read(in->peer, 0U, &eof, NULL) == CAI_OK && !eof);
}
static int write_token(cai_wire_peer *peer, c89cbor_type type,
                       uint64_t argument, const void *bytes, size_t length) {
  c89cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = bytes;
  token.length = length;
  return cai_wire_peer_write(peer, &token, NULL);
}
static void streaming(void) {
  fixture state;
  cai_wire_peer *writer;
  char bytes[2048];
  size_t record, i;
  memset(&state, 0, sizeof(state));
  memset(bytes, 'x', sizeof(bytes));
  assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
  assert(cai_wire_peer_open(dup(state.decoder->producer), 0U, observe, &state,
                            &writer, NULL) == CAI_OK);
  for (record = 0U; record < 1024U; ++record) {
    assert(cai_wire_peer_begin(writer, CAI_WIRE_CHUNK_SIZE + 3U, NULL) ==
           CAI_OK);
    pump(writer, state.decoder);
    assert(state.begun == record + 1U && state.ended == record);
    assert(write_token(writer, C89CBOR_BYTES, CAI_WIRE_CHUNK_SIZE, NULL, 0U) ==
           CAI_OK);
    for (i = 0U; i < CAI_WIRE_CHUNK_SIZE / sizeof(bytes); ++i) {
      assert(write_token(writer, C89CBOR_DATA, 0U, bytes, sizeof(bytes)) ==
             CAI_OK);
      pump(writer, state.decoder);
      state.produced += sizeof(bytes);
      assert(state.bytes == state.produced);
    }
    assert(write_token(writer, C89CBOR_END, C89CBOR_BYTES, NULL, 0U) == CAI_OK);
    assert(cai_wire_peer_end(writer, NULL) == CAI_OK);
    pump(writer, state.decoder);
    assert(state.ended == state.begun);
  }
  assert(state.bytes == 32U * 1024U * 1024U);
  cai_wire_peer_close(writer);
  assert(test_wire_finish(state.decoder, NULL) == CAI_OK);
  test_wire_close(state.decoder);
}
static void failures(void) {
  static const unsigned char record[] = {0, 0, 0, 1, 0};
  int stage;
  for (stage = 1; stage <= 3; ++stage) {
    fixture state;
    memset(&state, 0, sizeof(state));
    state.fail_kind = stage;
    assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
    assert(test_wire_feed(state.decoder, record, sizeof(record), NULL) ==
           CAI_ERR_CANCELLED);
    assert(test_wire_feed(state.decoder, NULL, 0U, NULL) == CAI_ERR_CANCELLED);
    assert(test_wire_finish(state.decoder, NULL) == CAI_ERR_CANCELLED);
    test_wire_close(state.decoder);
  }
  for (stage = 0; stage < 7; ++stage) {
    fixture state;
    cai_wire_peer *writer;
    int rc;
    memset(&state, 0, sizeof(state));
    assert(test_wire_open(observe, &state, &state.decoder, NULL) == CAI_OK);
    assert(cai_wire_peer_open(dup(state.decoder->producer), 0U, observe, &state,
                              &writer, NULL) == CAI_OK);
    if (stage < 2)
      rc = cai_wire_peer_begin(
          writer, stage == 0 ? 0U : CAI_WIRE_MAX_RECORD + 1U, NULL);
    else if (stage == 2)
      rc = cai_wire_peer_end(writer, NULL);
    else if (stage == 3)
      rc = write_token(writer, C89CBOR_NULL, 0U, NULL, 0U);
    else {
      assert(cai_wire_peer_begin(writer, stage == 4 ? 2U : 1U, NULL) == CAI_OK);
      if (stage == 4) {
        assert(write_token(writer, C89CBOR_NULL, 0U, NULL, 0U) == CAI_OK);
        rc = cai_wire_peer_end(writer, NULL);
      } else if (stage == 5)
        rc = write_token(writer, C89CBOR_UINT, 24U, NULL, 0U);
      else
        rc = cai_wire_peer_begin(writer, 1U, NULL);
    }
    assert(rc ==
           (stage == 4 || stage == 5 ? CAI_ERR_PROTOCOL : CAI_ERR_INVALID));
    assert(cai_wire_peer_end(writer, NULL) == rc);
    cai_wire_peer_close(writer);
    test_wire_close(state.decoder);
  }
}
int main(void) {
  fragments();
  bad_records();
  streaming();
  failures();
  return 0;
}
