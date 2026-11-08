#include <assert.h>
#include <cai/protocol.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct consumer {
  cai_protocol_header expected;
  size_t headers, commits, tokens, bytes, floats;
  double number;
  int check_header, check_bytes, fail;
} consumer;
static int consume(void *context, const cai_protocol_event *event,
                   cai_error *error) {
  consumer *c = context;
  (void)error;
  if (c->fail == (int)event->type + 1)
    return CAI_ERR_CANCELLED;
  if (c->check_header) {
    assert(event->header->kind == c->expected.kind);
    assert(event->header->request == c->expected.request);
    assert(event->header->conversation == c->expected.conversation);
    assert(event->header->generation == c->expected.generation);
    assert(event->header->sequence == c->expected.sequence);
    assert(event->header->stream == c->expected.stream);
    assert(event->header->attachment == c->expected.attachment);
  }
  if (event->type == CAI_PROTOCOL_HEADER)
    ++c->headers;
  else if (event->type == CAI_PROTOCOL_COMPLETE)
    ++c->commits;
  else {
    assert(event->token != NULL);
    ++c->tokens;
    if (event->token->type == CAI_CBOR_FLOAT64) {
      ++c->floats;
      assert(cai_cbor_token_get_float64(event->token, &c->number, NULL) ==
             CAI_OK);
    }
    if (event->token->type == CAI_CBOR_DATA) {
      if (c->check_bytes) {
        const unsigned char *bytes = event->token->data;
        size_t i;
        for (i = 0U; i < event->token->length; ++i)
          assert(bytes[i] == 0x80U);
      }
      c->bytes += event->token->length;
    }
  }
  return CAI_OK;
}
static void open_decoder(consumer *c, cai_protocol_decoder **protocol,
                         cai_wire_decoder **wire) {
  assert(cai_protocol_decoder_open(consume, c, protocol, NULL) == CAI_OK);
  assert(cai_wire_decoder_open(cai_protocol_decoder_event, *protocol, wire,
                               NULL) == CAI_OK);
}
static const unsigned char null_record[] = {
    0, 0, 0, 17, 0xa8, 0, 5, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 0xf6};
static const unsigned char nested[] = {0xbf, 0x00, 0x82, 0xf5, 0x7f,
                                       0x61, 'x',  0x60, 0xff, 0xff};
