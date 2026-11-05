#include "cai_internal.h"
#include <cai/execution.h>
#include <cai/transcript_selection.h>
#include <string.h>

/** Common validation for bounded borrowed spans. Decoding and projection
 * advance only after a complete event; DATA itself is never retained here. */
typedef struct selection_stream {
  cai_transcript_position at;
  uint64_t sequence, length;
  int open, has_data, text, execution;
} selection_stream;
struct cai_transcript_selection {
  selection_stream stream;
  cai_transcript_selection_result result, candidate;
  cai_execution_decoder *decoder;
  cai_execution_record record;
  char call[256];
  int failed;
};
struct cai_transcript_projector {
  selection_stream stream;
  cai_transcript_selection_result range;
  cai_sink *sink;
  uint64_t bytes;
  int started, done, failed, busy;
};
static int selection_error(cai_error *error, const char *message) {
  return cai_set_error(error, CAI_ERR_PROTOCOL, message);
}
static int position_valid(const cai_transcript_position *p) {
  return p != NULL && p->stream != 0U && p->conversation != 0U &&
         p->session[0] != '\0' && p->collection[0] != '\0' &&
         memchr(p->session, '\0', sizeof(p->session)) != NULL &&
         memchr(p->collection, '\0', sizeof(p->collection)) != NULL;
}
static int stream_event(selection_stream *s, const cai_transcript_event *e,
                        cai_error *error) {
  if (e->stream != s->at.stream || e->conversation != s->at.conversation)
    return selection_error(error, "transcript selection scope changed");
  switch (e->kind) {
  case CAI_TRANSCRIPT_EVENT_BEGIN:
    if (s->open || e->sequence <= s->at.sequence || e->type == NULL ||
        e->type[0] == '\0' || e->offset != 0U ||
        (e->has_data != 0 && e->has_data != 1))
      return selection_error(error, "invalid transcript selection begin");
    s->open = 1;
    s->has_data = e->has_data;
    s->sequence = e->sequence;
    s->length = 0U;
    s->text = strcmp(e->type, "assistant_text_delta") == 0;
    s->execution = strcmp(e->type, "execution") == 0;
    if ((s->text || s->execution) && !s->has_data)
      return selection_error(error, "selected transcript event has no data");
    break;
  case CAI_TRANSCRIPT_DATA:
    if (!s->open || !s->has_data || e->sequence != s->sequence ||
        e->offset != s->length || e->data == NULL || e->length == 0U ||
        e->length > 4096U || e->length > UINT64_MAX - s->length)
      return selection_error(error, "invalid transcript selection chunk");
    s->length += e->length;
    break;
  case CAI_TRANSCRIPT_EVENT_END:
    if (!s->open || e->sequence != s->sequence || e->offset != s->length ||
        e->position <= s->at.position)
      return selection_error(error, "invalid transcript selection end");
    /* Caller consumes this accepted event before advancing at. */
    s->open = 0;
    break;
  case CAI_TRANSCRIPT_REPLAY_BEGIN:
    if (s->open || e->session == NULL || e->collection == NULL ||
        strcmp(e->session, s->at.session) != 0 ||
        strcmp(e->collection, s->at.collection) != 0 ||
        e->watermark < s->at.position || e->omitted != 0U)
      return selection_error(error, "transcript selection replay changed");
    break;
  case CAI_TRANSCRIPT_REPLAY_END:
    if (s->open || e->watermark < s->at.position)
      return selection_error(error, "invalid transcript selection watermark");
    s->at.position = e->watermark;
    break;
  case CAI_TRANSCRIPT_ERROR:
    return cai_set_error(error, e->code > CAI_OK ? e->code : CAI_ERR_TRANSPORT,
                         e->reason != NULL ? e->reason
                                           : "transcript selection failed");
  case CAI_TRANSCRIPT_SHELL_OUTPUT:
  case CAI_TRANSCRIPT_SHELL_OUTPUT_END:
    break; /* Ephemeral bytes do not alter durable cursors. */
  default:
    return selection_error(error, "unknown transcript selection event");
  }
  return CAI_OK;
}
static void stream_accept(selection_stream *s, const cai_transcript_event *e) {
  s->at.sequence = e->sequence;
  s->at.position = e->position;
}
int cai_transcript_selection_open(const cai_transcript_position *after,
                                  const char *call,
                                  cai_transcript_selection **out,
                                  cai_error *error) {
  cai_transcript_selection *s;
  size_t length;
  if (out != NULL)
    *out = NULL;
  length = call != NULL ? strlen(call) : 0U;
  if (out == NULL || !position_valid(after) ||
      (call != NULL && strncmp(call, "input:", 6U) != 0) || length >= 256U)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "accepted cursor and queued work identity required");
  s = cai_alloc(NULL, sizeof(*s));
  if (s == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate transcript selection");
  memset(s, 0, sizeof(*s));
  s->stream.at = s->result.after = *after;
  if (call != NULL)
    memcpy(s->call, call, length + 1U);
  *out = s;
  return CAI_OK;
}
/** A bounded decoded execution record is inspected only at its authenticated
 * outer END. Nested child records cannot finish the parent's selected work. */
