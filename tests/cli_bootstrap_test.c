#define _POSIX_C_SOURCE 200809L
#include "../cli/bootstrap.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
typedef struct fixture {
  cai_cli_bootstrap_decoder *decoder;
  cai_cli_bootstrap_message result;
  unsigned int complete;
} fixture;
static int event(void *context, const cai_protocol_event *e, cai_error *error) {
  fixture *f = context;
  const cai_cli_bootstrap_message *value = NULL;
  int rc = cai_cli_bootstrap_decode(f->decoder, e, &value, error);
  if (value != NULL) {
    assert(e->type == CAI_PROTOCOL_COMPLETE);
    f->result = *value;
    f->complete++;
  }
  return rc;
}
static int unused(void *context, const cai_wire_event *e, cai_error *error) {
  (void)context;
  (void)e;
  (void)error;
  abort();
  return CAI_ERR_INVALID;
}
static size_t encode(const cai_cli_bootstrap_message *value,
                     unsigned char *bytes, size_t capacity) {
  cai_wire_peer *peer;
  int pair[2];
  size_t length;
  ssize_t n;
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(cai_wire_peer_open(pair[0], 16384U, unused, NULL, &peer, NULL) ==
         CAI_OK);
  assert(cai_cli_bootstrap_write(peer, value, NULL) == CAI_OK);
  length = cai_wire_peer_pending(peer);
  assert(length < capacity && cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
  n = read(pair[1], bytes, capacity);
  assert(n > 0 && (size_t)n == length);
  close(pair[1]);
  cai_wire_peer_close(peer);
  return length;
}
static void decode(const unsigned char *bytes, size_t length, size_t fragment,
                   int expected, const cai_cli_bootstrap_message *value) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_protocol_decoder *protocol;
  cai_wire_decoder *wire;
  size_t offset;
  int rc = CAI_OK;
  assert(f != NULL);
  assert(cai_cli_bootstrap_decoder_open(&f->decoder, NULL) == CAI_OK);
  assert(cai_protocol_decoder_open(event, f, &protocol, NULL) == CAI_OK);
  assert(cai_wire_decoder_open(cai_protocol_decoder_event, protocol, &wire,
                               NULL) == CAI_OK);
  for (offset = 0U; offset < length && rc == CAI_OK;) {
    size_t take = length - offset < fragment ? length - offset : fragment;
    rc = cai_wire_decoder_feed(wire, bytes + offset, take, NULL);
    offset += take;
    if (offset < length)
      assert(f->complete == 0U);
  }
  if (rc == CAI_OK)
    rc = cai_wire_decoder_finish(wire, NULL);
  assert(rc == expected);
  if (expected == CAI_OK) {
    assert(f->complete == 1U);
    assert(memcmp(&f->result, value, sizeof(*value)) == 0);
  } else
    assert(f->complete == 0U);
  cai_wire_decoder_close(wire);
  cai_protocol_decoder_close(protocol);
  cai_cli_bootstrap_decoder_close(f->decoder);
  free(f);
}
int main(void) {
  cai_cli_bootstrap_message *value = calloc(1U, sizeof(*value));
  cai_app_status *status = calloc(1U, sizeof(*status));
  unsigned char bytes[8192];
  size_t length, i;
  cai_error failure;
  char long_message[2048];
  char recovered[] = "A recovered diagnostic";
  assert(value && status);
  value->ready = value->persist = 1;
  memset(value->instance, 42, sizeof(value->instance));
  strcpy(value->session, "session_123");
  strcpy(value->log_path, "/private/log");
  length = encode(value, bytes, sizeof(bytes));
  for (i = 1U; i <= length; i++)
    decode(bytes, length, i, CAI_OK, value);
  for (i = 1U; i < length; i++)
    decode(bytes, i, 1U, CAI_ERR_PROTOCOL, NULL);
  bytes[4] = 0x87; /* definite seven-item array instead of envelope map */
  decode(bytes, length, length, CAI_ERR_PROTOCOL, NULL);
  length = encode(value, bytes, sizeof(bytes));
  for (i = 0U; i + strlen(value->session) <= length; i++)
    if (memcmp(bytes + i, value->session, strlen(value->session)) == 0) {
      bytes[i] = 0; /* byte strings must not smuggle a C-string suffix */
      break;
    }
  assert(i + strlen(value->session) <= length);
  decode(bytes, length, 1U, CAI_ERR_PROTOCOL, NULL);
  length = encode(value, bytes, sizeof(bytes));
  /* First metadata value (kind) follows key zero; another channel is invalid.
   */
  bytes[6] = 0x17;
  decode(bytes, length, 1U, CAI_ERR_PROTOCOL, NULL);
  value->ready = 0;
  value->exit_code = 255;
  value->code = CAI_ERR_LIMIT;
  value->clipped = 1;
  memset(value->session, 's', sizeof(value->session) - 1U);
  memset(value->log_path, 'p', sizeof(value->log_path) - 1U);
  value->log_path[0] = '/';
  memset(value->message, 'm', sizeof(value->message) - 1U);
  length = encode(value, bytes, sizeof(bytes));
  decode(bytes, length, 1U, CAI_OK, value);
  decode(bytes, length, 4096U, CAI_OK, value);
  cai_error_init(&failure);
  memset(long_message, 'd', sizeof(long_message) - 1U);
  long_message[sizeof(long_message) - 1U] = '\0';
  failure.code = CAI_ERR_TRANSPORT;
  failure.message = long_message; /* borrowed only by status(), not cleanup */
  status->session_id = "saved";
  status->log_path = "/private/session.log";
  status->has_server = status->server.persist = 1;
  memset(status->server_instance, 3, sizeof(status->server_instance));
  assert(cai_cli_bootstrap_status(value, 0, 2, status, &failure, NULL) ==
         CAI_OK);
  assert(value->clipped && strlen(value->message) == 1024U && value->persist);
  assert(strcmp(value->log_path, status->log_path) == 0);
  failure.message = NULL;
  status->outcome = CAI_APP_STOPPED;
  status->error.code = CAI_ERR_TRANSPORT;
  status->error.message = recovered;
  assert(cai_cli_bootstrap_status(value, 0, 0, status, NULL, NULL) == CAI_OK);
  assert(value->code == CAI_OK && !value->message[0]);
  status->outcome = CAI_APP_FAILED;
  assert(cai_cli_bootstrap_status(value, 0, 1, status, NULL, NULL) == CAI_OK);
  assert(value->code == CAI_ERR_TRANSPORT &&
         strcmp(value->message, recovered) == 0);
  failure.code = CAI_ERR_INVALID;
  assert(cai_cli_bootstrap_status(value, 0, 1, status, &failure, NULL) ==
         CAI_OK);
  assert(value->code == CAI_ERR_INVALID);
  status->outcome = CAI_APP_RUNNING;
  status->log_path = "relative";
  assert(cai_cli_bootstrap_status(value, 1, 0, status, NULL, NULL) ==
         CAI_ERR_PROTOCOL);
  free(status);
  free(value);
  return 0;
}
