#include "../cli/network_wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
typedef struct fixture {
  cai_cli_network_decoder *network;
  cai_cbor_decoder *decoder;
  cai_cbor_encoder *encoder;
  cai_sink *sink;
  cai_protocol_header header;
  const cai_app_network_event *row;
  size_t bytes, declared, writes;
  int fail;
} fixture;
static int receive(void *context, const cai_cbor_token *token,
                   cai_error *error) {
  fixture *f = context;
  const cai_app_network_event *row = NULL;
  cai_protocol_event event;
  int rc;
  event.type = CAI_PROTOCOL_PAYLOAD;
  event.header = &f->header;
  event.token = token;
  rc = cai_cli_network_decode(f->network, &event, &row, error);
  assert(row == NULL);
  return rc;
}
static int bytes(void *context, const void *data, size_t length,
                 cai_error *error) {
  fixture *f = context;
  const unsigned char *p = data;
  size_t i;
  f->writes++;
  if (f->fail && f->writes == 3U)
    return CAI_ERR_CANCELLED;
  f->bytes += length;
  for (i = 0U; i < length; i++) {
    int rc = cai_cbor_decoder_feed(f->decoder, p + i, 1U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int begin(void *context, const cai_protocol_header *header,
                 uint32_t length, cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  f->header = *header;
  f->declared = length;
  event.type = CAI_PROTOCOL_HEADER;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_network_decode(f->network, &event, &f->row, error);
}
static int token(void *context, const cai_cbor_token *t, cai_error *error) {
  return cai_cbor_encoder_write(((fixture *)context)->encoder, t, error);
}
static int end(void *context, cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  int rc = cai_cbor_encoder_finish(f->encoder, error);
  if (rc == CAI_OK)
    rc = cai_cbor_decoder_finish(f->decoder, error);
  if (rc != CAI_OK)
    return rc;
  assert(f->bytes == f->declared);
  event.type = CAI_PROTOCOL_COMPLETE;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_network_decode(f->network, &event, &f->row, error);
}
static void open_fixture(fixture *f) {
  cai_sink_callbacks cb;
  memset(f, 0, sizeof(*f));
  memset(&cb, 0, sizeof(cb));
  cb.context = f;
  cb.write = bytes;
  assert(cai_cli_network_decoder_open(&f->network, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(receive, f, &f->decoder, NULL) == CAI_OK);
  assert(cai_sink_from_callbacks(&cb, &f->sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->decoder);
  cai_cli_network_decoder_close(f->network);
  cai_sink_close(f->sink);
}
static int send(fixture *f, const cai_app_network_event *event) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = token;
  out.end = end;
  return cai_cli_network_write(&out, event, NULL);
}
static void *run(void *unused) {
  fixture f;
  cai_app_network_event event;
  char *prompt = malloc(CAI_CLI_INPUT_TEXT_MAX + 2U);
  int operation;
  (void)unused;
  assert(prompt != NULL);
  memset(prompt, 'x', CAI_CLI_INPUT_TEXT_MAX);
  prompt[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  for (operation = 0; operation <= CAI_APP_NETWORK_NOTICE; operation++) {
    open_fixture(&f);
    memset(&event, 0, sizeof(event));
    event.header.kind = CAI_PROTOCOL_NETWORK;
    event.header.attachment = 123U;
    event.header.generation = 456U;
    event.header.request = UINT64_MAX;
    event.header.conversation = 91U;
    event.operation = (cai_app_network_operation)operation;
    event.timeout_ms = 60000U;
    event.transferred = UINT64_MAX;
    event.code = CAI_ERR_CANCELLED;
    event.flags = 7U;
    event.address = "[::1]:1234";
    event.advertise = "localhost:1234";
    event.prompt = prompt;
    event.reason = "\342\202\254 fragmented text";
    assert(send(&f, &event) == CAI_OK && f.row != NULL);
    assert(f.row->operation == event.operation &&
           f.row->header.request == UINT64_MAX);
    assert(f.row->header.generation == event.header.generation &&
           f.row->header.attachment == event.header.attachment);
    assert(f.row->header.conversation == event.header.conversation);
    assert(!strcmp(f.row->prompt, prompt) &&
           !strcmp(f.row->reason, event.reason));
    assert(f.row->transferred == UINT64_MAX && f.row->flags == 7U);
    close_fixture(&f);
  }
  open_fixture(&f);
  f.fail = 1;
  assert(send(&f, &event) == CAI_ERR_CANCELLED);
  close_fixture(&f);
  prompt[CAI_CLI_INPUT_TEXT_MAX] = 'x';
  prompt[CAI_CLI_INPUT_TEXT_MAX + 1U] = '\0';
  open_fixture(&f);
  assert(send(&f, &event) == CAI_ERR_PROTOCOL && f.bytes == 0U);
  close_fixture(&f);
  event.prompt = "\300\200";
  open_fixture(&f);
  assert(send(&f, &event) == CAI_ERR_PROTOCOL && f.bytes == 0U);
  close_fixture(&f);
  event.prompt = "valid";
  event.operation = (cai_app_network_operation)-1;
  open_fixture(&f);
  assert(send(&f, &event) == CAI_ERR_PROTOCOL && f.bytes == 0U);
  close_fixture(&f);
  event.operation = CAI_APP_NETWORK_SEND;
  event.flags = 8U;
  open_fixture(&f);
  assert(send(&f, &event) == CAI_ERR_PROTOCOL && f.bytes == 0U);
  close_fixture(&f);
  event.flags = 0U;
  event.header.kind = CAI_PROTOCOL_SUBSCRIBE;
  open_fixture(&f);
  assert(send(&f, &event) == CAI_ERR_PROTOCOL && f.bytes == 0U);
  close_fixture(&f);
  free(prompt);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