static int selection_execution(cai_transcript_selection *s, cai_error *error) {
  cai_execution_record *r = &s->record;
  int rc;
  rc = cai_execution_decoder_finish(s->decoder, r, error);
  if (rc == CAI_OK && s->call[0] == '\0' &&
      s->result.state == CAI_TRANSCRIPT_WORK_WAITING && r->field == NULL &&
      strcmp(r->session_id, s->stream.at.session) == 0) {
    if (r->call_id == NULL && strcmp(r->kind, "response.completed") == 0)
      s->result.state = CAI_TRANSCRIPT_WORK_ACTIVE;
    else if (r->call_id != NULL &&
             (strcmp(r->kind, "turn.started") == 0 ||
              strcmp(r->kind, "response.completed") == 0 ||
              strcmp(r->kind, "turn.completed") == 0 ||
              strcmp(r->kind, "turn.failed") == 0 ||
              strcmp(r->kind, "turn.cancelled") == 0))
      memset(&s->candidate, 0, sizeof(s->candidate));
  }
  if (rc == CAI_OK && s->result.state == CAI_TRANSCRIPT_WORK_ACTIVE &&
      strcmp(r->session_id, s->stream.at.session) == 0 &&
      strcmp(r->kind, "request.failed") == 0 && r->field == NULL) {
    /* A failed new request must not expose the preceding tool preamble as
     * its final answer. Its own accepted partial text is still in candidate. */
    s->result.text_bytes = 0U;
    s->result.through_position = s->result.through_sequence = 0U;
  }
  if (rc == CAI_OK && r->field == NULL &&
      strcmp(r->session_id, s->stream.at.session) == 0 &&
      ((r->call_id == NULL && s->call[0] == '\0') ||
       (r->call_id != NULL && strcmp(r->call_id, s->call) == 0)) &&
      s->result.state <= CAI_TRANSCRIPT_WORK_ACTIVE) {
    if (strcmp(r->kind, "turn.started") == 0) {
      if (s->result.state != CAI_TRANSCRIPT_WORK_WAITING)
        rc = selection_error(error, "selected work started more than once");
      else
        s->result.state = CAI_TRANSCRIPT_WORK_ACTIVE;
    } else if (strcmp(r->kind, "response.completed") == 0) {
      if (s->result.state != CAI_TRANSCRIPT_WORK_ACTIVE)
        rc = selection_error(error, "selected response has no turn start");
      else {
        s->result.after =
            s->candidate.text_bytes != 0U ? s->candidate.after : s->stream.at;
        s->result.text_bytes = s->candidate.text_bytes;
        s->result.through_position = s->candidate.through_position;
        s->result.through_sequence = s->candidate.through_sequence;
        memset(&s->candidate, 0, sizeof(s->candidate));
      }
    } else if (strcmp(r->kind, "turn.completed") == 0 ||
               strcmp(r->kind, "turn.failed") == 0 ||
               strcmp(r->kind, "turn.cancelled") == 0) {
      int success = strcmp(r->kind, "turn.completed") == 0;
      if (success && (s->result.state != CAI_TRANSCRIPT_WORK_ACTIVE ||
                      s->candidate.text_bytes != 0U))
        rc = selection_error(error,
                             "selected completion has no response boundary");
      else {
        if (s->candidate.text_bytes != 0U) {
          s->result.after = s->candidate.after;
          s->result.text_bytes = s->candidate.text_bytes;
          s->result.through_position = s->candidate.through_position;
          s->result.through_sequence = s->candidate.through_sequence;
        }
        s->result.state = success ? CAI_TRANSCRIPT_WORK_COMPLETED
                          : strcmp(r->kind, "turn.failed") == 0
                              ? CAI_TRANSCRIPT_WORK_FAILED
                              : CAI_TRANSCRIPT_WORK_CANCELLED;
        s->result.code = success ? CAI_OK
                         : s->result.state == CAI_TRANSCRIPT_WORK_CANCELLED
                             ? CAI_ERR_CANCELLED
                             : r->code;
        if (s->result.state == CAI_TRANSCRIPT_WORK_FAILED && r->code == CAI_OK)
          rc = selection_error(error, "selected failure has no error code");
      }
    }
  }
  cai_execution_record_cleanup(r);
  return rc;
}
int cai_transcript_selection_event(void *context, const cai_transcript_event *e,
                                   cai_error *error) {
  cai_transcript_selection *s = context;
  int rc;
  if (s == NULL || e == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "selection and event required");
  if (s->failed)
    return cai_set_error(error, s->failed, "transcript selection has failed");
  if (s->result.state >= CAI_TRANSCRIPT_WORK_COMPLETED)
    return CAI_OK;
  rc = stream_event(&s->stream, e, error);
  if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_EVENT_BEGIN &&
      s->stream.execution) {
    rc = s->decoder == NULL ? cai_execution_decoder_open(&s->decoder, error)
                            : cai_execution_decoder_reset(s->decoder, error);
  } else if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_DATA &&
             s->stream.execution)
    rc = cai_execution_decoder_feed(s->decoder, e->data, e->length, error);
  else if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (s->stream.execution)
      rc = selection_execution(s, error);
    else if (s->stream.text &&
             (s->result.state == CAI_TRANSCRIPT_WORK_ACTIVE ||
              (s->call[0] == '\0' &&
               s->result.state == CAI_TRANSCRIPT_WORK_WAITING))) {
      if (s->stream.length > UINT64_MAX - s->candidate.text_bytes)
        rc = selection_error(error, "selected response length overflow");
      else if (s->stream.length != 0U) {
        if (s->candidate.text_bytes == 0U)
          s->candidate.after = s->stream.at;
        s->candidate.text_bytes += s->stream.length;
        s->candidate.through_position = e->position;
        s->candidate.through_sequence = e->sequence;
      }
    }
    if (rc == CAI_OK)
      stream_accept(&s->stream, e);
  }
  if (rc != CAI_OK)
    s->failed = rc;
  return rc;
}
int cai_transcript_selection_get(const cai_transcript_selection *s,
                                 cai_transcript_selection_result *out,
                                 cai_error *error) {
  if (s == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "selection and output required");
  if (s->failed)
    return cai_set_error(error, s->failed, "transcript selection has failed");
  *out = s->result;
  return CAI_OK;
}
void cai_transcript_selection_close(cai_transcript_selection *s) {
  if (s != NULL) {
    cai_execution_decoder_close(s->decoder);
    cai_execution_record_cleanup(&s->record);
    cai_free_mem(NULL, s);
  }
}
int cai_transcript_projector_open(const cai_transcript_selection_result *range,
                                  cai_sink *sink,
                                  cai_transcript_projector **out,
                                  cai_error *error) {
  cai_transcript_projector *p;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || range == NULL || sink == NULL ||
      !position_valid(&range->after) ||
      range->state < CAI_TRANSCRIPT_WORK_COMPLETED ||
      range->state > CAI_TRANSCRIPT_WORK_CANCELLED ||
      (range->text_bytes != 0U &&
       (range->through_position <= range->after.position ||
        range->through_sequence <= range->after.sequence)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "terminal selection and sink required");
  p = cai_alloc(NULL, sizeof(*p));
  if (p == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate transcript projector");
  memset(p, 0, sizeof(*p));
  p->stream.at = range->after;
  p->range = *range;
  p->sink = sink;
  p->done = range->text_bytes == 0U;
  *out = p;
  return CAI_OK;
}
int cai_transcript_projector_event(void *context, const cai_transcript_event *e,
                                   cai_error *error) {
  cai_transcript_projector *p = context;
  int rc;
  if (p == NULL || e == NULL || p->busy)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "projector and nonrecursive event required");
  if (p->failed)
    return cai_set_error(error, p->failed, "transcript projection has failed");
  if (p->done)
    return CAI_OK;
  p->busy = 1;
  if (e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN &&
      (p->started || e->stream == 0U)) {
    p->busy = 0;
    p->failed = CAI_ERR_PROTOCOL;
    return selection_error(error, "projection replay restarted");
  }
  if (!p->started && e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    p->stream.at.stream = e->stream;
    p->started = 1;
  }
  rc = p->started ? stream_event(&p->stream, e, error)
                  : selection_error(error, "projection replay begin required");
  if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN &&
      e->watermark < p->range.through_position)
    rc = selection_error(error, "projection range is no longer available");
  if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_EVENT_BEGIN &&
      e->sequence > p->range.through_sequence)
    rc = selection_error(error, "projection final event is missing");
  if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_DATA && p->stream.text) {
    if (e->length > p->range.text_bytes - p->bytes)
      rc = selection_error(error, "projection text exceeds selected length");
    else {
      rc = cai_sink_write(p->sink, e->data, e->length, error);
      if (rc == CAI_OK)
        p->bytes += e->length;
    }
  } else if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (e->position > p->range.through_position)
      rc = selection_error(error, "projection final position is missing");
    else if (e->sequence == p->range.through_sequence) {
      if (!p->stream.text || e->position != p->range.through_position ||
          p->bytes != p->range.text_bytes)
        rc = selection_error(error, "projection final boundary changed");
      else
        p->done = 1;
    }
    if (rc == CAI_OK)
      stream_accept(&p->stream, e);
  } else if (rc == CAI_OK && e->kind == CAI_TRANSCRIPT_REPLAY_END)
    rc = selection_error(error, "projection ended before its final boundary");
  if (rc != CAI_OK)
    p->failed = rc;
  p->busy = 0;
  return rc;
}
int cai_transcript_projector_done(const cai_transcript_projector *p) {
  return p != NULL && p->done && !p->failed;
}
void cai_transcript_projector_close(cai_transcript_projector *p) {
  if (p != NULL && !p->busy)
    cai_free_mem(NULL, p);
}
