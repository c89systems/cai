#define _POSIX_C_SOURCE 200809L
#include "../cli/journal_projection.h"
#include "../src/cai_input_queue_codec.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bounded;
/* Static musl calloc/realloc can call the wrapped malloc/free internally.
 * Count each public allocation once, like the dynamically linked native case.
 */
static int allocation_depth;
static struct allocation {
  void *pointer;
  size_t size;
} allocations[1024];
static size_t live_bytes, peak_bytes;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
static size_t allocation_at(void *pointer) {
  size_t i;
  if (pointer != NULL)
    for (i = 0U; i < 1024U; i++)
      if (allocations[i].pointer == pointer)
        return i;
  return 1024U;
}
static void remember(void *pointer, size_t size) {
  size_t i;
  if (pointer == NULL || !bounded)
    return;
  for (i = 0U; i < 1024U && allocations[i].pointer != NULL; i++) {
  }
  assert(i != 1024U);
  allocations[i].pointer = pointer;
  allocations[i].size = size;
  live_bytes += size;
  if (peak_bytes < live_bytes)
    peak_bytes = live_bytes;
}
void *__wrap_malloc(size_t n) {
  void *pointer;
  if (allocation_depth)
    return __real_malloc(n);
  if (bounded && (n > 65536U || n > 1048576U - live_bytes))
    return NULL;
  allocation_depth++;
  pointer = __real_malloc(n);
  allocation_depth--;
  remember(pointer, n);
  return pointer;
}
void *__wrap_calloc(size_t n, size_t width) {
  void *pointer;
  if (allocation_depth)
    return __real_calloc(n, width);
  if (bounded && width != 0U &&
      (n > 65536U / width || n > (1048576U - live_bytes) / width))
    return NULL;
  allocation_depth++;
  pointer = __real_calloc(n, width);
  allocation_depth--;
  remember(pointer, n * width);
  return pointer;
}
void *__wrap_realloc(void *p, size_t n) {
  size_t at = allocation_at(p), old = at == 1024U ? 0U : allocations[at].size;
  void *pointer;
  if (allocation_depth)
    return __real_realloc(p, n);
  if (bounded && (n > 65536U || n > 1048576U - (live_bytes - old)))
    return NULL;
  allocation_depth++;
  pointer = __real_realloc(p, n);
  allocation_depth--;
  if (pointer != NULL || n == 0U) {
    if (at != 1024U) {
      live_bytes -= old;
      allocations[at].pointer = NULL;
    }
    remember(pointer, n);
  }
  return pointer;
}
void __wrap_free(void *pointer) {
  size_t at = allocation_at(pointer);
  if (allocation_depth) {
    __real_free(pointer);
    return;
  }
  if (at != 1024U) {
    live_bytes -= allocations[at].size;
    allocations[at].pointer = NULL;
  }
  allocation_depth++;
  __real_free(pointer);
  allocation_depth--;
}

/* The fake receiver implements the same authenticated entry seams and head
 * precondition as LC. Payload generation remains bounded and synchronous. */
