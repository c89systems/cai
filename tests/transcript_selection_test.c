#include "../src/cai_execution.h"
#include <assert.h>
#include <cai/transcript_selection.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
static size_t largest;
static size_t fail_after;
static int allocate(size_t n) {
  if (n > largest)
    largest = n;
  if (n > 65536U)
    return 0;
  return fail_after == 0U || --fail_after != 0U;
}
void *__wrap_malloc(size_t n) { return allocate(n) ? __real_malloc(n) : NULL; }
void *__wrap_calloc(size_t n, size_t size) {
  return size != 0U && n > (size_t)-1 / size ? NULL
         : allocate(n * size)                ? __real_calloc(n, size)
                                             : NULL;
}
void *__wrap_realloc(void *p, size_t n) {
  return allocate(n) ? __real_realloc(p, n) : NULL;
}
static const char work[] = "input:parent:2:3:4";
typedef struct stream {
  cai_transcript_position at;
  int (*event)(void *, const cai_transcript_event *, cai_error *);
  void *context;
  size_t fragment;
} stream;
typedef struct output {
  cai_transcript_projector *projector;
  uint64_t bytes, hash;
  size_t writes;
  int fail, recursive;
  char text[64];
} output;
static void base(stream *s) {
  memset(s, 0, sizeof(*s));
  strcpy(s->at.session, "parent");
  strcpy(s->at.collection, "collection");
  s->at.stream = 7U;
  s->at.conversation = 3U;
  s->at.position = 11U;
  s->at.sequence = 5U;
  s->fragment = 4096U;
}
static cai_transcript_event event_at(const stream *s,
                                     cai_transcript_kind kind) {
  cai_transcript_event e;
  memset(&e, 0, sizeof(e));
  e.kind = kind;
  e.stream = s->at.stream;
  e.conversation = s->at.conversation;
  e.sequence = s->at.sequence + 1U;
  return e;
}
static int emit(stream *s, const char *type, const char *text, size_t length) {
  cai_transcript_event e = event_at(s, CAI_TRANSCRIPT_EVENT_BEGIN);
  size_t at = 0U;
  int rc;
  e.type = type;
  e.has_data = text != NULL;
  rc = s->event(s->context, &e, NULL);
  while (rc == CAI_OK && at < length) {
    size_t n = length - at;
    if (n > s->fragment)
      n = s->fragment;
    e = event_at(s, CAI_TRANSCRIPT_DATA);
    e.offset = at;
    e.data = text + at;
    e.length = n;
    rc = s->event(s->context, &e, NULL);
    at += n;
  }
  if (rc == CAI_OK) {
    e = event_at(s, CAI_TRANSCRIPT_EVENT_END);
    e.offset = length;
    e.position = s->at.position + 2U; /* Commit slots need not be events. */
    rc = s->event(s->context, &e, NULL);
    if (rc == CAI_OK) {
      s->at.position = e.position;
      s->at.sequence = e.sequence;
    }
  }
  return rc;
}
static void execution(stream *s, const char *kind, const char *call,
                      const char *session, int code) {
  cai_execution_record r;
  char *json = NULL;
  cai_execution_record_init(&r);
  r.kind = kind;
  r.call_id = call;
  r.session_id = session;
  r.code = code;
  r.final = r.line_start = 1;
  assert(cai_execution_record_json(&r, &json, NULL) == CAI_OK);
  assert(emit(s, "execution", json, strlen(json)) == CAI_OK);
  free(json);
}
static int write_output(void *context, const void *bytes, size_t n,
                        cai_error *error) {
  output *o = context;
  size_t i;
  (void)error;
  assert(n <= 4096U);
  if (o->fail)
    return CAI_ERR_TRANSPORT;
  if (o->recursive) {
    assert(!cai_transcript_projector_done(o->projector));
    assert(cai_transcript_projector_event(o->projector, NULL, NULL) ==
           CAI_ERR_INVALID);
    cai_transcript_projector_close(
        o->projector); /* Reentrant close is ignored. */
  }
  if (o->bytes + n < sizeof(o->text)) {
    memcpy(o->text + o->bytes, bytes, n);
    o->text[o->bytes + n] = '\0';
  }
  for (i = 0U; i < n; ++i)
    o->hash = o->hash * 33U + ((const unsigned char *)bytes)[i];
  o->bytes += n;
  ++o->writes;
  return CAI_OK;
}
static cai_sink *sink_open(output *o) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = write_output;
  callbacks.context = o;
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  return sink;
}
static int replay(stream *s, uint64_t through) {
  cai_transcript_event e = event_at(s, CAI_TRANSCRIPT_REPLAY_BEGIN);
  e.session = s->at.session;
  e.collection = s->at.collection;
  e.watermark = through;
  return s->event(s->context, &e, NULL);
}
static void last_response(size_t fragment) {
  stream s, p;
  cai_transcript_selection *selection = NULL;
  cai_transcript_selection_result result, saved;
  cai_transcript_projector *projector = NULL;
  cai_sink *sink;
  output out;
  base(&s);
  s.fragment = fragment;
  assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
         CAI_OK);
  s.event = cai_transcript_selection_event;
  s.context = selection;
  execution(&s, "turn.started", "input:parent:99:1:1", "parent", 0);
  assert(emit(&s, "assistant_text_delta", "UNRELATED", 9U) == CAI_OK);
  execution(&s, "turn.completed", "input:parent:99:1:1", "parent", 0);
  assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK &&
         result.state == CAI_TRANSCRIPT_WORK_WAITING &&
         result.text_bytes == 0U);
  execution(&s, "turn.started", work, "parent", 0);
  assert(emit(&s, "assistant_text_delta", "commentary", 10U) == CAI_OK);
  execution(&s, "response.completed", work, "parent", 0);
  assert(emit(&s, "subagent", "CHILD TEXT", 10U) == CAI_OK);
  execution(&s, "turn.completed", work, "child", 0);
  assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK &&
         result.state == CAI_TRANSCRIPT_WORK_ACTIVE &&
         result.text_bytes == 10U);
  assert(emit(&s, "assistant_text_delta", "final", 5U) == CAI_OK);
  assert(emit(&s, "reasoning_summary", "PRIVATE", 7U) == CAI_OK);
  assert(emit(&s, "assistant_text_delta", " λ", 3U) == CAI_OK);
  execution(&s, "response.completed", work, "parent", 0);
  execution(&s, "turn.completed", work, "parent", 0);
  assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK &&
         result.state == CAI_TRANSCRIPT_WORK_COMPLETED &&
         result.text_bytes == 8U);
  saved = result;
  assert(emit(&s, "assistant_text_delta", "FUTURE", 6U) == CAI_OK);
  assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK &&
         memcmp(&result, &saved, sizeof(result)) == 0);
  memset(&out, 0, sizeof(out));
  out.recursive = 1;
  sink = sink_open(&out);
  assert(cai_transcript_projector_open(&result, sink, &projector, NULL) ==
         CAI_OK);
  out.projector = projector;
  p = s;
  p.at = result.after;
  p.at.stream = 8U;
  p.event = cai_transcript_projector_event;
  p.context = projector;
  assert(replay(&p, result.through_position + 100U) == CAI_OK);
  assert(emit(&p, "assistant_text_delta", "final", 5U) == CAI_OK);
  assert(!cai_transcript_projector_done(projector) && out.bytes == 5U);
  assert(emit(&p, "reasoning_summary", "PRIVATE", 7U) == CAI_OK);
  assert(emit(&p, "assistant_text_delta", " λ", 3U) == CAI_OK);
  assert(cai_transcript_projector_done(projector) && out.bytes == 8U &&
         strcmp(out.text, "final λ") == 0);
  assert(emit(&p, "assistant_text_delta", "FUTURE", 6U) == CAI_OK &&
         out.bytes == 8U);
  cai_transcript_projector_close(projector);
  cai_sink_close(sink);
  cai_transcript_selection_close(selection);
}
static void terminal_cases(void) {
  int mode;
  for (mode = 0; mode < 6; ++mode) {
    stream s;
    cai_transcript_selection *selection = NULL;
    cai_transcript_selection_result result;
    cai_transcript_projector *projector = NULL;
    output out;
    cai_sink *sink;
    base(&s);
    assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
           CAI_OK);
    s.event = cai_transcript_selection_event;
    s.context = selection;
    if (mode != 0) {
      execution(&s, "turn.started", work, "parent", 0);
      assert(emit(&s, "assistant_text_delta", "partial", 7U) == CAI_OK);
      if (mode >= 3) {
        execution(&s, "response.completed", work, "parent", 0);
        if (mode == 3)
          execution(&s, "response.completed", work, "parent", 0);
        if (mode == 5)
          execution(&s, "request.failed", NULL, "parent", CAI_ERR_TRANSPORT);
      }
    }
    execution(&s,
              mode < 2 || mode == 5 ? "turn.failed"
              : mode == 2           ? "turn.cancelled"
                                    : "turn.completed",
              work, "parent", mode < 2 || mode == 5 ? CAI_ERR_LIMIT : 0);
    assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK);
    assert(result.state == (mode < 2 || mode == 5 ? CAI_TRANSCRIPT_WORK_FAILED
                            : mode == 2 ? CAI_TRANSCRIPT_WORK_CANCELLED
                                        : CAI_TRANSCRIPT_WORK_COMPLETED));
    assert(result.text_bytes ==
           (mode == 0 || mode == 3 || mode == 5 ? 0U : 7U));
    memset(&out, 0, sizeof(out));
    sink = sink_open(&out);
    assert(cai_transcript_projector_open(&result, sink, &projector, NULL) ==
           CAI_OK);
    assert(cai_transcript_projector_done(projector) ==
           (result.text_bytes == 0U));
    cai_transcript_projector_close(projector);
    cai_sink_close(sink);
    cai_transcript_selection_close(selection);
  }
}
/** A 16 MiB response crosses both adapters with no allocation over 64 KiB.
 * Sink progress is asserted before the source offers its next chunk. */
