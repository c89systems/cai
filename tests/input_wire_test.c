#include "../cli/input_wire.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  cai_cli_input_wire_decoder *input;
  cai_cbor_decoder *decoder;
  cai_cbor_encoder *encoder;
  cai_sink *sink;
  cai_protocol_header header;
  const cai_cli_input_wire_message *complete;
  size_t bytes, tokens, declared, writes;
  int fail;
} fixture;
static int receive(void *context, const cai_cbor_token *token,
                   cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_cli_input_wire_message *value = NULL;
  int rc;
  event.type = CAI_PROTOCOL_PAYLOAD;
  event.header = &f->header;
  event.token = token;
  ++f->tokens;
  rc = cai_cli_input_wire_decode(f->input, &event, &value, error);
  assert(value == NULL); /* Provisional tokens never admit a command. */
  return rc;
}
static int bytes(void *context, const void *data, size_t size,
                 cai_error *error) {
  fixture *f = context;
  const unsigned char *at = data;
  size_t i = 0U;
  ++f->writes;
  if (f->fail && f->writes == 3U)
    return CAI_ERR_CANCELLED;
  f->bytes += size;
  /* Exercise field/header/scalar boundaries without collecting the body. */
  while (i < size) {
    size_t n = size - i;
    int rc;
    if (n > 17U)
      n = 17U;
    rc = cai_cbor_decoder_feed(f->decoder, at + i, n, error);
    if (rc != CAI_OK)
      return rc;
    i += n;
  }
  return CAI_OK;
}
static int begin(void *context, const cai_protocol_header *header,
                 uint32_t length, cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_cli_input_wire_message *value = NULL;
  f->header = *header;
  f->declared = length;
  event.type = CAI_PROTOCOL_HEADER;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_input_wire_decode(f->input, &event, &value, error);
}
static int write_token(void *context, const cai_cbor_token *token,
                       cai_error *error) {
  fixture *f = context;
  return cai_cbor_encoder_write(f->encoder, token, error);
}
static int end(void *context, cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  int rc = cai_cbor_encoder_finish(f->encoder, error);
  if (rc == CAI_OK)
    rc = cai_cbor_decoder_finish(f->decoder, error);
  if (rc != CAI_OK)
    return rc;
  assert(f->tokens > 0U && f->bytes == f->declared && f->complete == NULL);
  event.type = CAI_PROTOCOL_COMPLETE;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_input_wire_decode(f->input, &event, &f->complete, error);
}
static void open_fixture(fixture *f) {
  cai_sink_callbacks sink;
  memset(f, 0, sizeof(*f));
  memset(&sink, 0, sizeof(sink));
  sink.context = f;
  sink.write = bytes;
  assert(cai_cli_input_wire_decoder_open(&f->input, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(receive, f, &f->decoder, NULL) == CAI_OK);
  assert(cai_sink_from_callbacks(&sink, &f->sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->decoder);
  cai_cli_input_wire_decoder_close(f->input);
  cai_sink_close(f->sink);
}
static int send(fixture *f, const cai_cli_input_wire_message *message) {
  cai_cli_input_wire_output output;
  output.context = f;
  output.begin = begin;
  output.write = write_token;
  output.end = end;
  return cai_cli_input_wire_write(&output, message, NULL);
}
static void roundtrips(void) {
  static const cai_protocol_kind kinds[] = {
      CAI_PROTOCOL_SYNCHRONIZE,  CAI_PROTOCOL_REQUEST,
      CAI_PROTOCOL_QUEUE_BEGIN,  CAI_PROTOCOL_QUEUE_ITEM,
      CAI_PROTOCOL_QUEUE_END,    CAI_PROTOCOL_QUEUE_CHANGE,
      CAI_PROTOCOL_RESULT,       CAI_PROTOCOL_HISTORY_APPEND,
      CAI_PROTOCOL_INPUT_SETTLED};
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_input_wire_message message;
  char *text = malloc(CAI_CLI_INPUT_TEXT_MAX + 2U);
  size_t i;
  assert(f != NULL && text != NULL);
  memset(text, 'x', CAI_CLI_INPUT_TEXT_MAX);
  text[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  for (i = 0U; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    cai_input_observation *o = &message.observation;
    const cai_cli_input_wire_message *got;
    memset(&message, 0, sizeof(message));
    message.header.kind = kinds[i];
    message.header.request = kinds[i] == CAI_PROTOCOL_INPUT_SETTLED ? 0U : 24U;
    message.header.conversation = 256U;
    message.header.sequence = kinds[i] == CAI_PROTOCOL_REQUEST ||
                                      kinds[i] == CAI_PROTOCOL_HISTORY_APPEND ||
                                      kinds[i] == CAI_PROTOCOL_SYNCHRONIZE ||
                                      kinds[i] == CAI_PROTOCOL_INPUT_SETTLED ||
                                      kinds[i] == CAI_PROTOCOL_RESULT
                                  ? 0U
                                  : 65536U;
    message.session = "session_with_128_byte_capacity";
    message.producer = 19U;
    message.managed_delivery = 1;
    message.delivery_after = ~(unsigned long long)0;
    message.request.operation = CAI_INPUT_OFFER;
    message.request.text = text;
    message.request.intent = CAI_INPUT_STEERING;
    message.request.enabled = 1;
    message.request.item_revision = ~(unsigned long long)0;
    o->producer = 19U;
    o->last_request = ~(unsigned long long)0;
    o->state.count = 128U;
    o->state.revision = 65536U;
    o->state.stopped = o->state.auto_approval = 1;
    o->index = 127U;
    o->entry.id = 65535U;
    o->entry.revision = 4294967296ULL;
    o->entry.client_id = 19U;
    o->entry.request_id = 255U;
    o->entry.edit_owner = 19U;
    o->entry.edit_token = 65536U;
    o->entry.intent = CAI_INPUT_STEERING;
    o->entry.held = 1;
    o->entry.edit_held = o->entry.ready = o->entry.independent = 1;
    strcpy(o->entry.origin, "opaque-origin");
    o->entry.text = text;
    o->change.type = CAI_INPUT_UPSERT;
    o->change.index = 127U;
    o->change.previous_view_revision = 65535U;
    o->change.client_id = 19U;
    o->change.request_id = 23U;
    o->change.entry = o->entry;
    o->result.outcome = CAI_INPUT_REJECTED;
    o->result.code = CAI_ERR_CANCELLED;
    o->result.item_id = 65535U;
    o->result.item_revision = 4294967296ULL;
    o->result.edit_token = 65536U;
    strcpy(o->result.reason, "detailed rejection");
    open_fixture(f);
    assert(send(f, &message) == CAI_OK && f->complete != NULL);
    got = f->complete;
    assert(got->header.kind == message.header.kind);
    if (kinds[i] == CAI_PROTOCOL_SYNCHRONIZE)
      assert(got->managed_delivery == 1 && got->producer == 19U);
    if (kinds[i] == CAI_PROTOCOL_INPUT_SETTLED)
      assert(got->delivery_after == ~(unsigned long long)0);
    if (kinds[i] == CAI_PROTOCOL_REQUEST)
      assert(strcmp(got->request.text, text) == 0 &&
             got->request.item_revision == ~(unsigned long long)0 &&
             got->request.enabled);
    if (kinds[i] == CAI_PROTOCOL_HISTORY_APPEND)
      assert(strcmp(got->request.text, text) == 0);
    if (kinds[i] == CAI_PROTOCOL_QUEUE_BEGIN)
      assert(got->observation.producer == 19U &&
             got->observation.last_request == ~(unsigned long long)0 &&
             strcmp(got->session, message.session) == 0 &&
             got->observation.state.count == 128U);
    if (kinds[i] == CAI_PROTOCOL_QUEUE_ITEM ||
        kinds[i] == CAI_PROTOCOL_QUEUE_CHANGE) {
      const cai_input_entry *entry = kinds[i] == CAI_PROTOCOL_QUEUE_ITEM
                                         ? &got->observation.entry
                                         : &got->observation.change.entry;
      assert(strcmp(entry->text, text) == 0 &&
             entry->revision == 4294967296ULL && entry->edit_token == 65536U &&
             strcmp(entry->origin, "opaque-origin") == 0 &&
             entry->intent == CAI_INPUT_STEERING);
    }
    if (kinds[i] == CAI_PROTOCOL_RESULT)
      assert(got->observation.result.code == CAI_ERR_CANCELLED &&
             strcmp(got->observation.result.reason, "detailed rejection") == 0);
    close_fixture(f);
  }
  memset(&message, 0, sizeof(message));
  message.header.kind = CAI_PROTOCOL_REQUEST;
  message.header.request = 1U;
  message.request.operation = CAI_INPUT_SYNCHRONIZE;
  open_fixture(f);
  assert(send(f, &message) == CAI_OK && f->complete->request.text == NULL);
  close_fixture(f);
  message.request.text = text;
  message.request.operation = CAI_INPUT_OFFER;
  text[CAI_CLI_INPUT_TEXT_MAX] = 'x';
  text[CAI_CLI_INPUT_TEXT_MAX + 1U] = '\0';
  open_fixture(f);
  assert(send(f, &message) == CAI_ERR_PROTOCOL && f->bytes == 0U);
  close_fixture(f);
  text[CAI_CLI_INPUT_TEXT_MAX] = '\0';
  strcpy(message.request.origin, "untrusted-origin");
  open_fixture(f);
  assert(send(f, &message) == CAI_ERR_PROTOCOL && f->bytes == 0U);
  close_fixture(f);
  message.request.origin[0] = '\0';
  open_fixture(f);
  f->fail = 1;
  assert(send(f, &message) == CAI_ERR_CANCELLED && f->complete == NULL);
  close_fixture(f);
  free(text);
  free(f);
}
static void malformed(void) {
  static const unsigned char cases[][20] = {
      {0x88},
      {0x8a},
      {0x9f},
      {0x89, 0x20},
      {0x89, 0x0d, 0, 0, 0, 0, 0, 0xf4, 0, 0xf6},
      {0x89, 0, 0, 0, 0, 0, 0, 0, 0, 0xf6},
      {0x89, 0, 0, 0, 0, 0, 0, 0xf4, 0, 0x61, 'x'},
      {0x89, 0, 0, 0, 0, 0, 0, 0xf4, 0, 0x43, 'a', 0, 'b'},
      {0x89, 0, 0, 0, 0, 0, 0, 0xf4, 0, 0x5a, 0, 1, 2, 1},
      {0x89, 0, 0, 0, 0, 0, 0, 0xf4, 0, 0x80},
      {0x89, 0, 0, 0, 0, 0, 0, 0xf4, 0, 0xf6, 0},
      {0x89, 0, 0, 0}};
  static const size_t sizes[] = {1U,  1U,  1U,  2U,  10U, 10U,
                                 11U, 13U, 14U, 10U, 11U, 4U};
  fixture *f = calloc(1U, sizeof(*f));
  cai_protocol_header header;
  size_t i;
  assert(f != NULL);
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_REQUEST;
  header.request = 1U;
  for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    int rc;
    open_fixture(f);
    assert(begin(f, &header, (uint32_t)sizes[i], NULL) == CAI_OK);
    rc = cai_cbor_decoder_feed(f->decoder, cases[i], sizes[i], NULL);
    if (rc == CAI_OK)
      rc = cai_cbor_decoder_finish(f->decoder, NULL);
    if (rc == CAI_OK) {
      cai_protocol_event event;
      event.type = CAI_PROTOCOL_COMPLETE;
      event.header = &f->header;
      event.token = NULL;
      rc = cai_cli_input_wire_decode(f->input, &event, &f->complete, NULL);
    }
    assert(rc == CAI_ERR_PROTOCOL && f->complete == NULL);
    close_fixture(f);
  }
  open_fixture(f);
  header.request = 0U;
  assert(begin(f, &header, 1U, NULL) == CAI_ERR_PROTOCOL);
  close_fixture(f);
  open_fixture(f);
  header.kind = CAI_PROTOCOL_RESULT;
  header.stream = 0U;
  header.sequence = 1U;
  assert(begin(f, &header, 1U, NULL) == CAI_ERR_PROTOCOL);
  close_fixture(f);
  open_fixture(f);
  header.kind = CAI_PROTOCOL_REQUEST;
  header.sequence = 0U;
  header.request = 1U;
  header.stream = 1U;
  assert(begin(f, &header, 1U, NULL) == CAI_ERR_PROTOCOL);
  close_fixture(f);
  free(f);
}
static void shell_roundtrips(void) {
  fixture *f = calloc(1U, sizeof(*f));
  cai_cli_input_wire_message message;
  unsigned char bytes_with_nul[4096];
  size_t i;
  assert(f != NULL);
  for (i = 0U; i < sizeof(bytes_with_nul); ++i)
    bytes_with_nul[i] = (unsigned char)i;
  memset(&message, 0, sizeof(message));
  message.header.kind = CAI_PROTOCOL_USER_SHELL;
  message.header.request = 17U;
  message.header.conversation = 3U;
  message.header.generation = 7U;
  message.shell.operation = CAI_APP_SHELL_WRITE;
  message.shell.recording = 12U;
  message.shell.sequence = 255U;
  message.shell.data = bytes_with_nul;
  message.shell.length = sizeof(bytes_with_nul);
  open_fixture(f);
  assert(send(f, &message) == CAI_OK && f->complete != NULL);
  assert(f->complete->shell.operation == CAI_APP_SHELL_WRITE &&
         f->complete->shell.recording == 12U &&
         f->complete->shell.sequence == 255U &&
         f->complete->shell.length == sizeof(bytes_with_nul) &&
         memcmp(f->complete->shell.data, bytes_with_nul,
                sizeof(bytes_with_nul)) == 0);
  close_fixture(f);
  message.shell.operation = CAI_APP_SHELL_FINISH;
  message.shell.data = NULL;
  message.shell.length = 0U;
  message.shell.exit_code = 137U;
  message.shell.duration_ms = ~(unsigned long long)0;
  open_fixture(f);
  assert(send(f, &message) == CAI_OK && f->complete != NULL);
  assert(f->complete->shell.exit_code == 137U &&
         f->complete->shell.duration_ms == ~(unsigned long long)0 &&
         f->complete->shell.length == 0U);
  close_fixture(f);
  message.header.request = 0U;
  open_fixture(f);
  assert(send(f, &message) == CAI_ERR_PROTOCOL && f->complete == NULL);
  close_fixture(f);
  free(f);
}
static void *run(void *unused) {
  (void)unused;
  roundtrips();
  malformed();
  shell_roundtrips();
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