static void fragments(void) {
  unsigned char bytes[sizeof(null_record) - 1U + sizeof(nested)];
  consumer c;
  cai_protocol_decoder *protocol;
  cai_wire_decoder *wire;
  size_t split, i;
  for (split = 0U; split <= sizeof(null_record); ++split) {
    memset(&c, 0, sizeof(c));
    c.check_header = 1;
    c.expected.kind = CAI_PROTOCOL_REQUEST;
    c.expected.request = 1U;
    c.expected.conversation = 2U;
    c.expected.generation = 3U;
    c.expected.sequence = 4U;
    c.expected.stream = 5U;
    c.expected.attachment = 6U;
    open_decoder(&c, &protocol, &wire);
    assert(cai_wire_decoder_feed(wire, null_record, split, NULL) == CAI_OK);
    assert(c.commits == (split == sizeof(null_record) ? 1U : 0U));
    assert(cai_wire_decoder_feed(wire, null_record + split,
                                 sizeof(null_record) - split, NULL) == CAI_OK);
    assert(c.headers == 1U && c.tokens == 1U && c.commits == 1U);
    /* No allocation/reset or stream reopening between records. */
    for (i = 0U; i < sizeof(null_record); ++i)
      assert(cai_wire_decoder_feed(wire, null_record + i, 1U, NULL) == CAI_OK);
    assert(c.headers == 2U && c.tokens == 2U && c.commits == 2U);
    assert(cai_wire_decoder_finish(wire, NULL) == CAI_OK);
    cai_wire_decoder_close(wire);
    cai_protocol_decoder_close(protocol);
  }
  memcpy(bytes, null_record, sizeof(null_record) - 1U);
  memcpy(bytes + sizeof(null_record) - 1U, nested, sizeof(nested));
  bytes[3] = (unsigned char)(sizeof(bytes) - 4U);
  memset(&c, 0, sizeof(c));
  open_decoder(&c, &protocol, &wire);
  for (i = 0U; i < sizeof(bytes); ++i) {
    assert(cai_wire_decoder_feed(wire, bytes + i, 1U, NULL) == CAI_OK);
    assert(c.commits == (i + 1U == sizeof(bytes) ? 1U : 0U));
  }
  assert(c.headers == 1U && c.bytes == 1U);
  assert(cai_wire_decoder_finish(wire, NULL) == CAI_OK);
  cai_wire_decoder_close(wire);
  cai_protocol_decoder_close(protocol);
}
static void floating_payload(void) {
  static const unsigned char number[] = {0xfb, 0x3f, 0xf8, 0, 0, 0, 0, 0, 0};
  unsigned char bytes[sizeof(null_record) + 8U];
  consumer c;
  cai_protocol_decoder *protocol;
  cai_wire_decoder *wire;
  size_t i;
  memcpy(bytes, null_record, sizeof(null_record) - 1U);
  memcpy(bytes + sizeof(null_record) - 1U, number, sizeof(number));
  bytes[3] = (unsigned char)(sizeof(bytes) - 4U);
  memset(&c, 0, sizeof(c));
  open_decoder(&c, &protocol, &wire);
  for (i = 0U; i < sizeof(bytes); ++i) {
    assert(cai_wire_decoder_feed(wire, bytes + i, 1U, NULL) == CAI_OK);
    assert(c.commits == (i + 1U == sizeof(bytes) ? 1U : 0U));
  }
  assert(c.floats == 1U && c.number == 1.5);
  assert(cai_wire_decoder_finish(wire, NULL) == CAI_OK);
  cai_wire_decoder_close(wire);
  cai_protocol_decoder_close(protocol);
  /* A floating envelope kind remains invalid even when numerically integral. */
  memcpy(bytes, null_record, 6U);
  memcpy(bytes + 6U, number, sizeof(number));
  memcpy(bytes + 6U + sizeof(number), null_record + 7U,
         sizeof(null_record) - 7U);
  bytes[3] = (unsigned char)(sizeof(bytes) - 4U);
  memset(&c, 0, sizeof(c));
  open_decoder(&c, &protocol, &wire);
  assert(cai_wire_decoder_feed(wire, bytes, sizeof(bytes), NULL) ==
         CAI_ERR_PROTOCOL);
  assert(c.commits == 0U && c.headers == 0U);
  cai_wire_decoder_close(wire);
  cai_protocol_decoder_close(protocol);
}
static void malformed(void) {
  unsigned char bytes[sizeof(null_record) + 1U];
  consumer c;
  cai_protocol_decoder *protocol;
  cai_wire_decoder *wire;
  size_t i;
  int rc;
  for (i = 0U; i < sizeof(null_record); ++i) {
    memset(&c, 0, sizeof(c));
    open_decoder(&c, &protocol, &wire);
    assert(cai_wire_decoder_feed(wire, null_record, i, NULL) == CAI_OK);
    rc = cai_wire_decoder_finish(wire, NULL);
    assert(rc == (i == 0U ? CAI_OK : CAI_ERR_PROTOCOL));
    assert(c.commits == 0U);
    cai_wire_decoder_close(wire);
    cai_protocol_decoder_close(protocol);
  }
  for (i = 0U; i < 10U; ++i) {
    size_t n = sizeof(null_record);
    memcpy(bytes, null_record, n);
    switch (i) {
    case 0:
      bytes[4] = 0x88; /* array instead of map */
      break;
    case 1:
      bytes[4] = 0xa7; /* missing required pair */
      break;
    case 2:
      bytes[4] = 0xbf; /* indefinite outer schema */
      break;
    case 3:
      /* Valid CBOR for an unknown kind, rather than an invalid additional
       * information byte masquerading as a kind above 23. */
      memmove(bytes + 8U, bytes + 7U, n - 7U);
      bytes[6] = 0x18;
      bytes[7] = (unsigned char)(CAI_PROTOCOL_TERMINAL_OUTPUT + 1);
      ++n;
      bytes[3] = (unsigned char)(n - 4U);
      break;
    case 4:
      bytes[9] = 1; /* duplicate key */
      break;
    case 5:
      bytes[13] = 5; /* out of order key */
      break;
    case 6:
      bytes[8] = 0x20; /* negative metadata */
      break;
    case 7:
      bytes[19] = 8; /* unknown payload key */
      break;
    case 8:
      bytes[3] = 18;
      bytes[n++] = 0; /* valid-looking record followed by second root */
      break;
    default:
      bytes[3] = 16; /* no payload */
      --n;
      break;
    }
    memset(&c, 0, sizeof(c));
    open_decoder(&c, &protocol, &wire);
    assert(cai_wire_decoder_feed(wire, bytes, n, NULL) == CAI_ERR_PROTOCOL);
    assert(c.commits == 0U);
    assert(cai_wire_decoder_feed(wire, null_record, sizeof(null_record),
                                 NULL) == CAI_ERR_PROTOCOL);
    cai_wire_decoder_close(wire);
    cai_protocol_decoder_close(protocol);
  }
}
static void callback_failure(void) {
  consumer c;
  cai_protocol_decoder *protocol;
  cai_wire_decoder *wire;
  cai_error error;
  int stage;
  for (stage = 1; stage <= 3; ++stage) {
    cai_error_init(&error);
    memset(&c, 0, sizeof(c));
    c.fail = stage;
    open_decoder(&c, &protocol, &wire);
    assert(cai_wire_decoder_feed(wire, null_record, sizeof(null_record),
                                 &error) == CAI_ERR_CANCELLED);
    assert(error.code == CAI_ERR_CANCELLED && c.commits == 0U);
    assert(cai_wire_decoder_finish(wire, NULL) == CAI_ERR_CANCELLED);
    cai_wire_decoder_close(wire);
    cai_protocol_decoder_close(protocol);
    cai_error_cleanup(&error);
  }
}
static int ignore(void *context, const cai_wire_event *event,
                  cai_error *error) {
  (void)context;
  (void)event;
  (void)error;
  return CAI_OK;
}
static void token(cai_wire_peer *peer, cai_cbor_type type, uint64_t argument,
                  const void *data, size_t length) {
  cai_cbor_token value;
  memset(&value, 0, sizeof(value));
  value.type = type;
  value.argument = argument;
  value.data = data;
  value.length = length;
  assert(cai_wire_peer_write(peer, &value, NULL) == CAI_OK);
}
static void transfer(cai_wire_peer *sender, cai_wire_peer *receiver) {
  int eof = 0;
  assert(cai_wire_peer_flush(sender, 0U, NULL) == CAI_OK);
  assert(cai_wire_peer_pending(sender) == 0U);
  assert(cai_wire_peer_read(receiver, 0U, &eof, NULL) == CAI_OK && !eof);
}
static void socket_stream(void) {
  static const uint64_t values[] = {
      0U,        23U,         24U,
      255U,      256U,        65535U,
      65536U,    0xffffffffU, UINT64_C(0x100000000),
      UINT64_MAX};
  unsigned char chunk[2048];
  cai_wire_peer *sender = NULL, *receiver = NULL;
  cai_protocol_decoder *protocol = NULL;
  int sockets[2], eof = 0;
  consumer c;
  size_t i, j, before;
  memset(&c, 0, sizeof(c));
  c.check_header = 1;
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cai_protocol_decoder_open(consume, &c, &protocol, NULL) == CAI_OK);
  assert(cai_wire_peer_open(sockets[0], 65536U, ignore, NULL, &sender, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_open(sockets[1], 65536U, cai_protocol_decoder_event,
                            protocol, &receiver, NULL) == CAI_OK);
  for (i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
    c.expected.kind = CAI_PROTOCOL_STATUS;
    c.expected.request = c.expected.conversation = c.expected.generation =
        c.expected.sequence = c.expected.stream = c.expected.attachment =
            values[i];
    assert(cai_protocol_peer_begin(sender, &c.expected, 1U, NULL) == CAI_OK);
    token(sender, CAI_CBOR_NULL, 0U, NULL, 0U);
    assert(cai_protocol_peer_end(sender, NULL) == CAI_OK);
    transfer(sender, receiver);
    assert(c.commits == i + 1U);
  }
  /* Nested indefinite values are streamed through the payload schema intact. */
  before = c.commits;
  assert(cai_protocol_peer_begin(sender, &c.expected, (uint32_t)sizeof(nested),
                                 NULL) == CAI_OK);
  {
    cai_cbor_token value;
    memset(&value, 0, sizeof(value));
    value.type = CAI_CBOR_MAP;
    value.indefinite = 1;
    assert(cai_wire_peer_write(sender, &value, NULL) == CAI_OK);
    token(sender, CAI_CBOR_UINT, 0U, NULL, 0U);
    token(sender, CAI_CBOR_ARRAY, 2U, NULL, 0U);
    token(sender, CAI_CBOR_BOOL, 1U, NULL, 0U);
    value.type = CAI_CBOR_TEXT;
    assert(cai_wire_peer_write(sender, &value, NULL) == CAI_OK);
    token(sender, CAI_CBOR_TEXT, 1U, NULL, 0U);
    token(sender, CAI_CBOR_DATA, 0U, "x", 1U);
    token(sender, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U);
    token(sender, CAI_CBOR_TEXT, 0U, NULL, 0U);
    token(sender, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U);
    token(sender, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U);
    token(sender, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U);
    token(sender, CAI_CBOR_END, CAI_CBOR_MAP, NULL, 0U);
  }
  assert(cai_protocol_peer_end(sender, NULL) == CAI_OK);
  transfer(sender, receiver);
  assert(c.commits == before + 1U && c.bytes == 1U);
  memset(chunk, 0x80, sizeof(chunk));
  c.check_bytes = 1;
  c.bytes = 0U;
  before = c.commits;
  c.expected.kind = CAI_PROTOCOL_STREAM_CHUNK;
  c.expected.stream = 42U;
  for (i = 0U; i < 1024U; ++i) {
    c.expected.sequence = (uint64_t)i;
    assert(cai_protocol_peer_begin(sender, &c.expected,
                                   CAI_WIRE_CHUNK_SIZE + 3U, NULL) == CAI_OK);
    token(sender, CAI_CBOR_BYTES, CAI_WIRE_CHUNK_SIZE, NULL, 0U);
    for (j = 0U; j < CAI_WIRE_CHUNK_SIZE / sizeof(chunk); ++j) {
      token(sender, CAI_CBOR_DATA, 0U, chunk, sizeof(chunk));
      transfer(sender, receiver);
      assert(c.bytes == i * CAI_WIRE_CHUNK_SIZE + (j + 1U) * sizeof(chunk));
      if ((j + 1U) * sizeof(chunk) < CAI_WIRE_CHUNK_SIZE)
        assert(c.commits == before + i);
    }
    token(sender, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U);
    assert(cai_protocol_peer_end(sender, NULL) == CAI_OK);
    transfer(sender, receiver);
    assert(c.commits == before + i + 1U);
  }
  assert(c.bytes == 32U * 1024U * 1024U);
  /* Reject impossible sizes/unknown kinds before emitting any prefix. */
  c.expected.kind = (cai_protocol_kind)(CAI_PROTOCOL_TERMINAL_OUTPUT + 1);
  assert(cai_protocol_peer_begin(sender, &c.expected, 1U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_wire_peer_pending(sender) == 0U);
  c.expected.kind = CAI_PROTOCOL_STATUS;
  assert(cai_protocol_peer_begin(sender, &c.expected, CAI_WIRE_MAX_RECORD,
                                 NULL) == CAI_ERR_LIMIT);
  assert(cai_protocol_peer_begin(sender, &c.expected, 0U, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_wire_peer_pending(sender) == 0U);
  cai_wire_peer_close(sender);
  assert(cai_wire_peer_read(receiver, 0U, &eof, NULL) == CAI_OK && eof);
  cai_wire_peer_close(receiver);
  cai_protocol_decoder_close(protocol);
}
static void *stream_worker(void *context) {
  (void)context;
  floating_payload();
  socket_stream();
  return NULL;
}
int main(void) {
  pthread_t thread;
  fragments();
  malformed();
  callback_failure();
  /* Use the actual default worker stack, including the musl build. */
  assert(pthread_create(&thread, NULL, stream_worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
