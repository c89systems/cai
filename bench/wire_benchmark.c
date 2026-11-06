/** @file Opt-in codec/transport measurements. No inference or session storage.
 * Socket endpoints are separate processes; setup and teardown are untimed. */
#define _POSIX_C_SOURCE 200809L
#include "benchmark.h"
#include "benchmark_build.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bench_counts bench_io;
int bench_output_fd = -1;
uint64_t bench_output_calls, bench_output_bytes;
static pid_t receiver_pid = -1;
ssize_t __real_send(int, const void *, size_t, int);
ssize_t __real_recv(int, void *, size_t, int);
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_send(int, const void *, size_t, int);
ssize_t __wrap_recv(int, void *, size_t, int);
ssize_t __wrap_write(int, const void *, size_t);
ssize_t __wrap_send(int fd, const void *bytes, size_t length, int flags) {
  ssize_t n;
  ++bench_io.sends;
  n = __real_send(fd, bytes, length, flags);
  if (n > 0)
    bench_io.sent += (uint64_t)n;
  return n;
}
ssize_t __wrap_recv(int fd, void *bytes, size_t length, int flags) {
  ssize_t n;
  ++bench_io.receives;
  n = __real_recv(fd, bytes, length, flags);
  if (n > 0)
    bench_io.received += (uint64_t)n;
  return n;
}
ssize_t __wrap_write(int fd, const void *bytes, size_t length) {
  ssize_t n = __real_write(fd, bytes, length);
  if (fd == bench_output_fd) {
    ++bench_output_calls;
    if (n > 0)
      bench_output_bytes += (uint64_t)n;
  }
  return n;
}
void bench_require(int condition, const char *operation) {
  if (!condition) {
    fprintf(stderr, "benchmark: %s (errno=%d: %s)\n", operation, errno,
            strerror(errno));
    if (receiver_pid > 0) {
      (void)kill(receiver_pid, SIGTERM);
      while (waitpid(receiver_pid, NULL, 0) < 0 && errno == EINTR) {
      }
    }
    exit(1);
  }
}
static double wall_time(void) {
  struct timespec clock;
  bench_require(clock_gettime(CLOCK_MONOTONIC, &clock) == 0,
                "read monotonic clock");
  return (double)clock.tv_sec + (double)clock.tv_nsec / 1e9;
}
bench_clock bench_time(void) {
  bench_clock out;
  struct rusage usage;
  out.wall = wall_time();
  bench_require(getrusage(RUSAGE_SELF, &usage) == 0, "read measurement clocks");
  out.user =
      (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6;
  out.system =
      (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
  return out;
}
void bench_wait(int fd, short events) {
  struct pollfd p;
  int n;
  p.fd = fd;
  p.events = events;
  p.revents = 0;
  do {
    ++bench_io.polls;
    n = poll(&p, 1U, 5000);
  } while (n < 0 && errno == EINTR);
  bench_require(n > 0 && !(p.revents & (POLLERR | POLLNVAL)),
                "socket progress deadline");
}
void bench_transfer(int fd, void *bytes, size_t length, int writing) {
  unsigned char *at = bytes;
  while (length != 0U) {
    ssize_t n = writing ? write(fd, at, length) : read(fd, at, length);
    if (n < 0 && errno == EINTR)
      continue;
    bench_require(n > 0, "benchmark control pipe");
    at += (size_t)n;
    length -= (size_t)n;
  }
}
static void nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL);
  bench_require(flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
                "nonblocking socket");
}

/** One explicitly bounded pre-encoded fixture for decode/socket-only cases.
 * Production measurements never gather the logical stream into this buffer. */
