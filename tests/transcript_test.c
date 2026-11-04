#include "../cli/transcript.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bounded;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void *__wrap_malloc(size_t size) {
  return bounded && size > 65536U ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  return bounded && size != 0U && count > 65536U / size
             ? NULL
             : __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size) {
  return bounded && size > 65536U ? NULL : __real_realloc(pointer, size);
}
typedef struct seen {
  cai_cli_transcript *reader;
  size_t begins, ends, bytes, committed, chunks;
  unsigned long long sequence, position, timestamp;
  long long offset;
  char text[64];
  int has_data, capture, cancel;
} seen;
static int visit(void *context, const cai_cli_transcript_event *event,
                 cai_error *error) {
  seen *s = context;
  (void)error;
  assert(cai_cli_transcript_feed(s->reader, NULL, 0U, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_transcript_finish(s->reader, NULL) == CAI_ERR_INVALID);
  cai_cli_transcript_close(s->reader);
  if (event->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    ++s->begins;
    s->bytes = 0U;
    s->has_data = event->has_data;
    s->sequence = event->sequence;
    s->timestamp = event->timestamp_ns;
    s->offset = event->utc_offset_seconds;
    assert(event->position == 0U && event->type != NULL);
  } else if (event->kind == CAI_CLI_TRANSCRIPT_DATA) {
    assert(event->position == 0U && event->sequence == s->sequence &&
           event->length <= 4096U);
    if (s->capture) {
      assert(s->bytes + event->length < sizeof(s->text));
      memcpy(s->text + s->bytes, event->data, event->length);
      s->text[s->bytes + event->length] = '\0';
    }
    s->bytes += event->length;
    ++s->chunks;
  } else {
    assert(event->sequence == s->sequence && event->data == NULL &&
           event->length == 0U && event->position != 0U);
    s->position = event->position;
    s->committed += s->bytes;
    ++s->ends;
  }
  return s->cancel == (int)event->kind + 1 ? CAI_ERR_CANCELLED : CAI_OK;
}
static void open_reader(seen *s) {
  memset(s, 0, sizeof(*s));
  assert(cai_cli_transcript_open(visit, s, &s->reader, NULL) == CAI_OK);
}
static int feed(cai_cli_transcript *reader, const char *text, size_t chunk) {
  size_t length = strlen(text), at = 0U;
  while (at < length) {
    size_t n = length - at;
    int rc;
    if (n > chunk)
      n = chunk;
    rc = cai_cli_transcript_feed(reader, text + at, n, NULL);
    if (rc != CAI_OK)
      return rc;
    at += n;
  }
  return CAI_OK;
}
static void large(void) {
  static const char prefix[] =
      "{\"record_type\":\"event\",\"sequence\":1,\"timestamp_ns\":9,"
      "\"utc_offset_seconds\":-3600,\"type\":\"tool_result\",\"data\":\"";
  static const char commit[] =
      "{\"record_type\":\"commit\",\"commit_created_at_ns\":8,"
      "\"applied_event_sequence\":1,\"state\":{\"history_append\":\"";
  char chunk[4096];
  size_t i;
  seen s;
  memset(chunk, 'x', sizeof(chunk));
  bounded = 1;
  open_reader(&s);
  assert(feed(s.reader, prefix, 1U) == CAI_OK);
  for (i = 0U; i < 2048U; ++i)
    assert(cai_cli_transcript_feed(s.reader, chunk, sizeof(chunk), NULL) ==
           CAI_OK);
  assert(s.begins == 1U && s.bytes > 8000000U && s.ends == 0U &&
         s.timestamp == 9U && s.offset == -3600);
  assert(feed(s.reader, "\"}\n", 1U) == CAI_OK && s.ends == 0U);
  assert(cai_cli_transcript_boundary(s.reader, 2U, "event-00000000000000000001",
                                     NULL) == CAI_OK);
  assert(s.ends == 1U && s.committed == 8388608U && s.has_data);
  assert(feed(s.reader, commit, 17U) == CAI_OK);
  for (i = 0U; i < 2048U; ++i)
    assert(cai_cli_transcript_feed(s.reader, chunk, sizeof(chunk), NULL) ==
           CAI_OK);
  assert(feed(s.reader, "\"}}\n", 1U) == CAI_OK);
  assert(cai_cli_transcript_boundary(
             s.reader, 3U, "commit-00000000000000000008-00000000000000000001",
             NULL) == CAI_OK);
  assert(s.begins == 1U && s.ends == 1U); /* Commit state never reaches UI. */
  assert(cai_cli_transcript_finish(s.reader, NULL) == CAI_OK);
  cai_cli_transcript_close(s.reader);
  bounded = 0;
}
static void fragments(void) {
  static const char event[] =
      "{\"record_type\":\"event\",\"sequence\":4,\"type\":\"text\","
      "\"data\":\"hi\\n\\u00e5\\ud83d\\ude00\\u0000end\"}\n";
  static const char expected[] = "hi\n\xc3\xa5\xf0\x9f\x98\x80\0end";
  size_t chunk;
  for (chunk = 1U; chunk < 24U; ++chunk) {
    seen s;
    open_reader(&s);
    s.capture = 1;
    assert(feed(s.reader, event, chunk) == CAI_OK && s.ends == 0U);
    assert(cai_cli_transcript_boundary(
               s.reader, 7U, "event-00000000000000000004", NULL) == CAI_OK);
    assert(s.ends == 1U && s.bytes == sizeof(expected) - 1U &&
           memcmp(s.text, expected, s.bytes) == 0 && s.has_data);
    assert(feed(s.reader,
                "{\"record_type\":\"event\",\"sequence\":9,\"type\":\"text\","
                "\"data\":null}",
                chunk) == CAI_OK);
    assert(cai_cli_transcript_boundary(
               s.reader, 11U, "event-00000000000000000009", NULL) == CAI_OK);
    assert(s.ends == 2U && s.bytes == 0U && !s.has_data);
    assert(feed(s.reader,
                "{\"record_type\":\"event\",\"sequence\":10,\"type\":\"text\"}",
                chunk) == CAI_OK);
    assert(cai_cli_transcript_boundary(
               s.reader, 12U, "event-00000000000000000010", NULL) == CAI_OK);
    assert(s.ends == 3U && !s.has_data);
    assert(feed(s.reader,
                "{\"record_type\":\"event\",\"sequence\":11,\"type\":\"text\","
                "\"data\":\"\"}",
                chunk) == CAI_OK);
    assert(cai_cli_transcript_boundary(
               s.reader, 13U, "event-00000000000000000011", NULL) == CAI_OK);
    assert(s.ends == 4U && s.bytes == 0U && s.has_data);
    assert(cai_cli_transcript_finish(s.reader, NULL) == CAI_OK);
    cai_cli_transcript_close(s.reader);
  }
}
static void failures(void) {
  static const char *const invalid[] = {
      "{\"record_type\":\"event\",\"data\":\"early\",\"sequence\":1,\"type\":"
      "\"text\"}",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":{}"
      "}",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":[]"
      "}",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":"
      "true}",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":4}",
      "{\"record_type\":\"event\",\"sequence\":0,\"type\":\"text\",\"data\":"
      "\"bad\"}",
      "{\"record_type\":\"other\",\"sequence\":1,\"type\":\"text\"}",
      "{\"record_type\":\"event\",\"sequence\":1,\"sequence\":2,\"type\":"
      "\"text\"}",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":"
      "\"unterminated",
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\",\"data\":"
      "\"ok\",\"timestamp_ns\":9}",
      "{} {}",
      "{\"record_type\":\"commit\",\"commit_created_at_ns\":3,\"data\":"
      "\"invalid\"}"};
  static const char good[] = "{\"record_type\":\"event\",\"sequence\":1,"
                             "\"type\":\"text\",\"data\":\"ok\"}";
  size_t i;
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    seen s;
    int rc;
    open_reader(&s);
    rc = feed(s.reader, invalid[i], 3U);
    if (rc == CAI_OK)
      rc = cai_cli_transcript_boundary(s.reader, 1U,
                                       "event-00000000000000000001", NULL);
    assert(rc == CAI_ERR_PROTOCOL && s.ends == 0U);
    assert(cai_cli_transcript_feed(s.reader, good, sizeof(good) - 1U, NULL) ==
           rc);
    cai_cli_transcript_close(s.reader);
  }
  for (i = 0U; i < 6U; ++i) {
    seen s;
    open_reader(&s);
    assert(feed(s.reader, good, 1U) == CAI_OK);
    if (i == 0U)
      assert(cai_cli_transcript_finish(s.reader, NULL) == CAI_ERR_PROTOCOL);
    else if (i == 1U)
      assert(cai_cli_transcript_boundary(s.reader, 1U, "event-1", NULL) ==
             CAI_ERR_PROTOCOL);
    else if (i == 2U)
      assert(cai_cli_transcript_boundary(s.reader, 0U,
                                         "event-00000000000000000001",
                                         NULL) == CAI_ERR_PROTOCOL);
    else if (i == 3U)
      assert(cai_cli_transcript_boundary(s.reader, 1U, NULL, NULL) ==
             CAI_ERR_PROTOCOL);
    else if (i == 4U) {
      assert(cai_cli_transcript_boundary(
                 s.reader, 1U, "event-00000000000000000001", NULL) == CAI_OK);
      assert(feed(s.reader, good, 1U) ==
             CAI_ERR_PROTOCOL); /* Sequence reused. */
    } else
      assert(cai_cli_transcript_boundary(s.reader, 1U,
                                         "event-00000000000000000002",
                                         NULL) == CAI_ERR_PROTOCOL);
    assert(s.ends == (i == 4U ? 1U : 0U));
    cai_cli_transcript_close(s.reader);
  }
  for (i = 0U; i < 3U; ++i) {
    seen s;
    int rc;
    open_reader(&s);
    s.cancel = (int)i + 1;
    rc = feed(s.reader, good, 1U);
    if (rc == CAI_OK)
      rc = cai_cli_transcript_boundary(s.reader, 1U,
                                       "event-00000000000000000001", NULL);
    assert(rc == CAI_ERR_CANCELLED);
    assert(cai_cli_transcript_finish(s.reader, NULL) == CAI_ERR_CANCELLED);
    cai_cli_transcript_close(s.reader);
  }
}
static void *run(void *unused) {
  (void)unused;
  fragments();
  large();
  failures();
  return NULL;
}
int main(void) {
  pthread_t worker;
  cai_cli_transcript *reader = NULL;
  assert(cai_cli_transcript_open(NULL, NULL, &reader, NULL) ==
             CAI_ERR_INVALID &&
         reader == NULL);
  assert(cai_cli_transcript_open(visit, NULL, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_transcript_feed(NULL, NULL, 0U, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_transcript_finish(NULL, NULL) == CAI_ERR_INVALID);
  cai_cli_transcript_close(NULL);
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
