/** @file Raw feeder: bounded bytes, normal EOF commit, abort on transfer
 * errors. */
#define _POSIX_C_SOURCE 200809L
#include "feed_tcp.h"
#include "log.h"
#include <cai/network.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int wait_socket(cai_net *net, cai_error *error) {
  struct pollfd fd;
  int ready;
  fd.fd = cai_net_fd(net);
  fd.events = POLLOUT;
  do {
    ready = poll(&fd, 1U, -1);
  } while (ready < 0 && errno == EINTR);
  if (ready < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for TCP transfer");
  return cai_net_connected(net, error);
}
int cai_cli_feed_tcp(int argc, char **argv) {
  cai_net_config config;
  cai_net *net = NULL;
  cai_cli_log *log = NULL;
  cai_error error;
  char bytes[2048];
  size_t offset, sent;
  ssize_t count;
  unsigned long long total = 0U;
  int i, rc = CAI_OK;
  if (argc < 2 || (strcmp(argv[1], "--feed-tcp") != 0 &&
                   !(argc > 3 && strcmp(argv[1], "--proto") == 0 &&
                     strcmp(argv[3], "--feed-tcp") == 0)))
    return -1;
  cai_error_init(&error);
  cai_net_config_init(&config);
  config.mode = CAI_NET_TCP_CONNECT;
  for (i = 1; i < argc && rc == CAI_OK; i++) {
    if (i + 1 < argc && strcmp(argv[i], "--feed-tcp") == 0 &&
        config.address == NULL)
      config.address = argv[++i];
    else if (i + 1 < argc && strcmp(argv[i], "--proto") == 0) {
      ++i;
      if (strcmp(argv[i], "tcpv4") && strcmp(argv[i], "tcpv6"))
        rc = cai_cli_error(&error, CAI_ERR_INVALID,
                           "--proto requires tcpv4 or tcpv6");
      config.ipv6 = strcmp(argv[i], "tcpv6") == 0;
    } else
      rc = cai_cli_error(&error, CAI_ERR_INVALID,
                         "usage: --feed-tcp ADDRESS [--proto tcpv4|tcpv6]");
  }
  if (cai_cli_log_new(&log, PSLOG_LEVEL_INFO, 0, NULL, &error) != CAI_OK) {
    cai_error_cleanup(&error);
    return 1;
  }
  if (rc == CAI_OK)
    rc = cai_net_open(&config, &net, &error);
  if (rc == CAI_OK)
    rc = wait_socket(net, &error);
  if (rc == CAI_OK) {
    pslog_field fields[1];
    fields[0] = pslog_str("destination", cai_net_remote(net));
    log->logger->info(log->logger, "TCP feeder connected", fields, 1U);
  }
  while (rc == CAI_OK) {
    do {
      count = read(STDIN_FILENO, bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
      rc = cai_cli_error(&error, CAI_ERR_TRANSPORT, "read TCP feeder stdin");
      break;
    }
    if (count == 0)
      break;
    for (offset = 0U; offset < (size_t)count && rc == CAI_OK;) {
      rc = cai_net_send(net, bytes + offset, (size_t)count - offset, &sent,
                        &error);
      offset += sent;
      total += sent;
      if (rc == CAI_OK && sent == 0U)
        rc = wait_socket(net, &error);
    }
  }
  cai_net_close(net, rc != CAI_OK);
  if (rc != CAI_OK)
    cai_cli_log_error(log->logger, "feed TCP prompt", &error);
  else {
    pslog_field fields[1];
    fields[0] = pslog_u64("bytes", total);
    log->logger->info(log->logger,
                      "TCP bytes transmitted; remote admission is unconfirmed",
                      fields, 1U);
  }
  (void)cai_cli_log_release(log);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
