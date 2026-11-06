/** @file Private measurement contract shared by the benchmark and UI adapter.
 * Metrics are counters, never retained application messages. */
#ifndef CAI_BENCHMARK_H
#define CAI_BENCHMARK_H
#include <cai/wire.h>
#include <stdint.h>
#include <sys/resource.h>

typedef struct bench_options {
  size_t bytes, message_bytes;
  unsigned int rounds, warmups, iterations;
  int verify;
  const char *only;
} bench_options;
typedef struct bench_counts {
  uint64_t sends, receives, sent, received, polls;
} bench_counts;
typedef struct bench_clock {
  double wall, user, system;
} bench_clock;
typedef struct bench_result {
  double wall, user, system, child_user, child_system, p50, p95;
  bench_counts parent, child;
  uint64_t payload, records, encoded;
} bench_result;
extern bench_counts bench_io;
extern int bench_output_fd;
extern uint64_t bench_output_calls, bench_output_bytes;
void bench_require(int condition, const char *operation);
bench_clock bench_time(void);
void bench_wait(int fd, short events);
void bench_transfer(int fd, void *bytes, size_t length, int writing);

#ifdef CAI_BENCH_FRONTEND
typedef struct bench_frontend bench_frontend;
int bench_frontend_open(int render, int verify, const void *expected,
                        size_t length, bench_frontend **out);
int bench_frontend_receive(void *, const cai_wire_event *, cai_error *);
void bench_frontend_send(cai_wire_peer *, uint64_t sequence, const void *,
                         size_t);
void bench_frontend_finish(bench_frontend *, uint64_t records);
void bench_frontend_close(bench_frontend *);
#endif
#endif