typedef struct fixture {
  unsigned char *payload, *encoded;
  const unsigned char *reference;
  size_t length, encoded_length, used;
  uint64_t bytes, records;
  int verify, text, ended;
  cai_cbor_encoder *encoder;
  cai_cbor_decoder *decoder;
  cai_wire_encoder *wire_encoder;
  cai_wire_decoder *wire_decoder;
  cai_sink *sink;
} fixture;
static int count_token(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  fixture *f = context;
  (void)error;
  if (token->type == CAI_CBOR_DATA) {
    if (f->verify) {
      size_t offset = (size_t)(f->bytes % f->length);
      bench_require(
          token->length <= f->length - offset &&
              memcmp(token->data, f->payload + offset, token->length) == 0,
          "decoded payload integrity");
    }
    f->bytes += token->length;
  } else if (token->type == CAI_CBOR_END) {
    ++f->records;
  }
  return CAI_OK;
}
static int count_wire(void *context, const cai_wire_event *event,
                      cai_error *error) {
  return event->type == CAI_WIRE_TOKEN
             ? count_token(context, event->token, error)
             : CAI_OK;
}
static int memory_sink(void *context, const void *bytes, size_t length,
                       cai_error *error) {
  fixture *f = context;
  if (f->decoder != NULL)
    return cai_cbor_decoder_feed(f->decoder, bytes, length, error);
  if (f->wire_decoder != NULL)
    return cai_wire_decoder_feed(f->wire_decoder, bytes, length, error);
  if (f->encoded != NULL) {
    bench_require(length <= f->length + 16U - f->used,
                  "bounded encoded fixture");
    memcpy(f->encoded + f->used, bytes, length);
  } else if (f->verify && f->reference != NULL) {
    size_t offset = f->used % f->encoded_length;
    bench_require(length <= f->encoded_length - offset &&
                      memcmp(bytes, f->reference + offset, length) == 0,
                  "encoded payload integrity");
  }
  f->used += length;
  return CAI_OK;
}
static void emit(fixture *f, cai_wire_peer *peer) {
  cai_cbor_token t;
  cai_cbor_type type = f->text ? CAI_CBOR_TEXT : CAI_CBOR_BYTES;
  unsigned int i;
  memset(&t, 0, sizeof(t));
  for (i = 0U; i < 3U; ++i) {
    int rc;
    t.type = i == 0U ? type : i == 1U ? CAI_CBOR_DATA : CAI_CBOR_END;
    t.argument = i == 0U ? f->length : i == 2U ? (uint64_t)type : 0U;
    t.data = i == 1U ? f->payload : NULL;
    t.length = i == 1U ? f->length : 0U;
    rc = peer != NULL ? cai_wire_peer_write(peer, &t, NULL)
         : f->wire_encoder != NULL
             ? cai_wire_encoder_write(f->wire_encoder, &t, NULL)
             : cai_cbor_encoder_write(f->encoder, &t, NULL);
    bench_require(rc == CAI_OK, "encode benchmark value");
  }
}
static void fixture_open(fixture *f, size_t length, int text, int verify) {
  static const char corpus[] =
      "The **benchmark** measures `streaming` text with bounded chunks.\n\n";
  cai_sink_callbacks callbacks;
  size_t i;
  if (length == 0U || length > 32768U) {
    fputs("benchmark: fixture size must be 1..32768 bytes\n", stderr);
    exit(2);
  }
  memset(f, 0, sizeof(*f));
  f->length = length;
  f->text = text;
  f->verify = verify;
  f->payload = malloc(length);
  f->encoded = malloc(length + 16U);
  bench_require(f->payload && f->encoded, "allocate bounded fixture");
  for (i = 0U; i < length; ++i)
    f->payload[i] = (unsigned char)corpus[i % (sizeof(corpus) - 1U)];
  f->payload[length - 1U] = '\n';
  if (text == 2) {
    for (i = 0U; i + 1U < length - 1U; i += 2U) {
      f->payload[i] = 0xc3U;
      f->payload[i + 1U] = 0xa9U;
    }
    for (; i + 1U < length; ++i)
      f->payload[i] = ' ';
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = f;
  callbacks.write = memory_sink;
  bench_require(cai_sink_from_callbacks(&callbacks, &f->sink, NULL) == CAI_OK &&
                    cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK,
                "open fixture encoder");
  emit(f, NULL);
  bench_require(cai_cbor_encoder_finish(f->encoder, NULL) == CAI_OK,
                "finish fixture");
  f->encoded_length = f->used;
  f->used = 0U;
}
static void fixture_close(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->decoder);
  cai_wire_encoder_close(f->wire_encoder);
  cai_wire_decoder_close(f->wire_decoder);
  cai_sink_close(f->sink);
  free(f->encoded);
  free(f->payload);
}
typedef enum mode {
  ENCODE,
  DECODE,
  ROUNDTRIP,
  FRAMED,
  RAW,
  PEER,
  RAW_RTT,
  PEER_RTT,
  TRANSCRIPT,
  RENDER
} mode;
typedef struct scenario {
  const char *name;
  mode operation;
  int text;
} scenario;
static const scenario cases[] = {
    {"cbor_encode_bytes", ENCODE, 0},
    {"cbor_encode_text", ENCODE, 1},
    {"cbor_decode_bytes", DECODE, 0},
    {"cbor_decode_text", DECODE, 1},
    {"cbor_roundtrip_text", ROUNDTRIP, 1},
    {"cbor_roundtrip_utf8", ROUNDTRIP, 2},
    {"framed_memory", FRAMED, 1},
    {"unix_raw", RAW, 1},
    {"wire_peer_bytes", PEER, 0},
    {"wire_peer_text", PEER, 1},
    {"unix_rtt", RAW_RTT, 1},
    {"wire_rtt", PEER_RTT, 1},
#ifdef CAI_BENCH_FRONTEND
    {"transcript_peer", TRANSCRIPT, 1},
    {"transcript_render", RENDER, 1},
#endif
};
static void memory_run(const scenario *s, const bench_options *o,
                       bench_result *result) {
  fixture f;
  unsigned char *saved;
  bench_clock start, end;
  uint64_t i, records = (o->bytes + o->message_bytes - 1U) / o->message_bytes;
  fixture_open(&f, o->message_bytes, s->text, o->verify);
  saved = f.encoded;
  f.reference = saved;
  f.encoded = NULL;
  if (s->operation == DECODE || s->operation == ROUNDTRIP)
    bench_require(cai_cbor_decoder_open(count_token, &f, &f.decoder, NULL) ==
                      CAI_OK,
                  "open decoder");
  if (s->operation == FRAMED)
    bench_require(
        cai_wire_decoder_open(count_wire, &f, &f.wire_decoder, NULL) ==
                CAI_OK &&
            cai_wire_encoder_open(f.sink, &f.wire_encoder, NULL) == CAI_OK,
        "open memory framing");
  bench_require(cai_cbor_encoder_reset(f.encoder, NULL) == CAI_OK,
                "reset fixture encoder");
  start = bench_time();
  for (i = 0U; i < records; ++i) {
    if (s->operation == DECODE) {
      bench_require(cai_cbor_decoder_feed(f.decoder, saved, f.encoded_length,
                                          NULL) == CAI_OK,
                    "decode fixture");
    } else {
      if (s->operation == FRAMED)
        bench_require(cai_wire_encoder_begin(f.wire_encoder,
                                             (uint32_t)f.encoded_length,
                                             NULL) == CAI_OK,
                      "begin memory frame");
      emit(&f, NULL);
      bench_require(s->operation == FRAMED
                        ? cai_wire_encoder_end(f.wire_encoder, NULL) == CAI_OK
                        : cai_cbor_encoder_finish(f.encoder, NULL) == CAI_OK,
                    "finish memory encoding");
      if (s->operation != FRAMED)
        bench_require(cai_cbor_encoder_reset(f.encoder, NULL) == CAI_OK,
                      "reuse encoder");
    }
    if (f.decoder != NULL)
      bench_require(cai_cbor_decoder_finish(f.decoder, NULL) == CAI_OK &&
                        cai_cbor_decoder_reset(f.decoder, NULL) == CAI_OK,
                    "accept decoded value");
  }
  if (f.wire_decoder != NULL)
    bench_require(cai_wire_decoder_finish(f.wire_decoder, NULL) == CAI_OK,
                  "finish memory stream");
  end = bench_time();
  bench_require(
      s->operation == ENCODE
          ? f.used == records * f.encoded_length
          : (f.bytes == records * o->message_bytes && f.records == records),
      "complete memory workload");
  result->wall = end.wall - start.wall;
  result->user = end.user - start.user;
  result->system = end.system - start.system;
  result->records = records;
  result->payload = records * o->message_bytes;
  result->encoded =
      records * (f.encoded_length + (s->operation == FRAMED ? 4U : 0U));
  f.encoded = saved;
  fixture_close(&f);
}

