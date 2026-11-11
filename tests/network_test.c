#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <assert.h>
#include <cai/network.h>
#include <cai/utf8.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned int unused_port(void) {
  struct sockaddr_in address;
  socklen_t size = sizeof(address);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  unsigned int port;
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
  port = ntohs(address.sin_port);
  close(fd);
  return port;
}
static int readable(cai_net *net, int timeout) {
  struct pollfd fd;
  fd.fd = cai_net_fd(net);
  fd.events = POLLIN;
  return poll(&fd, 1U, timeout);
}
static void unicode(void) {
  static const unsigned char valid[] =
      "ascii\000\302\242\342\202\254\360\237\230\200";
  static const unsigned char invalid[][5] = {{0xc0, 0x80},
                                             {0xed, 0xa0, 0x80},
                                             {0xf4, 0x90, 0x80, 0x80},
                                             {0xe0, 0x80, 0x80},
                                             {0x80},
                                             {0xc2, 'a'}};
  static const size_t lengths[] = {2U, 3U, 4U, 3U, 1U, 2U};
  size_t split, i;
  for (split = 0U; split <= sizeof(valid) - 1U; split++) {
    cai_utf8_state state = {0};
    assert(cai_utf8_feed(&state, valid, split));
    assert(cai_utf8_feed(&state, valid + split, sizeof(valid) - 1U - split));
    assert(cai_utf8_complete(&state));
  }
  for (i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    cai_utf8_state state = {0};
    size_t j;
    for (j = 0U; j < lengths[i]; j++)
      (void)cai_utf8_feed(&state, invalid[i] + j, 1U);
    assert(!cai_utf8_complete(&state));
    assert(!cai_utf8_feed(&state, "valid", 5U));
  }
  {
    cai_utf8_state state = {0};
    assert(cai_utf8_feed(&state, "\xf0\x9f", 2U));
    assert(!cai_utf8_complete(&state));
  }
}
static void sockets(void) {
  cai_net_config config;
  cai_net *listener = NULL, *sender = NULL, *peer = NULL;
  cai_error error;
  char address[64], bytes[32];
  size_t sent = 0U;
  static const unsigned char binary[] = {0U, 0xffU, 1U, 2U, 0x80U};
  cai_error_init(&error);
  cai_net_config_init(&config);
  snprintf(address, sizeof(address), "%u", unused_port());
  config.address = address;
  assert(cai_net_open(&config, &listener, &error) == CAI_OK);
  assert(!strncmp(cai_net_local(listener), "127.0.0.1:", 10U));
  assert(fcntl(cai_net_fd(listener), F_GETFL) & O_NONBLOCK);
  assert(fcntl(cai_net_fd(listener), F_GETFD) & FD_CLOEXEC);
  assert(cai_net_accept(listener, &peer, &error) == CAI_OK && peer == NULL);
  config.mode = CAI_NET_TCP_CONNECT;
  assert(cai_net_open(&config, &sender, &error) == CAI_OK);
  assert(readable(listener, 1000) == 1);
  assert(cai_net_accept(listener, &peer, &error) == CAI_OK && peer != NULL);
  assert(cai_net_connected(sender, &error) == CAI_OK);
  assert(cai_net_send(sender, binary, sizeof(binary), &sent, &error) == CAI_OK);
  assert(sent == sizeof(binary));
  assert(readable(peer, 1000) == 1);
  assert(recv(cai_net_fd(peer), bytes, sizeof(bytes), 0) ==
         (ssize_t)sizeof(binary));
  assert(!memcmp(bytes, binary, sizeof(binary)));
  assert(shutdown(cai_net_fd(sender), SHUT_WR) == 0);
  assert(readable(peer, 1000) == 1);
  assert(recv(cai_net_fd(peer), bytes, sizeof(bytes), 0) == 0);
  cai_net_close(sender, 0);
  cai_net_close(peer, 0);
  cai_net_close(listener, 0);
  config.mode = CAI_NET_MULTICAST_SEND;
  config.address = "127.0.0.1:49189";
  assert(cai_net_open(&config, &sender, &error) == CAI_ERR_INVALID);
  assert(sender == NULL);
  config.address = "239.193.89.1:49189";
  config.interface_name = "definitely-not-an-interface";
  assert(cai_net_open(&config, &sender, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
}
static void multicast(void) {
  cai_net_config config;
  cai_net *a = NULL, *b = NULL, *sender = NULL;
  cai_error error;
  char first[64], second[64], bytes[8];
  size_t sent;
  unsigned int port = unused_port();
  cai_error_init(&error);
  snprintf(first, sizeof(first), "239.193.89.201:%u", port);
  snprintf(second, sizeof(second), "239.193.89.202:%u", port);
  cai_net_config_init(&config);
  config.address = first;
  config.interface_name = "127.0.0.1";
  config.mode = CAI_NET_MULTICAST_RECEIVE;
  assert(cai_net_open(&config, &a, &error) == CAI_OK);
  config.address = second;
  assert(cai_net_open(&config, &b, &error) == CAI_OK);
  config.mode = CAI_NET_MULTICAST_SEND;
  config.address = first;
  assert(cai_net_open(&config, &sender, &error) == CAI_OK);
  assert(cai_net_send(sender, "A", 1U, &sent, &error) == CAI_OK && sent == 1U);
  assert(readable(a, 1000) == 1);
  assert(recv(cai_net_fd(a), bytes, sizeof(bytes), 0) == 1 && bytes[0] == 'A');
  assert(readable(b, 20) == 0);
  cai_net_close(sender, 0);
  config.address = second;
  assert(cai_net_open(&config, &sender, &error) == CAI_OK);
  assert(cai_net_send(sender, "B", 1U, &sent, &error) == CAI_OK && sent == 1U);
  assert(readable(b, 1000) == 1);
  assert(recv(cai_net_fd(b), bytes, sizeof(bytes), 0) == 1 && bytes[0] == 'B');
  assert(readable(a, 20) == 0);
  cai_net_close(sender, 0);
  cai_net_close(a, 0);
  cai_net_close(b, 0);
  cai_error_cleanup(&error);
}
static void ipv6(void) {
  cai_net_config config;
  cai_net *listener = NULL, *sender = NULL, *peer = NULL;
  struct sockaddr_in6 address;
  socklen_t length = sizeof(address);
  char endpoint_text[64];
  int fd = socket(AF_INET6, SOCK_STREAM, 0), only = 0;
  socklen_t option_size = sizeof(only);
  /* A system with IPv6 disabled still reports an actionable transport error.
   * Enabled systems exercise name resolution and the IPv6-only listener. */
  if (fd < 0)
    return;
  memset(&address, 0, sizeof(address));
  address.sin6_family = AF_INET6;
  address.sin6_addr = in6addr_loopback;
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return;
  }
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  snprintf(endpoint_text, sizeof(endpoint_text), "localhost:%u",
           ntohs(address.sin6_port));
  close(fd);
  cai_net_config_init(&config);
  config.address = endpoint_text;
  config.ipv6 = 1;
  assert(cai_net_open(&config, &listener, NULL) == CAI_OK);
  assert(!strncmp(cai_net_local(listener), "[::1]:", 6U));
  assert(getsockopt(cai_net_fd(listener), IPPROTO_IPV6, IPV6_V6ONLY, &only,
                    &option_size) == 0 &&
         only);
  config.mode = CAI_NET_TCP_CONNECT;
  assert(cai_net_open(&config, &sender, NULL) == CAI_OK);
  assert(readable(listener, 1000) == 1);
  assert(cai_net_accept(listener, &peer, NULL) == CAI_OK && peer != NULL);
  assert(cai_net_connected(sender, NULL) == CAI_OK);
  cai_net_close(sender, 0);
  cai_net_close(peer, 0);
  cai_net_close(listener, 0);
  config.address = "[::1]:0";
  assert(cai_net_open(&config, &sender, NULL) == CAI_ERR_INVALID &&
         sender == NULL);
}
static void event_source(void) {
  cai_agent_session_event event;
  cai_source *source = NULL;
  cai_error error;
  char bytes[256], one;
  size_t count = 0U;
  const char *expected =
      "{\"record_type\":\"event\",\"sequence\":42,\"timestamp_ns\":123,\"utc_"
      "offset_seconds\":3600,\"type\":\"assistant_text_delta\",\"data\":"
      "\"line\\n\\\"\\\\\"}";
  memset(&event, 0, sizeof(event));
  event.sequence = 42U;
  event.timestamp_ns = 123U;
  event.utc_offset_seconds = 3600;
  event.type = "assistant_text_delta";
  event.data = "line\n\"\\";
  cai_error_init(&error);
  assert(cai_session_event_source(&event, &source, &error) == CAI_OK);
  while (cai_source_read(source, &one, 1U, &error) != 0U) {
    assert(count < sizeof(bytes) - 1U);
    bytes[count++] = one;
  }
  bytes[count] = '\0';
  assert(error.code == CAI_OK);
  assert(!strcmp(bytes, expected));
  cai_source_close(source);
  cai_error_cleanup(&error);
}
static void *worker(void *unused) {
  (void)unused;
  unicode();
  sockets();
  ipv6();
  multicast();
  event_source();
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  if (argc == 3 && !strcmp(argv[1], "reset-fd")) {
    struct linger abortive;
    int fd = atoi(argv[2]);
    abortive.l_onoff = 1;
    abortive.l_linger = 0;
    assert(fd > 2 && setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive,
                                sizeof(abortive)) == 0);
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "port")) {
    printf("%u\n", unused_port());
    return 0;
  }
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