typedef struct fixture {
  lc_collection collection;
  const char *records[8], *ids[8];
  size_t count, chunk, calls, produced, received, delivered;
  char output[8192];
  size_t length;
  int large, capture, producing, observed_flow, failed_read, changed_head,
      cancelled, no_boundary, incomplete, fail_sink;
} fixture;
static int head(lc_collection *collection, lc_collection_head_res *out,
                lc_error *error) {
  fixture *f = collection->impl;
  (void)error;
  memset(out, 0, sizeof(*out));
  strcpy(out->etag, "stable-head");
  out->upper_position = f->count;
  return LC_OK;
}
static int bytes(fixture *f, lc_sink *sink, const void *data, size_t count,
                 lc_error *error) {
  const char *at = data;
  while (count != 0U) {
    size_t n = count < f->chunk ? count : f->chunk;
    if (!sink->write(sink, at, n, error))
      return 0;
    count -= n;
    at += n;
  }
  return 1;
}
static int repeat(fixture *f, lc_sink *sink, lc_error *error) {
  char chunk[4096];
  size_t i;
  memset(chunk, 'x', sizeof(chunk));
  f->producing = 1;
  for (i = 0U; i < 2048U; i++) {
    f->produced += sizeof(chunk);
    if (!bytes(f, sink, chunk, sizeof(chunk), error)) {
      f->producing = 0;
      return 0;
    }
  }
  f->producing = 0;
  return 1;
}
static int large_record(fixture *f, size_t position, lc_sink *sink,
                        lc_error *error) {
  static const char array_prefix[] = "[{\"role\":\"user\",\"content\":\"";
  static const char array_suffix[] = "\"}]";
  char prefix[1024];
  size_t payload = 8U * 1024U * 1024U;
  size_t array =
      sizeof(array_prefix) - 1U + payload + sizeof(array_suffix) - 1U;
  char number[32];
  int digits =
      snprintf(number, sizeof(number), "%llu", (unsigned long long)array);
  size_t total = (size_t)digits + 1U + array + 1U;
  if (position == 2U) {
    if (!bytes(f, sink,
               "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"text\","
               "\"data\":\"",
               sizeof("{\"record_type\":\"event\",\"sequence\":1,\"type\":"
                      "\"text\",\"data\":\"") -
                   1U,
               error) ||
        !repeat(f, sink, error))
      return 0;
    return bytes(f, sink, "\"}\n", 3U, error);
  }
  if (position == 3U) {
    snprintf(
        prefix, sizeof(prefix),
        "{\"record_type\":\"commit\",\"commit_created_at_ns\":3,\"applied_"
        "event_sequence\":1,\"state\":{\"revision\":2,\"base_bytes\":%llu,"
        "\"end_bytes\":%llu,\"blocks_keep\":0,\"hidden_keep\":0,\"owned_keep\":"
        "0,\"metadata\":{\"version\":1},\"history_append\":\"2\\n[]\\n\"}}\n",
        (unsigned long long)total, (unsigned long long)total + 5U);
    return bytes(f, sink, prefix, strlen(prefix), error);
  }
  snprintf(prefix, sizeof(prefix),
           "{\"record_type\":\"commit\",\"commit_created_at_ns\":1,\"applied_"
           "event_sequence\":0,\"state\":{\"revision\":1,\"base_bytes\":0,"
           "\"end_bytes\":%llu,\"blocks_keep\":0,\"hidden_keep\":0,\"owned_"
           "keep\":0,\"metadata\":{\"version\":1,\"goal_objective\":\"",
           (unsigned long long)total);
  if (!bytes(f, sink, prefix, strlen(prefix), error) || !repeat(f, sink, error))
    return 0;
  snprintf(prefix, sizeof(prefix),
           "\"},\"history_append\":\"%llu\\n[{\\\"role\\\":\\\"user\\\","
           "\\\"content\\\":\\\"",
           (unsigned long long)array);
  if (!bytes(f, sink, prefix, strlen(prefix), error))
    return 0;
  if (!repeat(f, sink, error))
    return 0;
  return bytes(f, sink, "\\\"}]\\n\"}}\n", sizeof("\\\"}]\\n\"}}\n") - 1U,
               error);
}
static int read_collection(lc_collection *collection,
                           const lc_collection_read_opts *options,
                           lc_sink *sink, lc_collection_read_res *out,
                           lc_error *error) {
  fixture *f = collection->impl;
  size_t i;
  ++f->calls;
  assert(f->calls <= 6U); /* Constant passes, never a read per context item. */
  assert(options->expected_head != NULL &&
         strcmp(options->expected_head, "stable-head") == 0);
  assert(options->has_after && options->has_through &&
         options->selector_lql == NULL);
  if ((f->failed_read || f->changed_head) && f->calls > 1U)
    return LC_ERR_TRANSPORT;
  for (i = 0U; i < f->count; i++) {
    lc_collection_read_metadata entry;
    if (i + 1U <= options->after || i + 1U > options->through)
      continue;
    if (f->large) {
      if (!large_record(f, i + 1U, sink, error))
        return LC_ERR_TRANSPORT;
    } else if (!bytes(f, sink, f->records[i], strlen(f->records[i]), error))
      return LC_ERR_TRANSPORT;
    memset(&entry, 0, sizeof(entry));
    entry.kind = LC_COLLECTION_READ_METADATA_ENTRY;
    entry.position = i + 1U;
    entry.append_id = f->ids[i];
    if (!f->no_boundary &&
        options->on_metadata(options->metadata_context, &entry, error) != LC_OK)
      return LC_ERR_INVALID;
  }
  out->complete = !f->incomplete;
  if (f->cancelled)
    out->stop_reason = LC_COLLECTION_READ_CANCELLED;
  return LC_OK;
}
static int receive(void *context, const void *data, size_t length,
                   cai_error *error) {
  fixture *f = context;
  (void)error;
  if (f->fail_sink)
    return CAI_ERR_CANCELLED;
  if (f->producing) {
    f->observed_flow = 1;
    f->delivered += length;
  }
  f->received += length;
  if (f->capture) {
    assert(f->length + length < sizeof(f->output));
    memcpy(f->output + f->length, data, length);
    f->length += length;
    f->output[f->length] = '\0';
  }
  return CAI_OK;
}
static void init(fixture *f) {
  memset(f, 0, sizeof(*f));
  f->collection.impl = f;
  f->collection.head = head;
  f->collection.read = read_collection;
  f->chunk = 1U;
  f->capture = 1;
}
static int project(fixture *f) {
  cai_sink_callbacks callbacks;
  cai_sink *sink = NULL;
  cai_error error;
  int rc, was_bounded = bounded;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = receive;
  callbacks.context = f;
  cai_error_init(&error);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  bounded = 1;
  rc = cai_cli_journal_project(&f->collection, sink, &error);
  bounded = was_bounded;
  if (rc != CAI_OK && !f->failed_read && !f->changed_head && !f->cancelled &&
      !f->no_boundary && !f->incomplete && !f->fail_sink)
    fprintf(stderr, "projection: %s\n",
            error.message != NULL ? error.message : "failed");
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  assert(live_bytes == 0U);
  return rc;
}
static const char first[] =
    "{\"record_type\":\"commit\",\"commit_created_at_ns\":1,\"applied_event_"
    "sequence\":2,\"state\":{\"revision\":1,\"base_bytes\":0,\"end_bytes\":16,"
    "\"blocks_keep\":0,\"hidden_keep\":0,\"owned_keep\":0,\"metadata\":{"
    "\"version\":1,\"model\":\"old\",\"history_blocks\":[{\"first_item\":0,"
    "\"event_start\":1,\"input_sequence\":1,\"kind\":1,\"summary\":\"A\"},{"
    "\"first_item\":1,\"event_start\":2,\"input_sequence\":2,\"kind\":1,"
    "\"summary\":\"B\"}],\"history_owned_events\":[{\"entry\":0,\"sequence\":1}"
    "]},\"history_append\":\"5\\n[\\\"a\\\"]\\n5\\n[\\\"b\\\"]\\n\"}}\n";
