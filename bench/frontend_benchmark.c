/** @file Production transcript codec and presenter on a controlled null sink.
 * No Pouch, runtime, editor draft, provider, or complete replay is constructed.
 */
#include "benchmark.h"
#include "transcript_presenter.h"
#include "transcript_wire.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct bench_frontend {
  cai_protocol_decoder *protocol;
  cai_cli_transcript_decoder *transcript;
  cai_cli_presenter *ui;
  cai_cli_transcript_presenter *presenter;
  const unsigned char *expected;
  size_t length, offset;
  uint64_t records;
  int verify, text, render;
};
/** Count the same decided Markdown emissions in both probes. Mode 2 discards
 * body output here; mode 1 runs the real CAI/Softline/OS output path. */
static int renderer_sink(void *context, const char *bytes, size_t length) {
  bench_frontend *f = context;
  ++bench_mdf_calls;
  bench_mdf_bytes += length;
  return f->render == 2 ? 0
                        : cai_cli_presenter_renderer_sink(f->ui, bytes, length);
}
/** Benchmark-only null Softline method. The CAI sink still does its native
 * geometry checks and feed bookkeeping; output stops at this boundary. */
static int discard_softline(sl_t *self, const char *bytes, size_t length) {
  (void)self;
  (void)bytes;
  (void)length;
  return SL_OK;
}
static int protocol_event(void *context, const cai_protocol_event *event,
                          cai_error *error) {
  bench_frontend *f = context;
  const cai_cli_transcript_message *m = NULL;
  int rc = cai_cli_transcript_decode(f->transcript, event, &m, error);
  if (rc != CAI_OK || m == NULL)
    return rc;
  if (m->event.kind == CAI_TRANSCRIPT_EVENT_BEGIN) {
    f->text = strcmp(m->event.type, "assistant_text_delta") == 0;
    f->offset = 0U;
  } else if (m->event.kind == CAI_TRANSCRIPT_DATA) {
    bench_require(f->text && m->event.offset == f->offset &&
                      m->event.length <= f->length - f->offset,
                  "transcript data boundaries");
    bench_require(!f->verify || memcmp(m->event.data, f->expected + f->offset,
                                       m->event.length) == 0,
                  "transcript payload integrity");
    f->offset += m->event.length;
  } else if (m->event.kind == CAI_TRANSCRIPT_EVENT_END && f->text) {
    bench_require(f->offset == f->length, "complete transcript body");
    ++f->records;
  }
  return f->presenter != NULL ? cai_cli_transcript_presenter_event(
                                    f->presenter, &m->event, error)
                              : CAI_OK;
}
int bench_frontend_open(int render, int verify, const void *expected,
                        size_t length, bench_frontend **out) {
  bench_frontend *f = calloc(1U, sizeof(*f));
  sl_config_t config;
  mdf_sink sink;
  bench_require(f != NULL, "allocate bounded frontend");
  f->expected = expected;
  f->length = length;
  f->verify = verify;
  f->render = render;
  bench_require(
      cai_cli_transcript_decoder_open(&f->transcript, NULL) == CAI_OK &&
          cai_protocol_decoder_open(protocol_event, f, &f->protocol, NULL) ==
              CAI_OK,
      "open typed transcript decoder");
  if (render) {
    f->ui = calloc(1U, sizeof(*f->ui));
    bench_output_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    bench_require(f->ui != NULL && bench_output_fd >= 0,
                  "open presentation sink");
    /* The native presenter also probes STDOUT for geometry. Isolate the
     * receiver from the caller's terminal so width stays exactly 80 columns. */
    bench_require(dup2(bench_output_fd, STDOUT_FILENO) == STDOUT_FILENO,
                  "isolate presentation geometry");
    sl_config_init(&config);
    config.output_fd = bench_output_fd;
    config.screen_width = 80;
    f->ui->sl = sl_create_with_config(&config);
    f->ui->interactive = 1;
    f->ui->utc = 1;
    f->ui->width = 80;
    bench_require(f->ui->sl != NULL &&
                      cai_cli_theme_parse("plain", &f->ui->theme) == NULL,
                  "create controlled Softline frontend");
    f->ui->render_theme = f->ui->theme;
    bench_require(cai_cli_presenter_renderer_create(f->ui, &f->ui->render_theme,
                                                    &f->ui->renderer,
                                                    NULL) == CAI_OK &&
                      sl_output_stream_begin(f->ui->sl) == SL_OK &&
                      cai_cli_transcript_presenter_open(f->ui, &f->presenter,
                                                        NULL) == CAI_OK,
                  "open output stream and presenter");
    memset(&sink, 0, sizeof(sink));
    sink.userdata = f;
    sink.write = renderer_sink;
    bench_require(f->ui->renderer->set_sink(f->ui->renderer, &sink) == MDF_OK,
                  "bind measured renderer sink");
    if (render == 3)
      f->ui->sl->output_stream_write = discard_softline;
  }
  *out = f;
  return CAI_OK;
}
int bench_frontend_receive(void *context, const cai_wire_event *event,
                           cai_error *error) {
  bench_frontend *f = context;
  return cai_protocol_decoder_event(f->protocol, event, error);
}
static int output_begin(void *context, const cai_protocol_header *header,
                        uint32_t length, cai_error *error) {
  return cai_protocol_peer_begin(context, header, length, error);
}
static int output_write(void *context, const cai_cbor_token *token,
                        cai_error *error) {
  return cai_wire_peer_write(context, token, error);
}
static int output_end(void *context, cai_error *error) {
  return cai_protocol_peer_end(context, error);
}
void bench_frontend_send(cai_wire_peer *peer, uint64_t sequence,
                         const void *bytes, size_t length) {
  cai_cli_transcript_message m;
  cai_cli_input_wire_output output;
  unsigned int event;
  size_t offset;
  memset(&output, 0, sizeof(output));
  output.context = peer;
  output.begin = output_begin;
  output.write = output_write;
  output.end = output_end;
  for (event = 0U; event < 2U; ++event) {
    memset(&m, 0, sizeof(m));
    m.header.kind = CAI_PROTOCOL_STREAM_BEGIN;
    m.header.request = m.header.conversation = m.header.generation =
        m.header.stream = m.header.attachment = 1U;
    m.header.sequence = 2U * sequence - 1U + event;
    m.event.type = event == 0U ? "assistant_text_delta" : "assistant_text_end";
    m.event.has_data = event == 0U;
    m.event.timestamp_ns = (uint64_t)time(NULL) * UINT64_C(1000000000);
    bench_require(cai_cli_transcript_write(&output, &m, NULL) == CAI_OK,
                  "write transcript begin");
    offset = 0U;
    while (event == 0U && offset < length) {
      size_t take = length - offset;
      if (take > CAI_CLI_TRANSCRIPT_CHUNK)
        take = CAI_CLI_TRANSCRIPT_CHUNK;
      m.header.kind = CAI_PROTOCOL_STREAM_CHUNK;
      m.event.offset = offset;
      m.event.data = (const unsigned char *)bytes + offset;
      m.event.length = take;
      bench_require(cai_cli_transcript_write(&output, &m, NULL) == CAI_OK,
                    "write transcript chunk");
      offset += take;
    }
    m.header.kind = CAI_PROTOCOL_STREAM_END;
    m.event.position = m.header.sequence;
    m.event.offset = offset;
    bench_require(cai_cli_transcript_write(&output, &m, NULL) == CAI_OK,
                  "write transcript end");
  }
}
void bench_frontend_finish(bench_frontend *f, uint64_t records) {
  bench_require(
      f->records == records &&
          (f->ui == NULL || (cai_cli_presenter_finish_response(f->ui) == 0 &&
                             !f->ui->sink_failed)),
      "complete transcript presentation");
  if (f->ui != NULL)
    bench_require(
        (f->length == 1U || (bench_mdf_calls > 0U && bench_mdf_bytes > 0U)) &&
            f->ui->width == 80 &&
            (f->render == 3
                 ? bench_output_calls == 0U
                 : bench_output_bytes > 0U && bench_output_calls > 0U),
        "complete measured renderer boundary");
}
void bench_frontend_close(bench_frontend *f) {
  if (f == NULL)
    return;
  cai_cli_transcript_presenter_close(f->presenter);
  if (f->ui != NULL) {
    sl_t *editor = f->ui->sl;
    cai_cli_presenter_close(f->ui);
    bench_require(sl_output_stream_end(editor) == SL_OK, "end output stream");
    sl_destroy(editor);
    close(bench_output_fd);
    bench_output_fd = -1;
    free(f->ui);
  }
  cai_protocol_decoder_close(f->protocol);
  cai_cli_transcript_decoder_close(f->transcript);
  free(f);
}