/** Raw baseline uses identical pre-encoded framed bytes and the production
 * peer's 4 KiB send / 2 KiB receive quanta. It performs no CBOR parsing. */
static void socket_bytes(int fd, void *data, size_t length, int writing) {
  unsigned char *at = data;
  while (length != 0U) {
    size_t take =
        length < (writing ? 4096U : 2048U) ? length : (writing ? 4096U : 2048U);
    ssize_t n =
        writing ? send(fd, at, take, MSG_NOSIGNAL) : recv(fd, at, take, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      bench_wait(fd, writing ? POLLOUT : POLLIN);
      continue;
    }
    bench_require(n > 0, "raw socket transfer");
    at += (size_t)n;
    length -= (size_t)n;
  }
}
typedef struct receiving {
  fixture f;
  cai_wire_peer *peer;
  int acknowledge;
} receiving;
static int receive_peer(void *context, const cai_wire_event *event,
                        cai_error *error) {
  receiving *r = context;
  int rc = count_wire(&r->f, event, error);
  if (rc == CAI_OK && event->type == CAI_WIRE_END && r->acknowledge) {
    cai_cbor_token t;
    memset(&t, 0, sizeof(t));
    t.type = CAI_CBOR_NULL;
    rc = cai_wire_peer_begin(r->peer, 1U, error);
    if (rc == CAI_OK)
      rc = cai_wire_peer_write(r->peer, &t, error);
    if (rc == CAI_OK)
      rc = cai_wire_peer_end(r->peer, error);
  }
  return rc;
}
static int acknowledgement(void *context, const cai_wire_event *event,
                           cai_error *error) {
  uint64_t *count = context;
  (void)error;
  if (event->type == CAI_WIRE_END)
    ++*count;
  return CAI_OK;
}
static void flush(cai_wire_peer *peer) {
  while (cai_wire_peer_pending(peer) != 0U) {
    size_t before = cai_wire_peer_pending(peer);
    bench_require(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK, "flush peer");
    if (cai_wire_peer_pending(peer) == before)
      bench_wait(cai_wire_peer_fd(peer), POLLOUT);
  }
}
typedef struct child_result {
  bench_counts io;
  double user, system;
  uint64_t output_calls, output_bytes;
} child_result;
static void receiver(int socket, int control, const scenario *s,
                     const bench_options *o, const unsigned char *frame,
                     size_t frame_bytes, uint64_t records) {
  receiving r;
  child_result result;
  bench_clock start, end;
  char ready = 'R';
  unsigned char *buffer;
  uint64_t i;
  int eof;
#ifdef CAI_BENCH_FRONTEND
  bench_frontend *frontend = NULL;
#endif
  memset(&r, 0, sizeof(r));
  fixture_open(&r.f, o->message_bytes, s->text, o->verify);
  buffer = malloc(frame_bytes > 2048U ? frame_bytes : 2048U);
  bench_require(buffer != NULL, "allocate raw receive fixture");
  if (s->operation == PEER || s->operation == PEER_RTT) {
    bench_require(cai_wire_peer_open(socket, 0U, receive_peer, &r, &r.peer,
                                     NULL) == CAI_OK,
                  "open receiving peer");
    r.acknowledge = s->operation == PEER_RTT;
  }
#ifdef CAI_BENCH_FRONTEND
  if (s->operation == TRANSCRIPT || s->operation == RENDER) {
    bench_require(bench_frontend_open(s->operation == RENDER, o->verify,
                                      r.f.payload, r.f.length,
                                      &frontend) == CAI_OK,
                  "open transcript consumer");
    bench_require(cai_wire_peer_open(socket, 0U, bench_frontend_receive,
                                     frontend, &r.peer, NULL) == CAI_OK,
                  "open transcript peer");
  }
#endif
  bench_transfer(control, &ready, 1U, 1);
  bench_transfer(control, &ready, 1U, 0);
  memset(&bench_io, 0, sizeof(bench_io));
  bench_output_calls = bench_output_bytes = 0U;
  start = bench_time();
  if (r.peer == NULL) {
    uint64_t remaining = records * frame_bytes, received = 0U;
    for (i = 0U; remaining != 0U; ++i) {
      static unsigned char ack[] = {0, 0, 0, 1, 0xf6};
      size_t take = s->operation == RAW_RTT ? frame_bytes
                    : remaining < 2048U     ? (size_t)remaining
                                            : 2048U;
      size_t offset = 0U;
      if (s->operation != RAW_RTT) {
        ssize_t n = recv(socket, buffer, take, 0);
        if (n < 0 && errno == EINTR)
          continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          bench_wait(socket, POLLIN);
          continue;
        }
        bench_require(n > 0, "raw stream receive");
        take = (size_t)n;
      } else {
        socket_bytes(socket, buffer, frame_bytes, 0);
      }
      while (o->verify && offset < take) {
        size_t at = (size_t)((received + offset) % frame_bytes);
        size_t count = frame_bytes - at;
        if (count > take - offset)
          count = take - offset;
        bench_require(memcmp(buffer + offset, frame + at, count) == 0,
                      "raw payload integrity");
        offset += count;
      }
      received += take;
      remaining -= take;
      if (s->operation == RAW_RTT)
        socket_bytes(socket, ack, sizeof(ack), 1);
    }
  } else {
    for (;;) {
      bench_require(cai_wire_peer_read(r.peer, 0U, &eof, NULL) == CAI_OK,
                    "receive framed stream");
      if (r.acknowledge)
        flush(r.peer);
      if ((s->operation == PEER || s->operation == PEER_RTT) &&
          r.f.records == records)
        break;
      if (eof)
        break;
      bench_wait(socket, POLLIN);
    }
#ifdef CAI_BENCH_FRONTEND
    if (frontend != NULL)
      bench_frontend_finish(frontend, records);
    else
#endif
      bench_require(r.f.records == records &&
                        r.f.bytes == records * o->message_bytes,
                    "complete socket workload");
  }
  end = bench_time();
  result.io = bench_io;
  result.user = end.user - start.user;
  result.system = end.system - start.system;
  result.output_calls = bench_output_calls;
  result.output_bytes = bench_output_bytes;
  bench_transfer(control, &result, sizeof(result), 1);
