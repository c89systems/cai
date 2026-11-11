/** @file Asynchronous DNS/connect/send with bounded prompt bytes and receipts.
 */
#define _POSIX_C_SOURCE 200809L
#include "prompt_send.h"
#include "error.h"
#include "input_wire.h"
#include <cai/network.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
struct cai_cli_prompt_send {
  pthread_t worker;
  pthread_mutex_t lock;
  int fd, started, cancelled, done, ipv6;
  cai_app_network_event result;
  char *address, *prompt;
  char reason[256];
  uint64_t deadline;
};
static uint64_t now_ms(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int cancelled(cai_cli_prompt_send *s) {
  int value;
  pthread_mutex_lock(&s->lock);
  value = s->cancelled;
  pthread_mutex_unlock(&s->lock);
  return value;
}
static void *send_prompt(void *context) {
  cai_cli_prompt_send *s = context;
  cai_net_config config;
  cai_net *net = NULL;
  cai_error error;
  struct pollfd fd;
  size_t length = strlen(s->prompt), offset = 0U, sent;
  int rc;
  uint64_t one = 1U;
  cai_error_init(&error);
  cai_net_config_init(&config);
  config.mode = CAI_NET_TCP_CONNECT;
  config.address = s->address;
  config.ipv6 = s->ipv6 || *s->address == '[';
  rc = cai_net_open(&config, &net, &error);
  fd.fd = cai_net_fd(net);
  fd.events = POLLOUT;
  do {
    if (cancelled(s) || now_ms() >= s->deadline) {
      rc = cai_cli_error(&error, CAI_ERR_CANCELLED,
                         "frontend TCP transfer cancelled or deadline reached");
      break;
    }
    if (rc != CAI_OK)
      break;
    rc = poll(&fd, 1U, 20);
    if (rc < 0 && errno != EINTR) {
      rc = cai_cli_error(&error, CAI_ERR_TRANSPORT,
                         "wait for frontend TCP transfer");
      break;
    }
    if (rc <= 0) {
      rc = CAI_OK;
      continue;
    }
    rc = cai_net_connected(net, &error);
    if (rc == CAI_OK)
      rc =
          cai_net_send(net, s->prompt + offset, length - offset, &sent, &error);
    if (rc == CAI_OK)
      offset += sent;
  } while (rc == CAI_OK && offset < length);
  /* Authority may be revoked after the final write but before EOF commit. */
  if (rc == CAI_OK && (cancelled(s) || now_ms() >= s->deadline))
    rc = cai_cli_error(&error, CAI_ERR_CANCELLED,
                       "frontend TCP authority revoked before close");
  cai_net_close(net, rc != CAI_OK);
  pthread_mutex_lock(&s->lock);
  s->result.transferred = offset;
  s->result.code = rc;
  s->result.flags =
      rc != CAI_OK && offset != 0U ? CAI_APP_NETWORK_UNCERTAIN : 0U;
  snprintf(s->reason, sizeof(s->reason), "%s",
           rc == CAI_OK ? "Local TCP transmission completed; remote admission "
                          "and inference are unconfirmed."
           : error.message != NULL ? error.message
                                   : "TCP transfer failed");
  s->result.reason = s->reason;
  s->done = 1;
  pthread_mutex_unlock(&s->lock);
  while (write(s->fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
  cai_error_cleanup(&error);
  return NULL;
}
int cai_cli_prompt_send_open(const cai_app_network_event *request, int ipv6,
                             cai_cli_prompt_send **out, cai_error *error) {
  cai_cli_prompt_send *s;
  *out = NULL;
  if (request == NULL || request->operation != CAI_APP_NETWORK_SEND ||
      request->header.request == 0U || request->prompt == NULL ||
      request->address == NULL ||
      strlen(request->prompt) > CAI_CLI_INPUT_TEXT_MAX || !*request->address ||
      request->timeout_ms == 0U || request->timeout_ms > 60000U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "valid bounded frontend TCP request required");
  s = calloc(1U, sizeof(*s));
  if (s == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate frontend TCP action");
  s->fd = -1;
  s->ipv6 = ipv6;
  if (pthread_mutex_init(&s->lock, NULL) != 0) {
    free(s);
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "initialize TCP action lock");
  }
  s->address = strdup(request->address);
  s->prompt = strdup(request->prompt);
  s->result.header = request->header;
  s->result.operation = CAI_APP_NETWORK_RECEIPT;
  s->deadline = now_ms() + request->timeout_ms;
  s->fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
  if (s->address == NULL || s->prompt == NULL || s->fd < 0 ||
      pthread_create(&s->worker, NULL, send_prompt, s) != 0) {
    cai_cli_prompt_send_close(s);
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "start frontend TCP action");
  }
  s->started = 1;
  *out = s;
  return CAI_OK;
}
int cai_cli_prompt_send_fd(cai_cli_prompt_send *s) {
  return s != NULL ? s->fd : -1;
}
void cai_cli_prompt_send_cancel(cai_cli_prompt_send *s) {
  if (s != NULL) {
    pthread_mutex_lock(&s->lock);
    s->cancelled = 1;
    pthread_mutex_unlock(&s->lock);
  }
}
int cai_cli_prompt_send_result(cai_cli_prompt_send *s,
                               const cai_app_network_event **out) {
  int done = 0;
  *out = NULL;
  if (s != NULL) {
    pthread_mutex_lock(&s->lock);
    done = s->done;
    if (done)
      *out = &s->result;
    pthread_mutex_unlock(&s->lock);
  }
  return done;
}
void cai_cli_prompt_send_close(cai_cli_prompt_send *s) {
  if (s != NULL) {
    cai_cli_prompt_send_cancel(s);
    if (s->started)
      (void)pthread_join(s->worker, NULL);
    if (s->fd >= 0)
      close(s->fd);
    pthread_mutex_destroy(&s->lock);
    free(s->address);
    free(s->prompt);
    free(s);
  }
}