static void large_response(void) {
  stream s, p;
  cai_transcript_selection *selection = NULL;
  cai_transcript_selection_result result;
  cai_transcript_projector *projector = NULL;
  cai_transcript_event e;
  char chunk[4096];
  size_t i;
  cai_sink *sink;
  output out;
  base(&s);
  assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
         CAI_OK);
  s.event = cai_transcript_selection_event;
  s.context = selection;
  execution(&s, "turn.started", work, "parent", 0);
  memset(chunk, 'x', sizeof(chunk));
  e = event_at(&s, CAI_TRANSCRIPT_EVENT_BEGIN);
  e.type = "assistant_text_delta";
  e.has_data = 1;
  assert(s.event(s.context, &e, NULL) == CAI_OK);
  e.kind = CAI_TRANSCRIPT_DATA;
  e.type = NULL;
  e.data = chunk;
  e.length = sizeof(chunk);
  for (i = 0U; i < 4096U; ++i) {
    e.offset = (uint64_t)i * sizeof(chunk);
    assert(s.event(s.context, &e, NULL) == CAI_OK);
  }
  e.kind = CAI_TRANSCRIPT_EVENT_END;
  e.offset = 4096U * sizeof(chunk);
  e.position = s.at.position + 2U;
  assert(s.event(s.context, &e, NULL) == CAI_OK);
  s.at.position = e.position;
  s.at.sequence = e.sequence;
  execution(&s, "response.completed", work, "parent", 0);
  execution(&s, "turn.completed", work, "parent", 0);
  assert(cai_transcript_selection_get(selection, &result, NULL) == CAI_OK &&
         result.text_bytes == 16777216U);
  memset(&out, 0, sizeof(out));
  sink = sink_open(&out);
  assert(cai_transcript_projector_open(&result, sink, &projector, NULL) ==
         CAI_OK);
  p = s;
  p.at = result.after;
  p.at.stream = 9U;
  p.event = cai_transcript_projector_event;
  p.context = projector;
  assert(replay(&p, result.through_position) == CAI_OK);
  e = event_at(&p, CAI_TRANSCRIPT_EVENT_BEGIN);
  e.type = "assistant_text_delta";
  e.has_data = 1;
  assert(p.event(p.context, &e, NULL) == CAI_OK);
  e.kind = CAI_TRANSCRIPT_DATA;
  e.type = NULL;
  e.data = chunk;
  e.length = sizeof(chunk);
  for (i = 0U; i < 4096U; ++i) {
    e.offset = (uint64_t)i * sizeof(chunk);
    assert(p.event(p.context, &e, NULL) == CAI_OK);
    assert(out.bytes == (i + 1U) * sizeof(chunk));
    assert(!cai_transcript_projector_done(projector));
  }
  e.kind = CAI_TRANSCRIPT_EVENT_END;
  e.offset = result.text_bytes;
  e.position = result.through_position;
  assert(p.event(p.context, &e, NULL) == CAI_OK &&
         cai_transcript_projector_done(projector));
  cai_transcript_projector_close(projector);
  cai_sink_close(sink);
  cai_transcript_selection_close(selection);
}
static void errors(void) {
  int mode;
  for (mode = 0; mode < 10; ++mode) {
    stream s;
    cai_transcript_selection *selection = NULL;
    cai_transcript_selection_result result;
    cai_transcript_event e;
    int rc;
    base(&s);
    assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
           CAI_OK);
    s.event = cai_transcript_selection_event;
    s.context = selection;
    e = event_at(&s, CAI_TRANSCRIPT_EVENT_BEGIN);
    e.type = "assistant_text_delta";
    e.has_data = 1;
    if (mode == 0)
      ++e.stream;
    else if (mode == 1)
      ++e.conversation;
    else if (mode == 2)
      e.has_data = 0;
    else if (mode == 3)
      e.sequence = s.at.sequence;
    else if (mode == 4)
      e.kind = CAI_TRANSCRIPT_ERROR;
    if (mode <= 4)
      rc = s.event(selection, &e, NULL);
    else {
      assert(s.event(selection, &e, NULL) == CAI_OK);
      if (mode == 5)
        rc = s.event(selection, &e, NULL);
      else if (mode < 9) {
        e.kind = CAI_TRANSCRIPT_DATA;
        e.data = "x";
        e.length = 1U;
        if (mode == 6)
          e.offset = 1U;
        if (mode == 7)
          e.length = 4097U;
        if (mode == 8)
          ++e.sequence;
        rc = s.event(selection, &e, NULL);
      } else {
        e.kind = CAI_TRANSCRIPT_EVENT_END;
        e.position = s.at.position;
        rc = s.event(selection, &e, NULL);
      }
    }
    assert(rc != CAI_OK);
    memset(&result, 0x5a, sizeof(result));
    assert(cai_transcript_selection_get(selection, &result, NULL) == rc);
    assert(s.event(selection, &e, NULL) == rc);
    cai_transcript_selection_close(selection);
  }
  for (mode = 0; mode < 11; ++mode) {
    stream s;
    cai_transcript_selection_result range;
    cai_transcript_projector *projector = NULL;
    cai_transcript_event e;
    cai_sink *sink;
    output out;
    int rc;
    base(&s);
    memset(&range, 0, sizeof(range));
    range.after = s.at;
    range.state = CAI_TRANSCRIPT_WORK_COMPLETED;
    range.text_bytes = 1U;
    range.through_sequence = s.at.sequence + 1U;
    range.through_position = s.at.position + 2U;
    memset(&out, 0, sizeof(out));
    sink = sink_open(&out);
    assert(cai_transcript_projector_open(&range, sink, &projector, NULL) ==
           CAI_OK);
    s.event = cai_transcript_projector_event;
    s.context = projector;
    if (mode == 0) {
      rc = emit(&s, "assistant_text_delta", "x", 1U);
    } else if (mode == 1 || mode == 2) {
      e = event_at(&s, CAI_TRANSCRIPT_REPLAY_BEGIN);
      e.session = s.at.session;
      e.collection = mode == 1 ? "replaced" : s.at.collection;
      e.watermark =
          mode == 2 ? range.through_position - 1U : range.through_position;
      rc = s.event(projector, &e, NULL);
    } else {
      assert(replay(&s, range.through_position) == CAI_OK);
      e = event_at(&s, CAI_TRANSCRIPT_EVENT_BEGIN);
      e.type = "assistant_text_delta";
      e.has_data = 1;
      if (mode == 3) {
        ++e.sequence;
        rc = s.event(projector, &e, NULL);
      } else if (mode == 4) {
        rc = replay(&s, range.through_position);
      } else if (mode == 5) {
        e.kind = CAI_TRANSCRIPT_REPLAY_END;
        e.watermark = range.through_position;
        rc = s.event(projector, &e, NULL);
      } else if (mode == 6) {
        out.fail = 1;
        rc = emit(&s, "assistant_text_delta", "x", 1U);
      } else if (mode == 7) {
        rc = emit(&s, "assistant_text_delta", "xx", 2U);
      } else if (mode == 8) {
        rc = emit(&s, "assistant_text_delta", "", 0U);
      } else if (mode == 9) {
        rc = emit(&s, "tool_result", "x", 1U);
      } else {
        assert(s.event(projector, &e, NULL) == CAI_OK);
        e.kind = CAI_TRANSCRIPT_EVENT_END;
        e.offset = 0U;
        e.position = range.through_position + 1U;
        rc = s.event(projector, &e, NULL);
      }
    }
    assert(rc != CAI_OK && !cai_transcript_projector_done(projector));
    assert(emit(&s, "assistant_text_delta", "x", 1U) == rc);
    cai_transcript_projector_close(projector);
    cai_sink_close(sink);
  }
}
static void invalid_and_allocation(void) {
  stream s;
  cai_transcript_selection *selection = NULL;
  cai_transcript_projector *projector = NULL;
  cai_transcript_selection_result range;
  cai_transcript_event e;
  cai_sink *sink;
  output out;
  base(&s);
  memset(&out, 0, sizeof(out));
  sink = sink_open(&out);
  assert(cai_transcript_selection_open(NULL, work, &selection, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_transcript_selection_open(&s.at, "tool-call", &selection, NULL) ==
         CAI_ERR_INVALID);
  assert(cai_transcript_selection_event(NULL, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_transcript_selection_get(NULL, &range, NULL) == CAI_ERR_INVALID);
  assert(cai_transcript_projector_open(NULL, sink, &projector, NULL) ==
         CAI_ERR_INVALID);
  assert(!cai_transcript_projector_done(NULL));
  fail_after = 1U;
  assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
             CAI_ERR_NOMEM &&
         selection == NULL);
  memset(&range, 0, sizeof(range));
  range.after = s.at;
  range.state = CAI_TRANSCRIPT_WORK_COMPLETED;
  fail_after = 1U;
  assert(cai_transcript_projector_open(&range, sink, &projector, NULL) ==
             CAI_ERR_NOMEM &&
         projector == NULL);
  assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
         CAI_OK);
  e = event_at(&s, CAI_TRANSCRIPT_EVENT_BEGIN);
  e.type = "execution";
  e.has_data = 1;
  fail_after = 1U;
  assert(cai_transcript_selection_event(selection, &e, NULL) == CAI_ERR_NOMEM);
  assert(cai_transcript_selection_get(selection, &range, NULL) ==
         CAI_ERR_NOMEM);
  cai_transcript_selection_close(selection);
  selection = NULL;
  assert(cai_transcript_selection_open(&s.at, work, &selection, NULL) ==
         CAI_OK);
  s.event = cai_transcript_selection_event;
  s.context = selection;
  assert(emit(&s, "execution", "{}", 2U) == CAI_ERR_PROTOCOL);
  cai_transcript_selection_close(selection);
  cai_sink_close(sink);
}
static void *worker(void *unused) {
  size_t i;
  (void)unused;
  for (i = 1U; i <= 17U; ++i)
    last_response(i);
  terminal_cases();
  large_response();
  errors();
  invalid_and_allocation();
  assert(largest <= 65536U);
  return NULL;
}
int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
