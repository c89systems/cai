#define _POSIX_C_SOURCE 200809L
#include "cai_internal.h"
#include <cai/wire.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BLOCK_SIZE 4096U
#define IO_BUDGET (64U * 1024U)
typedef struct wire_block {
  struct wire_block *next;
  size_t begin, end;
  unsigned char bytes[BLOCK_SIZE];
} wire_block;
struct cai_wire_peer {
  cai_wire_decoder *decoder;
  cai_wire_encoder *encoder;
  cai_sink *sink;
  wire_block *head, *tail;
  size_t pending, limit;
  int fd, failed, eof, reading, writing;
};
static int peer_fail(cai_wire_peer *peer, cai_error *error, int code,
                     const char *message) {
  peer->failed = code;
  return cai_set_error(error, code, message);
}
static int valid(cai_wire_peer *peer, cai_error *error) {
  if (peer == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "Wire peer is required");
  return peer->failed
             ? cai_set_error(error, peer->failed, "Wire peer has failed")
             : CAI_OK;
}
static int peer_transport(cai_wire_peer *peer, cai_error *error,
                          const char *message) {
  peer->failed = CAI_ERR_TRANSPORT;
  return cai_set_error_detail(error, CAI_ERR_TRANSPORT, message,
                              strerror(errno));
}
static int enqueue(void *context, const void *bytes, size_t count,
                   cai_error *error) {
  cai_wire_peer *peer = context;
  const unsigned char *input = bytes;
  if (count > peer->limit - peer->pending)
    return peer_fail(peer, error, CAI_ERR_LIMIT,
                     "Wire subscriber backlog exceeded; reconnect and sync");
  while (count != 0U) {
    size_t take;
    if (peer->tail == NULL || peer->tail->end == BLOCK_SIZE) {
      wire_block *block = cai_alloc(NULL, sizeof(*block));
      if (block == NULL)
        return peer_fail(peer, error, CAI_ERR_NOMEM,
                         "Allocate bounded wire output block");
      memset(block, 0, offsetof(wire_block, bytes));
      if (peer->tail != NULL)
        peer->tail->next = block;
      else
        peer->head = block;
      peer->tail = block;
    }
    take = BLOCK_SIZE - peer->tail->end;
    if (take > count)
      take = count;
    memcpy(peer->tail->bytes + peer->tail->end, input, take);
    peer->tail->end += take;
    peer->pending += take;
    input += take;
    count -= take;
  }
  return CAI_OK;
}
int cai_wire_peer_open(int descriptor, size_t backlog_limit,
                       cai_wire_event_fn callback, void *context,
                       cai_wire_peer **out, cai_error *error) {
  struct sockaddr_un address;
  socklen_t size = sizeof(address);
  cai_sink_callbacks callbacks;
  cai_wire_peer *peer;
  int rc, flags, descriptor_flags, type;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || callback == NULL || descriptor < 0 ||
      backlog_limit > 16U * 1024U * 1024U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Invalid wire peer configuration");
  if (getsockname(descriptor, (struct sockaddr *)&address, &size) != 0 ||
      address.sun_family != AF_UNIX)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire peer requires a Unix socket");
  size = sizeof(type);
  if (getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) != 0 ||
      type != SOCK_STREAM)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire peer requires a stream socket");
  size = sizeof(address);
  if (getpeername(descriptor, (struct sockaddr *)&address, &size) != 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Wire peer socket is not connected");
  flags = fcntl(descriptor, F_GETFL);
  descriptor_flags = fcntl(descriptor, F_GETFD);
  if (flags < 0 || descriptor_flags < 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Inspect wire socket descriptor");
  peer = cai_alloc(NULL, sizeof(*peer));
  if (peer == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "Allocate wire peer");
  memset(peer, 0, sizeof(*peer));
  peer->fd = -1;
  peer->limit = backlog_limit != 0U ? backlog_limit : 1024U * 1024U;
  rc = cai_wire_decoder_open(callback, context, &peer->decoder, error);
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = enqueue;
  callbacks.context = peer;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&callbacks, &peer->sink, error);
  if (rc == CAI_OK)
    rc = cai_wire_encoder_open(peer->sink, &peer->encoder, error);
#ifdef SO_NOSIGPIPE
  if (rc == CAI_OK) {
    int enabled = 1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0)
      rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Suppress wire socket SIGPIPE");
  }