#ifdef CAI_BENCH_FRONTEND
  bench_frontend_close(frontend);
#endif
  cai_wire_peer_close(r.peer);
  if (r.peer == NULL)
    close(socket);
  fixture_close(&r.f);
  free(buffer);
  close(control);
  _exit(0);
}
static int compare_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
static void socket_run(const scenario *s, const bench_options *o,
                       bench_result *result) {
  int sockets[2], controls[2], status, eof;
  fixture f;
  cai_wire_peer *peer = NULL;
  unsigned char *frame, ack[5];
  double *latencies = NULL;
  child_result child;
  bench_clock start, end;
  char ready;
  uint64_t i, acknowledgements = 0U;
  int rtt = s->operation == RAW_RTT || s->operation == PEER_RTT;
  uint64_t records =
      rtt ? o->iterations
          : (o->bytes + o->message_bytes - 1U) / o->message_bytes;
  fixture_open(&f, o->message_bytes, s->text, o->verify);
  frame = malloc(f.encoded_length + 4U);
  bench_require(frame != NULL, "allocate framed fixture");
  frame[0] = (unsigned char)(f.encoded_length >> 24U);
  frame[1] = (unsigned char)(f.encoded_length >> 16U);
  frame[2] = (unsigned char)(f.encoded_length >> 8U);
  frame[3] = (unsigned char)f.encoded_length;
  memcpy(frame + 4U, f.encoded, f.encoded_length);
  if (rtt) {
    latencies = malloc((size_t)records * sizeof(*latencies));
    bench_require(latencies != NULL, "allocate bounded RTT samples");
  }
  bench_require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0 &&
                    socketpair(AF_UNIX, SOCK_STREAM, 0, controls) == 0,
                "create connected endpoints");
  nonblocking(sockets[0]);
  nonblocking(sockets[1]);
  receiver_pid = fork();
  bench_require(receiver_pid >= 0, "fork receiver");
  if (receiver_pid == 0) {
    receiver_pid = -1;
    close(sockets[0]);
    close(controls[0]);
    receiver(sockets[1], controls[1], s, o, frame, f.encoded_length + 4U,
             records);
  }
  close(sockets[1]);
  close(controls[1]);
  if (s->operation != RAW && s->operation != RAW_RTT)
    bench_require(cai_wire_peer_open(sockets[0], 0U, acknowledgement,
                                     &acknowledgements, &peer, NULL) == CAI_OK,
                  "open sending peer");
  bench_transfer(controls[0], &ready, 1U, 0);
  bench_require(ready == 'R', "receiver ready");
  memset(&bench_io, 0, sizeof(bench_io));
  start = bench_time();
  bench_transfer(controls[0], &ready, 1U, 1);
  for (i = 0U; i < records; ++i) {
    double began = rtt ? wall_time() : 0.0;
    if (peer == NULL)
      socket_bytes(sockets[0], frame, f.encoded_length + 4U, 1);
    else {
#ifdef CAI_BENCH_FRONTEND
      if (s->operation == TRANSCRIPT || s->operation == RENDER)
        bench_frontend_send(peer, i + 1U, f.payload, f.length);
      else
#endif
      {
        bench_require(cai_wire_peer_begin(peer, (uint32_t)f.encoded_length,
                                          NULL) == CAI_OK,
                      "begin wire frame");
        emit(&f, peer);
        bench_require(cai_wire_peer_end(peer, NULL) == CAI_OK,
                      "end wire frame");
      }
      /* One complete record per flush for both raw and framed comparisons. */
      flush(peer);
    }
    if (rtt) {
      if (peer == NULL)
        socket_bytes(sockets[0], ack, sizeof(ack), 0);
      else
        while (acknowledgements != i + 1U) {
          bench_require(cai_wire_peer_read(peer, 0U, &eof, NULL) == CAI_OK &&
                            (!eof || acknowledgements == i + 1U),
                        "receive acknowledgement");
          if (acknowledgements != i + 1U)
            bench_wait(sockets[0], POLLIN);
        }
      latencies[i] = (wall_time() - began) * 1e6;
    }
  }
  bench_require(shutdown(sockets[0], SHUT_WR) == 0, "finish sending");
  bench_transfer(controls[0], &child, sizeof(child), 0);
  end = bench_time();
  result->parent = bench_io;
  result->child = child.io;
  result->wall = end.wall - start.wall;
  result->user = end.user - start.user;
  result->system = end.system - start.system;
  result->child_user = child.user;
  result->child_system = child.system;
  result->records = records;
  result->payload = records * o->message_bytes;
  result->encoded = result->parent.sent;
  if (rtt) {
    qsort(latencies, (size_t)records, sizeof(*latencies), compare_double);
    result->p50 = latencies[(records - 1U) / 2U];
    result->p95 = latencies[(records * 95U + 99U) / 100U - 1U];
  }
  bench_require(waitpid(receiver_pid, &status, 0) == receiver_pid &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "receiver completed successfully");
  receiver_pid = -1;
  if (peer != NULL)
    cai_wire_peer_close(peer);
  else
    close(sockets[0]);
  close(controls[0]);
  /* Renderer output counters are reported independently of socket bytes. */
  bench_output_calls = child.output_calls;
  bench_output_bytes = child.output_bytes;
  fixture_close(&f);
  free(frame);
  free(latencies);
}

