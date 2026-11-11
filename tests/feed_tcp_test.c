/** Raw utility mode bypasses CAI startup and distinguishes EOF from errors. */
#define _POSIX_C_SOURCE 200809L
#include "../cli/feed_tcp.h"
#include <arpa/inet.h>
#include <assert.h>
#include <cai/network.h>
#include <errno.h>
#include <lc/lc.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct fixture {
  cai_net *listener;
  unsigned char bytes[128];
  size_t length;
  int reset;
} fixture;
static int input_mode, reads;
static const unsigned char binary[] = {0U, 0xffU, 'a', '\n', 0x80U, 1U};
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int, void *, size_t);
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
ssize_t __wrap_read(int fd, void *buffer, size_t capacity) {
  if (fd != STDIN_FILENO || !input_mode)
    return __real_read(fd, buffer, capacity);
  if (reads++ == 0) {
    assert(capacity >= sizeof(binary));
    memcpy(buffer, binary, sizeof(binary));
    return sizeof(binary);
  }
  if (input_mode == 2) {
    errno = EIO;
    return -1;
  }
  return 0;
}
int __wrap_lc_client_open(const lc_client_config *c, lc_client **out,
                          lc_error *e) {
  (void)c;
  (void)out;
  (void)e;
  assert(!"feeder must never open storage");
  return LC_ERR_INVALID;
}
static void *receive(void *context) {
  fixture *f = context;
  struct pollfd fd;
  cai_net *peer = NULL;
  int done = 0;
  fd.fd = cai_net_fd(f->listener);
  fd.events = POLLIN;
  assert(poll(&fd, 1U, 5000) == 1);
  assert(cai_net_accept(f->listener, &peer, NULL) == CAI_OK && peer != NULL);
  fd.fd = cai_net_fd(peer);
  while (!done) {
    ssize_t count;
    assert(poll(&fd, 1U, 5000) == 1);
    count = recv(fd.fd, f->bytes + f->length, sizeof(f->bytes) - f->length, 0);
    if (count > 0)
      f->length += (size_t)count;
    else if (count == 0)
      done = 1;
    else if (errno != EAGAIN && errno != EINTR) {
      assert(errno == ECONNRESET);
      f->reset = done = 1;
    }
  }
  cai_net_close(peer, 0);
  return NULL;
}
static unsigned int port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  unsigned int result;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
  result = ntohs(address.sin_port);
  close(fd);
  return result;
}
static void *run(void *unused) {
  cai_net_config config;
  fixture f;
  pthread_t receiver;
  char address[64], program[] = "cai", flag[] = "--feed-tcp";
  char ordinary_flag[] = "-i";
  char *arguments[3];
  int mode;
  (void)unused;
  arguments[0] = program;
  arguments[1] = ordinary_flag;
  arguments[2] = flag;
  assert(cai_cli_feed_tcp(3, arguments) == -1);
  arguments[1] = flag;
  arguments[2] = address;
  for (mode = 1; mode <= 2; mode++) {
    snprintf(address, sizeof(address), "%u", port());
    cai_net_config_init(&config);
    config.address = address;
    memset(&f, 0, sizeof(f));
    assert(cai_net_open(&config, &f.listener, NULL) == CAI_OK);
    assert(pthread_create(&receiver, NULL, receive, &f) == 0);
    input_mode = mode;
    reads = 0;
    assert(cai_cli_feed_tcp(3, arguments) == (mode == 1 ? 0 : 1));
    assert(pthread_join(receiver, NULL) == 0);
    input_mode = 0;
    if (mode == 1)
      assert(f.length == sizeof(binary) &&
             !memcmp(f.bytes, binary, sizeof(binary)) && !f.reset);
    else
      assert(f.reset);
    cai_net_close(f.listener, 0);
  }
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
