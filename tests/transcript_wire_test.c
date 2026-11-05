#include "../cli/transcript_wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  cai_cli_transcript_decoder *input;
  cai_cbor_decoder *decoder;
  cai_cbor_encoder *encoder;
  cai_sink *sink;
  cai_protocol_header header;
  const cai_cli_transcript_message *complete;
  size_t bytes, declared, writes;
  int cancel;
} fixture;
static int receive(void *context, const cai_cbor_token *token,
                   cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_cli_transcript_message *value = NULL;
  int rc;
  event.type = CAI_PROTOCOL_PAYLOAD;
  event.header = &f->header;
  event.token = token;
  rc = cai_cli_transcript_decode(f->input, &event, &value, error);
  assert(value == NULL); /* Not an accepted record before frame completion. */
  return rc;
}
static int bytes(void *context, const void *data, size_t length,
                 cai_error *error) {
  fixture *f = context;
  const unsigned char *at = data;
  size_t i;
  ++f->writes;
  if (f->cancel && f->writes == 3U)
    return CAI_ERR_CANCELLED;
  f->bytes += length;
  /* Split every CBOR scalar, string header and binary/UTF-8 boundary. */
  for (i = 0U; i < length; ++i) {
    int rc = cai_cbor_decoder_feed(f->decoder, at + i, 1U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int begin(void *context, const cai_protocol_header *header,
                 uint32_t length, cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_cli_transcript_message *value = NULL;
  f->header = *header;
  f->declared = length;
  event.type = CAI_PROTOCOL_HEADER;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_transcript_decode(f->input, &event, &value, error);
}
static int write_token(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  return cai_cbor_encoder_write(((fixture *)context)->encoder, token, error);
}
static int complete(fixture *f, cai_error *error) {
  cai_protocol_event event;
  event.type = CAI_PROTOCOL_COMPLETE;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_transcript_decode(f->input, &event, &f->complete, error);
}
static int end(void *context, cai_error *error) {
  fixture *f = context;
  int rc = cai_cbor_encoder_finish(f->encoder, error);
  if (rc == CAI_OK)
    rc = cai_cbor_decoder_finish(f->decoder, error);
  if (rc != CAI_OK)
    return rc;
  assert(f->bytes == f->declared && f->complete == NULL);
  return complete(f, error);
}
static void open_fixture(fixture *f) {
  cai_sink_callbacks sink;
  memset(f, 0, sizeof(*f));
  memset(&sink, 0, sizeof(sink));
  sink.context = f;
  sink.write = bytes;
  assert(cai_cli_transcript_decoder_open(&f->input, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(receive, f, &f->decoder, NULL) == CAI_OK);
  assert(cai_sink_from_callbacks(&sink, &f->sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->decoder);
  cai_cli_transcript_decoder_close(f->input);
  cai_sink_close(f->sink);
}
static int send(fixture *f, const cai_cli_transcript_message *m) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = write_token;
  out.end = end;
  return cai_cli_transcript_write(&out, m, NULL);
}
static void message(cai_cli_transcript_message *m, cai_protocol_kind kind) {
  memset(m, 0, sizeof(*m));
  m->header.kind = kind;
  m->header.conversation = 7U;
  m->header.request = 65536U;
  m->header.stream =
      kind == CAI_PROTOCOL_SUBSCRIBE || kind == CAI_PROTOCOL_HISTORY_GET
          ? 0U
          : m->header.request;
  m->header.sequence = kind == CAI_PROTOCOL_STREAM_BEGIN ||
                               kind == CAI_PROTOCOL_STREAM_CHUNK ||
                               kind == CAI_PROTOCOL_STREAM_END
                           ? UINT64_MAX
                           : 0U;
  m->event.session = "conversation";
  m->event.collection = "12345678901234567890";
  m->event.type = "assistant_text_delta";
  m->event.watermark = UINT64_MAX;
  m->event.visibility = 256U;
  m->event.omitted = UINT64_MAX;
  m->replay_turns = -1LL;
  m->event.timestamp_ns = 1793780000000000000ULL;
  m->event.utc_offset_seconds = -12600;
  m->event.has_data = 1;
  m->event.position = 4294967296ULL;
  m->event.sequence = UINT64_MAX;
  m->event.offset = 255U;
  m->event.failure = CAI_TRANSCRIPT_SLOW;
  m->event.code = CAI_ERR_LIMIT;
  m->event.reason = "resynchronize";
}
static void roundtrips(void) {
  static const int64_t limits[] = {-1, 0, 1, 10, INT64_MAX};
  static const cai_protocol_kind kinds[] = {
      CAI_PROTOCOL_SUBSCRIBE,         CAI_PROTOCOL_HISTORY_GET,
      CAI_PROTOCOL_UNSUBSCRIBE,       CAI_PROTOCOL_REPLAY_BEGIN,
      CAI_PROTOCOL_STREAM_BEGIN,      CAI_PROTOCOL_STREAM_CHUNK,
      CAI_PROTOCOL_STREAM_END,        CAI_PROTOCOL_REPLAY_END,
      CAI_PROTOCOL_SUBSCRIPTION_ERROR};
  fixture *f = calloc(1U, sizeof(*f));
  unsigned char *binary = malloc(CAI_CLI_TRANSCRIPT_CHUNK);
  cai_cli_transcript_message m;
  size_t i;
  assert(f != NULL && binary != NULL);
  for (i = 0U; i < CAI_CLI_TRANSCRIPT_CHUNK; ++i)
    binary[i] = (unsigned char)i;
  for (i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    const cai_transcript_event *got;
    message(&m, kinds[i]);
    m.event.data = binary;
    m.event.length = CAI_CLI_TRANSCRIPT_CHUNK;
    open_fixture(f);
    assert(send(f, &m) == CAI_OK && f->complete != NULL);
    got = &f->complete->event;
    if (kinds[i] == CAI_PROTOCOL_SUBSCRIBE ||
        kinds[i] == CAI_PROTOCOL_HISTORY_GET)
      assert(strcmp(got->session, m.event.session) == 0 &&
             strcmp(got->collection, m.event.collection) == 0 &&
             got->position == m.event.position && got->sequence == UINT64_MAX);
    if (kinds[i] == CAI_PROTOCOL_REPLAY_BEGIN)
      assert(strcmp(got->collection, m.event.collection) == 0 &&
             got->watermark == UINT64_MAX && got->visibility == 256U &&
             got->omitted == UINT64_MAX);
    if (kinds[i] == CAI_PROTOCOL_SUBSCRIBE)
      assert(f->complete->replay_turns == -1LL);
    if (kinds[i] == CAI_PROTOCOL_STREAM_BEGIN)
      assert(got->timestamp_ns == m.event.timestamp_ns &&
             got->utc_offset_seconds == -12600 && got->has_data &&
             strcmp(got->type, m.event.type) == 0);
    if (kinds[i] == CAI_PROTOCOL_STREAM_CHUNK)
      assert(got->length == CAI_CLI_TRANSCRIPT_CHUNK && got->offset == 255U &&
             memcmp(got->data, binary, got->length) == 0);
    if (kinds[i] == CAI_PROTOCOL_STREAM_END)
      assert(got->position == m.event.position && got->offset == 255U);
    if (kinds[i] == CAI_PROTOCOL_REPLAY_END)
      assert(got->watermark == UINT64_MAX);
    if (kinds[i] == CAI_PROTOCOL_SUBSCRIPTION_ERROR)
      assert(got->failure == CAI_TRANSCRIPT_SLOW &&
             got->code == CAI_ERR_LIMIT &&
             strcmp(got->reason, m.event.reason) == 0);
    close_fixture(f);
  }
  for (i = 0U; i < 4U; ++i) {
    message(&m, CAI_PROTOCOL_STREAM_BEGIN);
    m.event.utc_offset_seconds = i == 0U   ? INT64_MIN
                                 : i == 1U ? INT64_MAX
                                 : i == 2U ? 0
                                           : 19800;
    m.event.has_data = 0;
    open_fixture(f);
    assert(send(f, &m) == CAI_OK &&
           f->complete->event.utc_offset_seconds ==
               m.event.utc_offset_seconds &&
           !f->complete->event.has_data);
    close_fixture(f);
  }
  for (i = 0U; i < sizeof(limits) / sizeof(limits[0]); ++i) {
    message(&m, CAI_PROTOCOL_SUBSCRIBE);
    m.event.position = m.event.sequence = 0U;
    m.replay_turns = limits[i];
    open_fixture(f);
    assert(send(f, &m) == CAI_OK && f->complete->replay_turns == limits[i]);
    close_fixture(f);
  }
  message(&m, CAI_PROTOCOL_STREAM_CHUNK);
  open_fixture(f);
  assert(send(f, &m) == CAI_OK && f->complete->event.length == 0U);
  close_fixture(f);
  m.event.data = binary;
  m.event.length = CAI_CLI_TRANSCRIPT_CHUNK;
  open_fixture(f);
  f->cancel = 1;
  assert(send(f, &m) == CAI_ERR_CANCELLED && f->complete == NULL);
  close_fixture(f);
  free(binary);
  free(f);
}
static void rejected(void) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_transcript_message m;
  char oversized[194];
  size_t i;
  assert(f != NULL);
  memset(oversized, 'x', sizeof(oversized) - 1U);
  oversized[sizeof(oversized) - 1U] = '\0';
  for (i = 0U; i < 15U; ++i) {
    message(&m, CAI_PROTOCOL_STREAM_BEGIN);
    switch (i) {
    case 0U:
      m.header.request = 0U;
      break;
    case 1U:
      m.header.stream = 1U;
      break;
    case 2U:
      m.header.sequence = 0U;
      break;
    case 3U:
      m.header.conversation = 0U;
      break;
    case 4U:
      m.header.kind = CAI_PROTOCOL_REPLAY_BEGIN;
      break;
    case 5U:
      m.header.kind = CAI_PROTOCOL_SUBSCRIBE;
      break;
    case 6U:
      m.event.type = oversized;
      break;
    case 7U:
      m.event.has_data = 2;
      break;
    case 8U:
      message(&m, CAI_PROTOCOL_STREAM_CHUNK);
      m.event.length = CAI_CLI_TRANSCRIPT_CHUNK + 1U;
      break;
    case 9U:
      message(&m, CAI_PROTOCOL_STREAM_CHUNK);
      m.event.length = 1U; /* Missing bytes. */
      break;
    case 10U:
      message(&m, CAI_PROTOCOL_SUBSCRIPTION_ERROR);
      m.event.failure = (cai_transcript_failure)-1;
      break;
    case 11U:
      message(&m, CAI_PROTOCOL_SUBSCRIPTION_ERROR);
      m.event.code = CAI_OK;
      break;
    case 12U:
      message(&m, CAI_PROTOCOL_SUBSCRIPTION_ERROR);
      m.event.reason = oversized;
      break;
    case 13U:
      message(&m, CAI_PROTOCOL_SUBSCRIBE);
      m.event.position = m.event.sequence = 0U;
      m.replay_turns = -2;
      break;
    case 14U:
      message(&m, CAI_PROTOCOL_SUBSCRIBE);
      m.replay_turns = 1; /* A cursor must preserve all missed work. */
      break;
    }
    open_fixture(f);
    assert(send(f, &m) == CAI_ERR_PROTOCOL && f->bytes == 0U);
    close_fixture(f);
  }
  free(f);
}
static void malformed(void) {
  static const unsigned char cases[][16] = {
      {0x83},
      {0x85},
      {0x9f},
      {0x84, 0x20},
      {0x84, 0, 0xf6},
      {0x84, 0, 0x1b, 0x80, 0, 0, 0, 0, 0, 0, 0},
      {0x84, 0, 0, 0x61, 'x'},
      {0x84, 0, 0, 0x43, 'a', 0, 'b', 0xf4},
      {0x84, 0, 0, 0x58, 129},
      {0x84, 0, 0, 0x40, 0},
      {0x84, 0, 0, 0x40, 0xf4, 0},
      {0x84, 0, 0}};
  static const size_t sizes[] = {1U, 1U, 1U, 2U, 3U, 11U,
                                 5U, 8U, 5U, 5U, 6U, 3U};
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_transcript_message m;
  size_t i;
  assert(f != NULL);
  message(&m, CAI_PROTOCOL_STREAM_BEGIN);
  for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    int rc;
    open_fixture(f);
    assert(begin(f, &m.header, (uint32_t)sizes[i], NULL) == CAI_OK);
    rc = bytes(f, cases[i], sizes[i], NULL);
    if (rc == CAI_OK)
      rc = cai_cbor_decoder_finish(f->decoder, NULL);
    if (rc == CAI_OK)
      rc = complete(f, NULL);
    assert(rc == CAI_ERR_PROTOCOL && f->complete == NULL);
    close_fixture(f);
  }
  open_fixture(f);
  assert(begin(f, &m.header, 0U, NULL) == CAI_OK);
  assert(complete(f, NULL) == CAI_ERR_PROTOCOL && f->complete == NULL);
  close_fixture(f);
  free(f);
}
static void *run(void *unused) {
  (void)unused;
  roundtrips();
  rejected();
  malformed();
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
