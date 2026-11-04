#include "../cli/catalog_wire.h"
#include "../cli/command_help.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
typedef struct fixture {
  cai_cli_catalog_decoder *catalog;
  cai_cbor_decoder *decoder;
  cai_cbor_encoder *encoder;
  cai_sink *sink;
  cai_protocol_header header;
  const cai_app_catalog_event *row;
  size_t bytes, declared, writes;
  int fail;
} fixture;
static int receive(void *context, const cai_cbor_token *token,
                   cai_error *error) {
  fixture *f = context;
  const cai_app_catalog_event *row = NULL;
  cai_protocol_event event;
  int rc;
  event.type = CAI_PROTOCOL_PAYLOAD;
  event.header = &f->header;
  event.token = token;
  rc = cai_cli_catalog_decode(f->catalog, &event, &row, error);
  assert(row == NULL);
  return rc;
}
static int bytes(void *context, const void *data, size_t length,
                 cai_error *error) {
  fixture *f = context;
  const unsigned char *p = data;
  size_t i;
  ++f->writes;
  if (f->fail && f->writes == 3U)
    return CAI_ERR_CANCELLED;
  f->bytes += length;
  for (i = 0U; i < length; ++i) {
    int rc = cai_cbor_decoder_feed(f->decoder, p + i, 1U, error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int begin(void *context, const cai_protocol_header *h, uint32_t n,
                 cai_error *error) {
  fixture *f = context;
  cai_protocol_event event;
  f->header = *h;
  f->declared = n;
  event.type = CAI_PROTOCOL_HEADER;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_catalog_decode(f->catalog, &event, &f->row, error);
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
  assert(f->bytes == f->declared && f->row == NULL);
  event.type = CAI_PROTOCOL_COMPLETE;
  event.header = &f->header;
  event.token = NULL;
  return cai_cli_catalog_decode(f->catalog, &event, &f->row, error);
}
static void open_fixture(fixture *f) {
  cai_sink_callbacks sink;
  memset(f, 0, sizeof(*f));
  memset(&sink, 0, sizeof(sink));
  sink.write = bytes;
  sink.context = f;
  assert(cai_cli_catalog_decoder_open(&f->catalog, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(receive, f, &f->decoder, NULL) == CAI_OK);
  assert(cai_sink_from_callbacks(&sink, &f->sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->decoder);
  cai_cli_catalog_decoder_close(f->catalog);
  cai_sink_close(f->sink);
}
static int send(fixture *f, const cai_app_catalog_event *row) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = token;
  out.end = end;
  return cai_cli_catalog_write(&out, row, NULL);
}
typedef struct help_check {
  size_t chunks;
  int keys, invocation, connections, fail;
} help_check;
static int help_chunk(void *context, const char *bytes, size_t length) {
  help_check *out = context;
  assert(length != 0U);
  ++out->chunks;
  if (out->fail && out->chunks == 3U)
    return -1;
  out->keys |= strstr(bytes, "Queue and approval keys") != NULL;
  out->invocation |= strstr(bytes, "other-client --help") != NULL;
  out->connections |= strstr(bytes, "Workspace connections") != NULL;
  return 0;
}
static void *run(void *unused) {
  fixture *f = malloc(sizeof(*f));
  cai_app_catalog_event row;
  char longest[65], too_long[66], reason[192];
  int phase, scenario;
  (void)unused;
  assert(f != NULL);
  memset(longest, 'x', sizeof(longest) - 1U);
  longest[64] = '\0';
  memset(too_long, 'x', sizeof(too_long) - 1U);
  too_long[65] = '\0';
  memset(reason, 'r', sizeof(reason) - 1U);
  reason[191] = '\0';
  for (phase = 0; phase <= CAI_APP_CATALOG_ERROR; ++phase) {
    open_fixture(f);
    memset(&row, 0, sizeof(row));
    row.phase = (cai_app_catalog_phase)phase;
    row.revision = UINT64_MAX;
    row.conversation = UINT64_MAX;
    row.index = phase == CAI_APP_CATALOG_BEGIN ? 0U : 256U;
    row.text = phase == CAI_APP_CATALOG_ITEM    ? longest
               : phase == CAI_APP_CATALOG_ERROR ? reason
                                                : "";
    row.code = phase == CAI_APP_CATALOG_ERROR ? CAI_ERR_LIMIT : CAI_OK;
    assert(send(f, &row) == CAI_OK && f->row != NULL);
    assert(f->row->phase == row.phase && f->row->index == row.index &&
           f->row->revision == row.revision &&
           f->row->conversation == row.conversation &&
           strcmp(f->row->text, row.text) == 0 && f->row->code == row.code);
    close_fixture(f);
  }
  for (scenario = 0; scenario < 7; ++scenario) {
    open_fixture(f);
    memset(&row, 0, sizeof(row));
    row.revision = row.conversation = 1U;
    row.phase = CAI_APP_CATALOG_ITEM;
    row.text = "r\xc3\xa4ksm\xc3\xb6rg\xc3\xa5s";
    if (scenario == 1)
      row.text = too_long;
    if (scenario == 2)
      row.revision = 0U;
    if (scenario == 3)
      row.conversation = 0U;
    if (scenario == 4)
      row.code = CAI_ERR_LIMIT;
    if (scenario == 5)
      row.text = "";
    if (scenario == 6)
      f->fail = 1;
    assert(send(f, &row) == (scenario == 0   ? CAI_OK
                             : scenario == 6 ? CAI_ERR_CANCELLED
                                             : CAI_ERR_PROTOCOL));
    assert((f->row != NULL) == (scenario == 0));
    close_fixture(f);
  }
  for (scenario = 0; scenario < 3; ++scenario) {
    help_check h;
    memset(&h, 0, sizeof(h));
    h.fail = scenario == 2;
    assert(
        cai_cli_command_help("other-client", scenario == 1, help_chunk, &h) ==
        (scenario == 2 ? CAI_ERR_TRANSPORT : CAI_OK));
    if (scenario != 2)
      assert(h.chunks > 30U && h.keys && h.invocation &&
             h.connections == scenario);
    else
      assert(h.chunks == 3U);
  }
  free(f);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