static const char final_prefix[] =
    "{\"record_type\":\"commit\",\"commit_created_at_ns\":2,\"applied_event_"
    "sequence\":2,\"state\":{\"revision\":2,\"base_bytes\":8,\"end_bytes\":16,"
    "\"blocks_keep\":1,\"hidden_keep\":0,\"owned_keep\":1,\"accounting\":{"
    "\"input_tokens\":99,\"cached_tokens\":4,\"output_tokens\":7,\"reasoning_"
    "tokens\":3,\"total_tokens\":106,\"usd\":0.125,\"eur\":0.25},";
static const char final_suffix[] =
    "\"metadata\":{\"version\":1,\"model\":\"new\",\"goal_status\":\"active\","
    "\"goal_elapsed_seconds\":123,\"history_blocks\":[{\"first_item\":1,"
    "\"event_start\":3,\"input_sequence\":3,\"kind\":1,\"summary\":\"C\"}],"
    "\"history_hidden_events\":[{\"first\":2,\"last\":2}],\"history_owned_"
    "events\":[{\"entry\":1,\"sequence\":3}]},\"history_append\":\"5\\n["
    "\\\"c\\\"]\\n\"}}\n";
static char final[1024];
static const char event1[] =
    "{\"record_type\":\"event\",\"sequence\":1,\"timestamp_ns\":123,\"utc_"
    "offset_seconds\":3600,\"type\":\"text\",\"data\":\"hi \\u00e5 "
    "\\ud83d\\ude00\\n\"}\n";