static unsigned long number(const char *text, unsigned long maximum) {
  char *end;
  unsigned long n;
  bench_require(text && *text && strspn(text, "0123456789") == strlen(text),
                "expected an unsigned decimal argument");
  errno = 0;
  n = strtoul(text, &end, 10);
  bench_require(!errno && !*end && n <= maximum, "numeric argument range");
  return n;
}
static void json_string(const char *text) {
  const unsigned char *p = (const unsigned char *)text;
  putchar('"');
  while (*p) {
    if (*p == '"' || *p == '\\')
      printf("\\%c", *p);
    else if (*p < 32U)
      printf("\\u%04x", (unsigned int)*p);
    else
      putchar(*p);
    ++p;
  }
  putchar('"');
}
static void metadata(const bench_options *o) {
  struct utsname system;
  char model[256] = "unknown", line[512];
  FILE *cpu = fopen("/proc/cpuinfo", "r");
  bench_require(uname(&system) == 0, "host identity");
  while (cpu != NULL && fgets(line, sizeof(line), cpu) != NULL)
    if (strncmp(line, "model name", 10U) == 0) {
      char *colon = strchr(line, ':');
      if (colon != NULL) {
        colon += strspn(colon + 1U, " \t") + 1U;
        colon[strcspn(colon, "\r\n")] = '\0';
        snprintf(model, sizeof(model), "%s", colon);
      }
      break;
    }
  if (cpu != NULL)
    fclose(cpu);
  printf("{\"kind\":\"metadata\",\"target\":");
  json_string(CAI_BENCH_TARGET);
  printf(",\"build\":");
  json_string(CAI_BENCH_BUILD);
  printf(",\"compiler\":");
  json_string(CAI_BENCH_COMPILER);
  printf(",\"kernel\":");
  json_string(system.release);
  printf(",\"machine\":");
  json_string(system.machine);
  printf(",\"cpu\":");
  json_string(model);
  printf(",\"message_bytes\":%lu,\"requested_bytes\":%lu,\"iterations\":%u,"
         "\"rounds\":%u,\"warmups\":%u,\"verify\":%s}\n",
         (unsigned long)o->message_bytes, (unsigned long)o->bytes,
         o->iterations, o->rounds, o->warmups, o->verify ? "true" : "false");
}
static void report(const scenario *s, const bench_options *o,
                   unsigned int round, const bench_result *r) {
  bench_require(r->wall > 0.0 && r->payload > 0U && r->records > 0U,
                "nonempty timed measurement");
  printf("{\"kind\":\"sample\",\"case\":\"%s\",\"round\":%u,"
         "\"message_bytes\":%lu,\"payload_bytes\":%llu,\"records\":%llu,"
         "\"wire_bytes\":%llu,\"wall_s\":%.9f,\"sender_user_s\":%.9f,"
         "\"sender_system_s\":%.9f,\"receiver_user_s\":%.9f,"
         "\"receiver_system_s\":%.9f,\"mib_s\":%.6f,\"records_s\":%.3f,"
         "\"cpu_pct\":%.3f,\"rtt_p50_us\":%.6f,\"rtt_p95_us\":%.6f,"
         "\"send_calls\":%llu,\"recv_calls\":%llu,\"poll_calls\":%llu,"
         "\"output_calls\":%llu,\"output_bytes\":%llu}\n",
         s->name, round, (unsigned long)o->message_bytes,
         (unsigned long long)r->payload, (unsigned long long)r->records,
         (unsigned long long)r->encoded, r->wall, r->user, r->system,
         r->child_user, r->child_system,
         (double)r->payload / 1048576.0 / r->wall, (double)r->records / r->wall,
         100.0 * (r->user + r->system + r->child_user + r->child_system) /
             r->wall,
         r->p50, r->p95, (unsigned long long)(r->parent.sends + r->child.sends),
         (unsigned long long)(r->parent.receives + r->child.receives),
         (unsigned long long)(r->parent.polls + r->child.polls),
         (unsigned long long)bench_output_calls,
         (unsigned long long)bench_output_bytes);
}
int main(int argc, char **argv) {
  bench_options options;
  size_t i, selected = 0U;
  int arg;
  memset(&options, 0, sizeof(options));
  options.bytes = 8U * 1024U * 1024U;
  options.message_bytes = 4096U;
  options.rounds = 5U;
  options.warmups = 1U;
  options.iterations = 2000U;
  for (arg = 1; arg < argc; ++arg) {
    const char *flag = argv[arg];
    if (strcmp(flag, "--verify") == 0) {
      options.verify = 1;
      continue;
    }
    if (strcmp(flag, "--list") == 0) {
      for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i)
        puts(cases[i].name);
      return ferror(stdout) ? 1 : 0;
    }
    if (strcmp(flag, "--help") == 0) {
      puts("Usage: cai_benchmark [--rounds N] [--warmups N] [--bytes N] "
           "[--message-bytes 1..32768] [--iterations N] [--case NAME] "
           "[--verify]\n"
           "Outputs JSONL metadata and raw samples; --list lists available "
           "cases.");
      return 0;
    }
    bench_require(arg + 1 < argc, "option requires a value");
    if (strcmp(flag, "--case") == 0)
      options.only = argv[++arg];
    else if (strcmp(flag, "--rounds") == 0)
      options.rounds = (unsigned int)number(argv[++arg], 100U);
    else if (strcmp(flag, "--warmups") == 0)
      options.warmups = (unsigned int)number(argv[++arg], 100U);
    else if (strcmp(flag, "--bytes") == 0)
      options.bytes = number(argv[++arg], 1073741824UL);
    else if (strcmp(flag, "--message-bytes") == 0)
      options.message_bytes = number(argv[++arg], 32768U);
    else if (strcmp(flag, "--iterations") == 0)
      options.iterations = (unsigned int)number(argv[++arg], 1000000U);
    else
      bench_require(0, "unknown benchmark option");
  }
  bench_require(options.rounds && options.bytes && options.message_bytes &&
                    options.iterations,
                "rounds, bytes, message size and iterations must be positive");
  metadata(&options);
  for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    unsigned int round;
    if (options.only != NULL && strcmp(options.only, cases[i].name) != 0)
      continue;
    ++selected;
    for (round = 0U; round < options.warmups + options.rounds; ++round) {
      bench_result result;
      memset(&result, 0, sizeof(result));
      bench_output_calls = bench_output_bytes = 0U;
      if (cases[i].operation <= FRAMED)
        memory_run(&cases[i], &options, &result);
      else
        socket_run(&cases[i], &options, &result);
      if (round >= options.warmups)
        report(&cases[i], &options, round - options.warmups + 1U, &result);
      bench_require(!ferror(stdout), "write benchmark report");
    }
  }
  bench_require(selected != 0U && fflush(stdout) == 0,
                "selected benchmark/output");
  return 0;
}
