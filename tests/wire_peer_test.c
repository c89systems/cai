#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/wire.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct receiver {
  cai_wire_peer *peer;
  size_t begins, ends, bytes;
  int reply;
} receiver;
static int observe(void *context, const cai_wire_event *event,
                   cai_error *error) {
  receiver *state = context;
  (void)error;
  if (event->type == CAI_WIRE_BEGIN)
    ++state->begins;
  else if (event->type == CAI_WIRE_END) {
    ++state->ends;
    if (state->reply) {
      cai_cbor_token token;
      memset(&token, 0, sizeof(token));
      token.type = CAI_CBOR_NULL;
      assert(cai_wire_peer_begin(state->peer, 1U, NULL) == CAI_OK);
      assert(cai_wire_peer_write(state->peer, &token, NULL) == CAI_OK);
      assert(cai_wire_peer_end(state->peer, NULL) == CAI_OK);
    }
  } else if (event->token->type == CAI_CBOR_DATA) {
    const unsigned char *bytes = event->token->data;
    size_t i;
    for (i = 0U; i < event->token->length; ++i)
      assert(bytes[i] == 'x');
    state->bytes += event->token->length;
  }
  return CAI_OK;
}
static int write_token(cai_wire_peer *peer, cai_cbor_type type,
                       uint64_t argument, const void *bytes, size_t length) {
  cai_cbor_token token;
  memset(&token, 0, sizeof(token));
  token.type = type;
  token.argument = argument;
  token.data = bytes;
  token.length = length;
  return cai_wire_peer_write(peer, &token, NULL);
}
static void string_record(cai_wire_peer *peer, size_t size) {
  char data[2048];
  size_t i;
  memset(data, 'x', sizeof(data));
  assert(size >= 65536U && size <= CAI_WIRE_MAX_RECORD - 5U);
  assert(cai_wire_peer_begin(peer, (uint32_t)size + 5U, NULL) == CAI_OK);
  assert(write_token(peer, CAI_CBOR_TEXT, size, NULL, 0U) == CAI_OK);
  for (i = 0U; i < size;) {
    size_t take = size - i < sizeof(data) ? size - i : sizeof(data);
    assert(write_token(peer, CAI_CBOR_DATA, 0U, data, take) == CAI_OK);
    i += take;
  }
  assert(write_token(peer, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U) == CAI_OK);
  assert(cai_wire_peer_end(peer, NULL) == CAI_OK);
}
static void short_io(void) {
  int sockets[2], buffer = 4096, eof;
  receiver a, b;
  const size_t body = CAI_WIRE_MAX_RECORD - 5U;
  size_t i;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &buffer,
                    sizeof(buffer)) == 0);
  assert(cai_wire_peer_open(sockets[0], 0U, observe, &a, &a.peer, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_open(sockets[1], 0U, observe, &b, &b.peer, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_fd(a.peer) == sockets[0]);
  assert((fcntl(sockets[0], F_GETFL) & O_NONBLOCK) != 0);
  assert((fcntl(sockets[0], F_GETFD) & FD_CLOEXEC) != 0);
  assert(cai_wire_peer_read(b.peer, 0U, &eof, NULL) == CAI_OK && !eof);
  b.reply = 1;
  string_record(a.peer, body);
  assert(cai_wire_peer_pending(a.peer) == CAI_WIRE_MAX_RECORD + 4U);
  /* A header is fragmented across actual socket writes and reads. */
  for (i = 0U; i < 3U; ++i) {
    assert(cai_wire_peer_flush(a.peer, 1U, NULL) == CAI_OK);
    assert(cai_wire_peer_read(b.peer, 1U, &eof, NULL) == CAI_OK && !eof);
    assert(b.begins == 0U);
  }
  assert(cai_wire_peer_flush(a.peer, CAI_WIRE_MAX_RECORD, NULL) == CAI_OK);
  /* Small kernel send buffer forces partial sends/EAGAIN without waiting. */
  assert(cai_wire_peer_pending(a.peer) > 0U);
  for (i = 0U; i < 10000U && b.ends == 0U; ++i) {
    assert(cai_wire_peer_read(b.peer, 997U, &eof, NULL) == CAI_OK && !eof);
    assert(cai_wire_peer_flush(a.peer, 631U, NULL) == CAI_OK);
  }
  assert(b.ends == 1U && b.bytes == body);
  assert(cai_wire_peer_pending(a.peer) == 0U);
  assert(cai_wire_peer_pending(b.peer) == 5U);
  assert(cai_wire_peer_flush(b.peer, 0U, NULL) == CAI_OK);
  assert(cai_wire_peer_read(a.peer, 0U, &eof, NULL) == CAI_OK && !eof);
  assert(a.ends == 1U);
  for (i = 0U; i < 1000U; ++i) {
    assert(cai_wire_peer_begin(a.peer, 1U, NULL) == CAI_OK);
    assert(write_token(a.peer, CAI_CBOR_NULL, 0U, NULL, 0U) == CAI_OK);
    assert(cai_wire_peer_end(a.peer, NULL) == CAI_OK);
    assert(cai_wire_peer_flush(a.peer, 0U, NULL) == CAI_OK);
    assert(cai_wire_peer_read(b.peer, 0U, &eof, NULL) == CAI_OK && !eof);
    assert(cai_wire_peer_flush(b.peer, 0U, NULL) == CAI_OK);
    assert(cai_wire_peer_read(a.peer, 0U, &eof, NULL) == CAI_OK && !eof);
    assert(a.ends == i + 2U && b.ends == i + 2U);
    assert(cai_wire_peer_pending(a.peer) == 0U);
  }
  assert(shutdown(sockets[0], SHUT_WR) == 0);
  assert(cai_wire_peer_read(b.peer, 0U, &eof, NULL) == CAI_OK && eof);
  assert(cai_wire_peer_read(b.peer, 0U, &eof, NULL) == CAI_OK && eof);
  cai_wire_peer_close(a.peer);
  cai_wire_peer_close(b.peer);
}
static void streamed_records(void) {
  int sockets[2], eof;
  receiver a, b;
  char bytes[2048];
  size_t record, chunk, produced = 0U;
  memset(&a, 0, sizeof(a));
  memset(&b, 0, sizeof(b));
  memset(bytes, 'x', sizeof(bytes));
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cai_wire_peer_open(sockets[0], 0U, observe, &a, &a.peer, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_open(sockets[1], 0U, observe, &b, &b.peer, NULL) ==
         CAI_OK);
  for (record = 0U; record < 1024U; ++record) {
    assert(cai_wire_peer_begin(a.peer, CAI_WIRE_CHUNK_SIZE + 3U, NULL) ==
           CAI_OK);
    assert(write_token(a.peer, CAI_CBOR_TEXT, CAI_WIRE_CHUNK_SIZE, NULL, 0U) ==
           CAI_OK);
    for (chunk = 0U; chunk < CAI_WIRE_CHUNK_SIZE / sizeof(bytes); ++chunk) {
      assert(write_token(a.peer, CAI_CBOR_DATA, 0U, bytes, sizeof(bytes)) ==
             CAI_OK);
      produced += sizeof(bytes);
      assert(cai_wire_peer_flush(a.peer, 0U, NULL) == CAI_OK);
      assert(cai_wire_peer_read(b.peer, 0U, &eof, NULL) == CAI_OK && !eof);
      /* Prove socket delivery while producing, including before this record
       * is finished; no full record or logical stream is staged for sending. */
      assert(b.bytes == produced);
      if (chunk == 0U)
        assert(b.begins == record + 1U && b.ends == record);
    }
    assert(write_token(a.peer, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U) ==
           CAI_OK);
    assert(cai_wire_peer_end(a.peer, NULL) == CAI_OK);
    assert(b.ends == record + 1U && cai_wire_peer_pending(a.peer) == 0U);
  }
  assert(b.bytes == 32U * 1024U * 1024U);
  cai_wire_peer_close(a.peer);
  cai_wire_peer_close(b.peer);
}
static void backlog(void) {
  int sockets[2];
  receiver state;
  const size_t size = 65536U;
  memset(&state, 0, sizeof(state));
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  assert(cai_wire_peer_open(sockets[0], size + 9U, observe, &state, &state.peer,
                            NULL) == CAI_OK);
  string_record(state.peer, size);
  assert(cai_wire_peer_pending(state.peer) == size + 9U);
  assert(cai_wire_peer_begin(state.peer, 1U, NULL) == CAI_ERR_LIMIT);
  assert(cai_wire_peer_flush(state.peer, 0U, NULL) == CAI_ERR_LIMIT);
  assert(cai_wire_peer_pending(state.peer) <= size + 9U);
  cai_wire_peer_close(state.peer);
  close(sockets[1]);
}
static void disconnects(void) {
  static const unsigned char record[] = {0, 0, 0, 4, 0x63, 'x', 'x', 'x'};
  size_t i;
  for (i = 0U; i < sizeof(record); ++i) {
    int sockets[2], eof = 0;
    receiver state;
    memset(&state, 0, sizeof(state));
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(cai_wire_peer_open(sockets[0], 0U, observe, &state, &state.peer,
                              NULL) == CAI_OK);
    assert(write(sockets[1], record, i) == (ssize_t)i);
    close(sockets[1]);
    assert(cai_wire_peer_read(state.peer, 0U, &eof, NULL) ==
           (i == 0U ? CAI_OK : CAI_ERR_PROTOCOL));
    assert(eof == (i == 0U));
    cai_wire_peer_close(state.peer);
  }
}
static void sigpipe(void) {
  pid_t child = fork();
  int status;
  assert(child >= 0);
  if (child == 0) {
    int sockets[2];
    receiver state;
    memset(&state, 0, sizeof(state));
    assert(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(cai_wire_peer_open(sockets[0], 0U, observe, &state, &state.peer,
                              NULL) == CAI_OK);
    close(sockets[1]);
    assert(cai_wire_peer_begin(state.peer, 1U, NULL) == CAI_OK);
    assert(write_token(state.peer, CAI_CBOR_NULL, 0U, NULL, 0U) == CAI_OK);
    assert(cai_wire_peer_end(state.peer, NULL) == CAI_OK);
    assert(cai_wire_peer_flush(state.peer, 0U, NULL) == CAI_ERR_TRANSPORT);
    cai_wire_peer_close(state.peer);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void invalid_open(void) {
  int descriptors[2];
  receiver state;
  memset(&state, 0, sizeof(state));
  assert(pipe(descriptors) == 0);
  assert(cai_wire_peer_open(descriptors[0], 0U, observe, &state, &state.peer,
                            NULL) == CAI_ERR_INVALID);
  assert(state.peer == NULL && fcntl(descriptors[0], F_GETFD) >= 0);
  close(descriptors[0]);
  close(descriptors[1]);
  assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) == 0);
  assert(cai_wire_peer_open(descriptors[0], 0U, observe, &state, &state.peer,
                            NULL) == CAI_ERR_INVALID);
  close(descriptors[0]);
  close(descriptors[1]);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  assert(cai_wire_peer_open(descriptors[0], 17U * 1024U * 1024U, observe,
                            &state, &state.peer, NULL) == CAI_ERR_INVALID);
  close(descriptors[0]);
  close(descriptors[1]);
}
int main(void) {
  short_io();
  streamed_records();
  backlog();
  disconnects();
  sigpipe();
  invalid_open();
  return 0;
}
