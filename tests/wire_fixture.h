/** @file wire_fixture.h
 * Raw socket producer for testing the real peer's fragment and EOF handling.
 * This harness owns no codec; all decoding happens in the production peer.
 */
#ifndef CAI_TEST_WIRE_FIXTURE_H
#define CAI_TEST_WIRE_FIXTURE_H
#include <assert.h>
#include <cai/wire.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct test_wire_input {
  cai_wire_peer *peer;
  int producer;
} test_wire_input;
static int test_wire_open(cai_wire_event_fn callback, void *context,
                          test_wire_input **out, cai_error *error) {
  int pair[2], rc;
  test_wire_input *f = calloc(1U, sizeof(*f));
  assert(f != NULL && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  f->producer = pair[0];
  rc = cai_wire_peer_open(pair[1], 0U, callback, context, &f->peer, error);
  assert(rc == CAI_OK);
  *out = f;
  return rc;
}
static int test_wire_feed(test_wire_input *f, const void *bytes, size_t length,
                          cai_error *error) {
  const unsigned char *at = bytes;
  int rc = CAI_OK, eof;
  if (length == 0U)
    return cai_wire_peer_read(f->peer, 1U, &eof, error);
  while (length != 0U && rc == CAI_OK) {
    size_t take = length < 512U ? length : 512U;
    assert(write(f->producer, at, take) == (ssize_t)take);
    rc = cai_wire_peer_read(f->peer, take, &eof, error);
    at += take;
    length -= take;
  }
  return rc;
}
static int test_wire_finish(test_wire_input *f, cai_error *error) {
  int eof = 0, rc;
  assert(shutdown(f->producer, SHUT_WR) == 0);
  rc = cai_wire_peer_read(f->peer, 0U, &eof, error);
  assert(rc != CAI_OK || eof);
  return rc;
}
static void test_wire_close(test_wire_input *f) {
  cai_wire_peer_close(f->peer);
  close(f->producer);
  free(f);
}
#endif