#endif
  if (rc == CAI_OK &&
      (fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ||
       fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0)) {
    (void)fcntl(descriptor, F_SETFL, flags);
    (void)fcntl(descriptor, F_SETFD, descriptor_flags);
    rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                       "Configure nonblocking wire socket");
  }
  if (rc == CAI_OK) {
    peer->fd = descriptor;
    *out = peer;
  } else
    cai_wire_peer_close(peer);
  return rc;
}
int cai_wire_peer_fd(const cai_wire_peer *peer) {
  return peer != NULL ? peer->fd : -1;
}
size_t cai_wire_peer_pending(const cai_wire_peer *peer) {
  return peer != NULL ? peer->pending : 0U;
}
int cai_wire_peer_read(cai_wire_peer *peer, size_t budget, int *eof,
                       cai_error *error) {
  unsigned char bytes[2048];
  size_t attempts = 0U;
  int rc = valid(peer, error);
  if (eof != NULL)
    *eof = 0;
  if (rc != CAI_OK)
    return rc;
  if (eof == NULL || peer->reading)
    return peer_fail(peer, error, CAI_ERR_INVALID,
                     "Wire read requires EOF output and no nested read");
  if (peer->eof) {
    *eof = 1;
    return CAI_OK;
  }
  peer->reading = 1;
  if (budget == 0U)
    budget = IO_BUDGET;
  while (budget != 0U && rc == CAI_OK && attempts++ < 128U) {
    size_t take = budget < sizeof(bytes) ? budget : sizeof(bytes);
    ssize_t count = recv(peer->fd, bytes, take, 0);
    if (count > 0) {
      budget -= (size_t)count;
      rc = cai_wire_decoder_feed(peer->decoder, bytes, (size_t)count, error);
    } else if (count == 0) {
      rc = cai_wire_decoder_finish(peer->decoder, error);
      if (rc == CAI_OK)
        *eof = peer->eof = 1;
      break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    else if (errno != EINTR)
      rc = peer_transport(peer, error, "Read wire socket");
  }
  peer->reading = 0;
  if (rc != CAI_OK)
    peer->failed = rc;
  return rc;
}
int cai_wire_peer_flush(cai_wire_peer *peer, size_t budget, cai_error *error) {
  size_t attempts = 0U;
  int flags = 0, rc = valid(peer, error);
  if (rc != CAI_OK)
    return rc;
  if (peer->writing)
    return peer_fail(peer, error, CAI_ERR_INVALID, "Nested wire flush");
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  peer->writing = 1;
  if (budget == 0U)
    budget = IO_BUDGET;
  while (budget != 0U && peer->pending != 0U && rc == CAI_OK &&
         attempts++ < 128U) {
    wire_block *block = peer->head;
    size_t take = block->end - block->begin;
    ssize_t count;
    if (take > budget)
      take = budget;
    count = send(peer->fd, block->bytes + block->begin, take, flags);
    if (count > 0) {
      block->begin += (size_t)count;
      peer->pending -= (size_t)count;
      budget -= (size_t)count;
      if (block->begin == block->end) {
        if (block == peer->tail) {
          /* Reuse one bounded block for ordinary delta/receipt traffic. */
          block->begin = block->end = 0U;
        } else {
          peer->head = block->next;
          cai_free_mem(NULL, block);
        }
      }
    } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    else if (count == 0)
      rc = peer_fail(peer, error, CAI_ERR_TRANSPORT,
                     "Wire socket made no write progress");
    else if (errno != EINTR)
      rc = peer_transport(peer, error, "Write wire socket");
  }
  peer->writing = 0;
  return rc;
}
int cai_wire_peer_begin(cai_wire_peer *peer, uint32_t length,
                        cai_error *error) {
  int rc = valid(peer, error);
  if (rc == CAI_OK)
    rc = cai_wire_encoder_begin(peer->encoder, length, error);
  if (peer != NULL && rc != CAI_OK)
    peer->failed = rc;
  return rc;
}
int cai_wire_peer_write(cai_wire_peer *peer, const cai_cbor_token *token,
                        cai_error *error) {
  int rc = valid(peer, error);
  if (rc == CAI_OK)
    rc = cai_wire_encoder_write(peer->encoder, token, error);
  if (peer != NULL && rc != CAI_OK)
    peer->failed = rc;
  return rc;
}
int cai_wire_peer_end(cai_wire_peer *peer, cai_error *error) {
  int rc = valid(peer, error);
  if (rc == CAI_OK)
    rc = cai_wire_encoder_end(peer->encoder, error);
  if (peer != NULL && rc != CAI_OK)
    peer->failed = rc;
  return rc;
}
void cai_wire_peer_close(cai_wire_peer *peer) {
  if (peer != NULL) {
    wire_block *block;
    if (peer->fd >= 0)
      close(peer->fd);
    while ((block = peer->head) != NULL) {
      peer->head = block->next;
      cai_free_mem(NULL, block);
    }
    cai_wire_encoder_close(peer->encoder);
    cai_wire_decoder_close(peer->decoder);
    cai_sink_close(peer->sink);
    cai_free_mem(NULL, peer);
  }
}
