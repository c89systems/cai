#include "batch_document.h"
#include "error.h"
#include <string.h>

static int failure(int *failed, cai_error *error, const char *message) {
  *failed = 1;
  return cai_cli_error(error, CAI_ERR_PROTOCOL, message);
}
static int filter_copy(char *out, size_t capacity, const char *text) {
  size_t n = text != NULL ? strlen(text) : 0U;
  if (n >= capacity)
    return 0;
  if (n != 0U)
    memcpy(out, text, n);
  out[n] = '\0';
  return 1;
}
int cai_cli_batch_document_init(cai_cli_batch_document *d, const char *kind,
                                const char *source, const char *call,
                                const char *parent, cai_error *error) {
  if (d == NULL || kind == NULL || *kind == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID, "document filter is required");
  memset(d, 0, sizeof(*d));
  if (!filter_copy(d->kind, sizeof(d->kind), kind) ||
      !filter_copy(d->source, sizeof(d->source), source) ||
      !filter_copy(d->call, sizeof(d->call), call) ||
      !filter_copy(d->parent, sizeof(d->parent), parent))
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "document filter exceeds its bound");
  return CAI_OK;
}
void cai_cli_batch_document_reset(cai_cli_batch_document *d) {
  memset(&d->range, 0, sizeof(d->range));
  d->offset = 0U;
  d->failed = 0;
}
static int matches(const cai_cli_batch_document *d,
                   const cai_app_execution_event *e) {
  const cai_execution_record *r = e->record;
  if (r == NULL || strcmp(r->kind, d->kind) != 0 ||
      (d->source[0] != '\0' && strcmp(r->session_id, d->source) != 0) ||
      (d->call[0] != '\0' &&
       (r->call_id == NULL || strcmp(r->call_id, d->call) != 0)))
    return 0;
  if (d->parent[0] != '\0')
    return e->subagent && r->parent_call_id != NULL &&
           strcmp(r->parent_call_id, d->parent) == 0;
  return !e->subagent;
}
static int field_offset(uint64_t *offset, const cai_execution_record *r,
                        size_t n) {
  if (r->offset != *offset || n > UINT64_MAX - *offset || n > 4096U)
    return 0;
  *offset = r->final ? 0U : *offset + n;
  return 1;
}
int cai_cli_batch_document_capture(cai_cli_batch_document *d,
                                   const cai_transcript_position *before,
                                   const cai_app_execution_event *e,
                                   cai_error *error) {
  const cai_transcript_event *end = e != NULL ? e->boundary : NULL;
  size_t n;
  if (d == NULL || before == NULL || e == NULL || end == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "accepted document boundary required");
  if (d->failed)
    return failure(&d->failed, error, "document selection has failed");
  if (!matches(d, e))
    return CAI_OK;
  n = strlen(e->record->data);
  if (!field_offset(&d->offset, e->record, n) ||
      end->position <= before->position || end->sequence <= before->sequence ||
      end->stream != before->stream ||
      end->conversation != before->conversation ||
      n > UINT64_MAX - d->range.text_bytes)
    return failure(&d->failed, error, "invalid document field boundary");
  if (n == 0U && d->range.text_bytes == 0U)
    return CAI_OK;
  if (d->range.text_bytes == 0U)
    d->range.after = *before;
  else if (before->stream != d->range.after.stream ||
           before->conversation != d->range.after.conversation ||
           strcmp(before->session, d->range.after.session) != 0 ||
           strcmp(before->collection, d->range.after.collection) != 0)
    return failure(&d->failed, error, "document changed conversation");
  d->range.text_bytes += n;
  d->range.through_position = end->position;
  d->range.through_sequence = end->sequence;
  return CAI_OK;
}
int cai_cli_batch_document_finish(cai_cli_batch_document *d, int code,
                                  cai_error *error) {
  if (d == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "document is required");
  if (d->failed || d->offset != 0U)
    return failure(&d->failed, error, "document ended inside a field");
  d->range.state = code == CAI_OK ? CAI_TRANSCRIPT_WORK_COMPLETED
                                  : CAI_TRANSCRIPT_WORK_FAILED;
  d->range.code = code;
  return CAI_OK;
}
int cai_cli_batch_document_read(cai_cli_batch_document_reader *r,
                                const cai_cli_batch_document *d, cai_sink *sink,
                                cai_error *error) {
  if (r == NULL || d == NULL || sink == NULL || d->failed ||
      d->range.state != CAI_TRANSCRIPT_WORK_COMPLETED || d->offset != 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "completed document and sink required");
  memset(r, 0, sizeof(*r));
  r->document = *d;
  r->sink = sink;
  r->done = d->range.text_bytes == 0U;
  return CAI_OK;
}
int cai_cli_batch_document_execution(cai_cli_batch_document_reader *r,
                                     const cai_app_execution_event *e,
                                     cai_error *error) {
  const cai_transcript_selection_result *range;
  size_t n;
  int rc;
  if (r == NULL || e == NULL || e->boundary == NULL || r->busy)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "document reader reentry or invalid event");
  if (r->failed)
    return failure(&r->failed, error, "document reader has failed");
  if (r->done)
    return CAI_OK;
  if (!r->started || e->boundary->stream != r->stream || e->record == NULL)
    return failure(&r->failed, error, "document reader has no accepted replay");
  range = &r->document.range;
  if (!matches(&r->document, e))
    return CAI_OK;
  n = strlen(e->record->data);
  if (e->boundary->position <= range->after.position ||
      e->boundary->position > range->through_position ||
      r->bytes > range->text_bytes || n > range->text_bytes - r->bytes ||
      !field_offset(&r->offset, e->record, n))
    return failure(&r->failed, error, "selected document range changed");
  r->busy = 1;
  rc = n != 0U ? cai_sink_write(r->sink, e->record->data, n, error) : CAI_OK;
  r->busy = 0;
  if (rc != CAI_OK) {
    r->failed = 1;
    return rc;
  }
  r->bytes += n;
  return CAI_OK;
}
int cai_cli_batch_document_event(cai_cli_batch_document_reader *r,
                                 const cai_transcript_event *e,
                                 cai_error *error) {
  const cai_transcript_selection_result *range;
  if (r == NULL || e == NULL || r->busy)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "document reader reentry or invalid span");
  if (r->failed)
    return failure(&r->failed, error, "document reader has failed");
  if (r->done)
    return CAI_OK;
  range = &r->document.range;
  if (e->kind == CAI_TRANSCRIPT_ERROR)
    return failure(&r->failed, error,
                   e->reason != NULL ? e->reason : "document source failed");
  if (e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    if (r->started || e->stream == 0U || e->omitted != 0U ||
        e->session == NULL || e->collection == NULL ||
        strcmp(e->session, range->after.session) != 0 ||
        strcmp(e->collection, range->after.collection) != 0 ||
        e->conversation != range->after.conversation ||
        e->watermark < range->through_position)
      return failure(&r->failed, error, "document replay identity changed");
    r->stream = e->stream;
    r->started = 1;
  } else if (!r->started || e->stream != r->stream ||
             e->conversation != range->after.conversation)
    return failure(&r->failed, error, "document replay scope changed");
  else if (e->kind == CAI_TRANSCRIPT_EVENT_END &&
           e->position >= range->through_position) {
    if (e->position != range->through_position ||
        e->sequence != range->through_sequence ||
        r->bytes != range->text_bytes || r->offset != 0U)
      return failure(&r->failed, error, "document replay is incomplete");
    r->done = 1;
  } else if (e->kind == CAI_TRANSCRIPT_REPLAY_END)
    return failure(&r->failed, error,
                   "document replay ended before its final boundary");
  return CAI_OK;
}
