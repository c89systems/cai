#include "../cli/batch_document.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Generate a logical journal twice without retaining its contents. All
 * substantial aggregates live on the heap, including on default musl stacks. */
typedef struct fixture {
  cai_cli_batch_document document;
  cai_cli_batch_document_reader reader;
  cai_execution_record record;
  cai_transcript_position at;
  cai_transcript_event end;
  cai_app_execution_event execution;
  cai_sink *sink;
  uint64_t bytes;
  int reject, reenter;
} fixture;
static int output(void *context, const void *data, size_t n, cai_error *error) {
  fixture *f = context;
  const unsigned char *p = data;
  size_t i;
  (void)error;
  assert(n <= 4096U);
  if (f->reject)
    return CAI_ERR_TRANSPORT;
  if (f->reenter)
    assert(cai_cli_batch_document_event(&f->reader, &f->end, NULL) ==
           CAI_ERR_INVALID);
  for (i = 0U; i < n; ++i)
    assert(p[i] == 'a' + (f->bytes + i) % 26U);
  f->bytes += n;
  return CAI_OK;
}
static void source(fixture *f) {
  memset(&f->at, 0, sizeof(f->at));
  strcpy(f->at.session, "parent");
  strcpy(f->at.collection, "collection");
  f->at.conversation = 3U;
  f->at.stream = 7U;
  f->at.sequence = 5U;
  f->at.position = 11U;
}
static void record(fixture *f, size_t n, uint64_t offset, int final) {
  size_t i;
  memset(&f->record, 0, sizeof(f->record));
  f->record.kind = "review_markdown_delta";
  f->record.session_id = "reviewer";
  f->record.parent_call_id = "command:parent:2:3:4";
  f->record.offset = offset;
  f->record.final = final;
  for (i = 0U; i < n; ++i)
    f->record.data[i] = (char)('a' + (offset + i) % 26U);
  memset(&f->end, 0, sizeof(f->end));
  f->end.kind = CAI_TRANSCRIPT_EVENT_END;
  f->end.stream = f->at.stream;
  f->end.conversation = f->at.conversation;
  f->end.sequence = f->at.sequence + 1U;
  f->end.position = f->at.position + 2U;
  memset(&f->execution, 0, sizeof(f->execution));
  f->execution.boundary = &f->end;
  f->execution.record = &f->record;
  f->execution.subagent = 1;
}
static void accepted(fixture *f) {
  f->at.sequence = f->end.sequence;
  f->at.position = f->end.position;
}
static void initialize(fixture *f) {
  memset(f, 0, sizeof(*f));
  source(f);
  assert(cai_cli_batch_document_init(&f->document, "review_markdown_delta",
                                     "reviewer", NULL, "command:parent:2:3:4",
                                     NULL) == CAI_OK);
}
static void capture(fixture *f, size_t n, uint64_t offset, int final) {
  record(f, n, offset, final);
  assert(cai_cli_batch_document_capture(&f->document, &f->at, &f->execution,
                                        NULL) == CAI_OK);
  accepted(f);
}
static void read_document(fixture *f) {
  cai_sink_callbacks callbacks;
  cai_transcript_event e;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = output;
  callbacks.context = f;
  assert(cai_sink_from_callbacks(&callbacks, &f->sink, NULL) == CAI_OK);
  assert(cai_cli_batch_document_read(&f->reader, &f->document, f->sink, NULL) ==
         CAI_OK);
  memset(&e, 0, sizeof(e));
  e.kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
  e.session = "parent";
  e.collection = "collection";
  e.conversation = f->document.range.after.conversation;
  e.stream = 17U;
  e.watermark = f->document.range.through_position;
  assert(cai_cli_batch_document_event(&f->reader, &e, NULL) == CAI_OK);
}
static void *run(void *unused) {
  fixture *f = calloc(1U, sizeof(*f));
  uint64_t offset, size = 16U * 1024U * 1024U;
  (void)unused;
  assert(f != NULL);
  initialize(f);
  for (offset = 0U; offset < size; offset += 4096U)
    capture(f, 4096U, offset, 0);
  /* Exact chunk multiples end with a zero-byte final record. */
  capture(f, 0U, size, 1);
  assert(cai_cli_batch_document_finish(&f->document, CAI_OK, NULL) == CAI_OK);
  assert(f->document.range.text_bytes == size);
  read_document(f);
  source(f);
  f->at.stream = 17U;
  f->reenter = 1;
  for (offset = 0U; offset <= size; offset += 4096U) {
    record(f, offset < size ? 4096U : 0U, offset, offset == size);
    assert(cai_cli_batch_document_execution(&f->reader, &f->execution, NULL) ==
           CAI_OK);
    assert(cai_cli_batch_document_event(&f->reader, &f->end, NULL) == CAI_OK);
    accepted(f);
  }
  assert(f->reader.done && f->bytes == size);
  cai_sink_close(f->sink);

  initialize(f);
  record(f, 3U, 0U, 1);
  f->record.parent_call_id = "another review";
  assert(cai_cli_batch_document_capture(&f->document, &f->at, &f->execution,
                                        NULL) == CAI_OK);
  f->record.parent_call_id = "command:parent:2:3:4";
  f->record.session_id = "another reviewer";
  assert(cai_cli_batch_document_capture(&f->document, &f->at, &f->execution,
                                        NULL) == CAI_OK);
  assert(f->document.range.text_bytes == 0U);
  capture(f, 3U, 0U, 1);
  assert(cai_cli_batch_document_finish(&f->document, CAI_OK, NULL) == CAI_OK);
  read_document(f);
  source(f);
  f->at.stream = 17U;
  record(f, 3U, 0U, 1);
  f->reject = 1;
  assert(cai_cli_batch_document_execution(&f->reader, &f->execution, NULL) ==
         CAI_ERR_TRANSPORT);
  assert(cai_cli_batch_document_event(&f->reader, &f->end, NULL) ==
         CAI_ERR_PROTOCOL);
  cai_sink_close(f->sink);

  initialize(f);
  capture(f, 3U, 0U, 0);
  assert(cai_cli_batch_document_finish(&f->document, CAI_OK, NULL) ==
         CAI_ERR_PROTOCOL);
  cai_cli_batch_document_reset(&f->document);
  record(f, 3U, 1U, 1);
  assert(cai_cli_batch_document_capture(&f->document, &f->at, &f->execution,
                                        NULL) == CAI_ERR_PROTOCOL);
  initialize(f);
  capture(f, 3U, 0U, 1);
  assert(cai_cli_batch_document_finish(&f->document, CAI_OK, NULL) == CAI_OK);
  read_document(f);
  f->end.kind = CAI_TRANSCRIPT_REPLAY_END;
  f->end.stream = 17U;
  assert(cai_cli_batch_document_event(&f->reader, &f->end, NULL) ==
         CAI_ERR_PROTOCOL);
  cai_sink_close(f->sink);
  free(f);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  puts("batch documents: bounded range projection and failure cases passed");
  return 0;
}