static const char event2[] = "{\"record_type\":\"event\",\"sequence\":2,"
                             "\"type\":\"text\",\"data\":\"REMOVED\"}\n";
static void seed_fixture(fixture *f) {
  init(f);
  f->count = 4U;
  f->records[0] = event1;
  f->ids[0] = "event-00000000000000000001";
  f->records[1] = event2;
  f->ids[1] = "event-00000000000000000002";
  f->records[2] = first;
  f->ids[2] = "commit-00000000000000000001-00000000000000000002";
  f->records[3] = final;
  f->ids[3] = "commit-00000000000000000002-00000000000000000002";
}
static void edits(void) {
  fixture f;
  size_t chunk;
  for (chunk = 1U; chunk < 24U; chunk++) {
    seed_fixture(&f);
    f.chunk = chunk;
    assert(project(&f) == CAI_OK);
    assert(strstr(f.output, "REMOVED") == NULL &&
           strstr(f.output, "\"model\":\"old\"") == NULL);
    assert(strstr(f.output, "5\\n[\\\"a\\\"]\\n5\\n[\\\"c\\\"]\\n") != NULL);
    assert(strstr(f.output, "[\\\"b\\\"]") == NULL &&
           strstr(f.output, "\"summary\":\"B\"") == NULL);
    assert(strstr(f.output, "\"summary\":\"A\"") &&
           strstr(f.output, "\"summary\":\"C\""));
    assert(strstr(f.output, "\"input_tokens\":99") &&
           strstr(f.output, "\"goal_elapsed_seconds\":123"));
    assert(strstr(f.output, "\"revision\":1") &&
           !strstr(f.output, "\"revision\":2"));
    assert(strstr(f.output, "\"cached_tokens\":4") &&
           strstr(f.output, "\"reasoning_tokens\":3"));
    assert(f.calls == 4U);
  }
}
static void errors(void) {
  fixture f;
  char corrupted[4096];
  char *at;
  static const char reordered[] =
      "{\"record_type\":\"commit\",\"commit_created_at_ns\":1,\"applied_event_"
      "sequence\":0,\"state\":{\"history_append\":\"\",\"revision\":1,\"base_"
      "bytes\":0,\"end_bytes\":0,\"blocks_keep\":0,\"hidden_keep\":0,\"owned_"
      "keep\":0,\"metadata\":{\"version\":1}}}\n";
  const char *needles[] = {"\"revision\":2",    "\"base_bytes\":8",
                           "\"end_bytes\":16",  "\"blocks_keep\":1",
                           "\"hidden_keep\":0", "\"owned_keep\":1",
                           "\"version\":1"};
  size_t i;
  for (i = 0U; i < sizeof(needles) / sizeof(needles[0]); i++) {
    seed_fixture(&f);
    strcpy(corrupted, final);
    at = strstr(corrupted, needles[i]);
    assert(at);
    at[strlen(needles[i]) - 1U] = '9';
    f.records[3] = corrupted;
    assert(project(&f) != CAI_OK);
  }
  init(&f);
  f.count = 1U;
  f.records[0] = reordered;
  f.ids[0] = "commit-00000000000000000001-00000000000000000000";
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.failed_read = 1;
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.changed_head = 1;
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.cancelled = 1;
  assert(project(&f) == CAI_ERR_CANCELLED);
  seed_fixture(&f);
  f.fail_sink = 1;
  assert(project(&f) == CAI_ERR_CANCELLED);
  seed_fixture(&f);
  f.no_boundary = 1;
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.incomplete = 1;
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.ids[3] = "wrong";
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  f.records[3] = "{\"record_type\":\"commit\"";
  assert(project(&f) != CAI_OK);
  seed_fixture(&f);
  strcpy(corrupted, final);
  at = strstr(corrupted, "[\\\"c\\\"]");
  assert(at);
  at[0] = '{';
  f.records[3] = corrupted;
  assert(project(&f) != CAI_OK);
}
static void checkpoint(void) {
  fixture f;
  cai_input_queue *queue = NULL, *restored = NULL;
  cai_input_queue_client *client = NULL;
  cai_input_queue_config config;
  cai_input_request request;
  cai_input_result result;
  cai_input_state state;
  cai_input_checkpoint_restore *parser = NULL;
  cai_source *source = NULL;
  char json[4096], record[8192];
  const char *at, *saved;
  size_t length = 0U, n;
  unsigned long long through = 0U;
  cai_input_queue_config_init(&config);
  assert(cai_input_queue_open(&config, &queue, NULL) == CAI_OK);
  assert(cai_input_queue_client_open(queue, &client, NULL) == CAI_OK);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = 1U;
  request.text = "pending prompt after cut";
  request.intent = CAI_INPUT_STEERING;
  strcpy(request.origin, "message:export");
  assert(cai_input_queue_request(client, &request, &result, NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_ACCEPTED);
  assert(cai_input_checkpoint_source(queue, 2U, &source, NULL) == CAI_OK);
  while ((n = cai_source_read(source, json + length, sizeof(json) - length - 1U,
                              NULL)) != 0U) {
    length += n;
    assert(length + 1U < sizeof(json));
  }
  json[length] = '\0';
  cai_source_close(source);
  seed_fixture(&f);
  at = strstr(first, "\"history_append\"");
  assert(at);
  assert(snprintf(record, sizeof(record), "%.*s\"input_checkpoint\":%s,%s",
                  (int)(at - first), first, json, at) < (int)sizeof(record));
  f.records[2] = record;
  assert(project(&f) == CAI_OK && f.calls == 5U);
  saved = strstr(f.output, json);
  assert(saved != NULL);
  assert(cai_input_checkpoint_begin(NULL, &parser, NULL) == CAI_OK);
  for (n = 0U; n < length; n++)
    assert(cai_input_checkpoint_feed(parser, saved + n, 1U, NULL) == CAI_OK);
  assert(cai_input_checkpoint_finish(parser, &restored, &through, NULL) ==
             CAI_OK &&
         through == 2U);
  assert(cai_input_queue_get_state(restored, &state, NULL) == CAI_OK &&
         state.count == 1U);
  cai_input_queue_close(restored);
  cai_input_checkpoint_close(parser);
  cai_input_queue_client_close(client);
  cai_input_queue_close(queue);
}
static void *worker(void *unused) {
  fixture f;
  (void)unused;
  edits();
  errors();
  checkpoint();
  init(&f);
  f.count = 3U;
  f.large = 1;
  f.capture = 0;
  f.chunk = 4096U;
  f.ids[0] = "commit-00000000000000000001-00000000000000000000";
  f.ids[1] = "event-00000000000000000001";
  f.ids[2] = "commit-00000000000000000003-00000000000000000001";
  bounded = 1;
  assert(project(&f) == CAI_OK);
  bounded = 0;
  assert(live_bytes == 0U && peak_bytes < 1048576U);
  assert(f.observed_flow && f.delivered > 16000000U && f.calls == 4U);
  return NULL;
}
int main(void) {
  pthread_t thread;
  assert(snprintf(final, sizeof(final), "%s%s", final_prefix, final_suffix) <
         (int)sizeof(final));
  /* Default musl worker stack, never an enlarged export stack. */
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
