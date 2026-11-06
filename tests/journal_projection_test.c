#define _POSIX_C_SOURCE 200809L
#include "../cli/journal_projection.h"
#include "../cli/markdown_export.h"
#include "../src/cai_input_queue_codec.h"
#include <assert.h>
#include <cai/execution.h>
#include <cai/handover.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bounded_allocator.h"

/* Normal checks stress multi-MiB values; memory diagnostics use the same
 * paths with values still larger than the maximum permitted allocation. */
static size_t generated_bytes = 8U * 1024U * 1024U;

int __wrap_cai_client_open(const cai_client_config *, cai_client **,
                           cai_error *);
int __wrap_cai_agent_runtime_open(cai_client *,
                                  const cai_agent_runtime_config *,
                                  cai_agent_runtime **, cai_error *);
int __wrap_cai_client_open(const cai_client_config *config, cai_client **out,
                           cai_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
int __wrap_cai_agent_runtime_open(cai_client *client,
                                  const cai_agent_runtime_config *config,
                                  cai_agent_runtime **out, cai_error *error) {
  (void)client;
  (void)config;
  (void)out;
  (void)error;
  abort();
}

/* The fake receiver implements the same authenticated entry seams and head
 * precondition as LC. Payload generation remains bounded and synchronous. */
typedef struct fixture {
  lc_collection collection;
  const char *records[8], *ids[8];
  size_t count, chunk, calls, produced, received, delivered, heads, read_limit;
  char output[8192];
  size_t length, cancel_after, sdk_cancel_checks;
  size_t deleted_position, delete_from_call;
  int append_after_head;
  int large, large_markdown, capture, producing, observed_flow, failed_read,
      changed_head, cancelled, no_boundary, incomplete, fail_sink,
      callback_cancelled;
} fixture;
static int head(lc_collection *collection, lc_collection_head_res *out,
                lc_error *error) {
  fixture *f = collection->impl;
  f->heads++;
  (void)error;
  memset(out, 0, sizeof(*out));
  strcpy(out->etag, "stable-head");
  out->upper_position = f->count;
  out->live_count = f->count;
  if (f->append_after_head)
    ++f->count;
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
  for (i = 0U; i < generated_bytes / sizeof(chunk); i++) {
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
  const char *array_prefix =
      f->large_markdown ? "[{\"role\":\"user\",\"content\":[{\"text\":\""
                        : "[{\"role\":\"user\",\"content\":\"";
  const char *array_suffix = f->large_markdown ? "\"}]}]" : "\"}]";
  char prefix[1024];
  size_t payload = generated_bytes;
  size_t array = strlen(array_prefix) + payload + strlen(array_suffix);
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
           "\\\"content\\\":%s",
           (unsigned long long)array,
           f->large_markdown ? "[{\\\"text\\\":\\\"" : "\\\"");
  if (!bytes(f, sink, prefix, strlen(prefix), error))
    return 0;
  if (!repeat(f, sink, error))
    return 0;
  return f->large_markdown ? bytes(f, sink, "\\\"}]}]\\n\"}}\n",
                                   sizeof("\\\"}]}]\\n\"}}\n") - 1U, error)
                           : bytes(f, sink, "\\\"}]\\n\"}}\n",
                                   sizeof("\\\"}]\\n\"}}\n") - 1U, error);
}
static int read_collection(lc_collection *collection,
                           const lc_collection_read_opts *options,
                           lc_sink *sink, lc_collection_read_res *out,
                           lc_error *error) {
  fixture *f = collection->impl;
  size_t i;
  ++f->calls;
  assert(f->calls <= f->read_limit);      /* Never a read per context item. */
  assert(options->expected_head == NULL); /* Prefix remains valid on append. */
  assert(options->has_after && options->has_through &&
         options->selector_lql == NULL);
  if (options->cancelled != NULL) {
    f->sdk_cancel_checks++;
    if (options->cancelled(options->cancel_context)) {
      out->complete = 1;
      out->stop_reason = LC_COLLECTION_READ_CANCELLED;
      return LC_OK;
    }
  }
  if ((f->failed_read || f->changed_head) && f->calls > 1U)
    return LC_ERR_TRANSPORT;
  for (i = 0U; i < f->count; i++) {
    lc_collection_read_metadata entry;
    if (i + 1U <= options->after || i + 1U > options->through)
      continue;
    if (i + 1U == f->deleted_position && f->calls >= f->delete_from_call)
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
  f->read_limit = 6U;
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
static void immutable_prefix(void) {
  fixture f;
  static const char appended[] =
      "{\"record_type\":\"event\",\"sequence\":3,"
      "\"type\":\"text\",\"data\":\"LATER APPEND\"}\n";
  size_t call;
  seed_fixture(&f);
  f.records[4] = appended;
  f.ids[4] = "event-00000000000000000003";
  f.append_after_head = 1;
  assert(project(&f) == CAI_OK);
  assert(f.count == 5U && f.heads == 1U && !strstr(f.output, "LATER APPEND"));
  for (call = 1U; call <= 2U; ++call) {
    seed_fixture(&f);
    f.deleted_position = 2U; /* Even an originally hidden record is counted. */
    f.delete_from_call = call;
    assert(project(&f) != CAI_OK);
  }
  seed_fixture(&f);
  f.deleted_position = 3U;
  f.delete_from_call = 4U;
  assert(project(&f) != CAI_OK);
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
typedef struct view_events {
  fixture *fixture;
  cai_cli_journal_view *view;
  cai_sink *sink;
  unsigned int begins, ends;
  int reenter;
} view_events;
static int view_event(void *context, const cai_cli_transcript_event *event,
                      cai_error *error) {
  view_events *events = context;
  assert(event->sequence == 1U);
  if (event->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    assert(strcmp(event->type, "text") == 0);
    assert(event->timestamp_ns == 123U && event->utc_offset_seconds == 3600);
    events->begins++;
    if (events->reenter)
      assert(cai_cli_journal_view_metadata(events->view, events->sink, NULL) ==
             CAI_ERR_INVALID);
  } else if (event->kind == CAI_CLI_TRANSCRIPT_END)
    events->ends++;
  else
    return receive(events->fixture, event->data, event->length, error);
  return CAI_OK;
}
static void reset_output(fixture *f) {
  f->length = 0U;
  f->output[0] = '\0';
}
static void views(void) {
  fixture f;
  cai_cli_journal_view *view = NULL;
  cai_sink sink;
  view_events events;
  char jsonl[8192];
  unsigned int i;
  seed_fixture(&f);
  f.read_limit = 12U;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = receive;
  sink.callbacks.context = &f;
  assert(cai_cli_journal_view_open(&f.collection, NULL, &view, NULL) == CAI_OK);
  assert(f.heads == 1U && f.calls == 1U);
  assert(cai_cli_journal_view_metadata(view, &sink, NULL) == CAI_OK);
  assert(f.calls == 2U && strstr(f.output, "\"model\":\"new\""));
  assert(strstr(f.output, "\"summary\":\"A\"") &&
         strstr(f.output, "\"summary\":\"C\""));
  assert(!strstr(f.output, "\"summary\":\"B\""));
  for (i = 0U; i < 2U; i++) {
    reset_output(&f);
    assert(cai_cli_journal_view_history(view, &sink, NULL) == CAI_OK);
    assert(strcmp(f.output, "5\n[\"a\"]\n5\n[\"c\"]\n") == 0);
    assert(f.calls == 3U + i);
  }
  reset_output(&f);
  memset(&events, 0, sizeof(events));
  events.fixture = &f;
  events.view = view;
  events.sink = &sink;
  events.reenter = 1;
  assert(cai_cli_journal_view_events(view, view_event, &events, NULL) ==
         CAI_OK);
  assert(events.begins == 1U && events.ends == 1U && f.calls == 5U);
  assert(strcmp(f.output, "hi \303\245 \360\237\230\200\n") == 0);
  reset_output(&f);
  assert(cai_cli_journal_view_jsonl(view, &sink, NULL) == CAI_OK);
  assert(strstr(f.output, "REMOVED") == NULL && f.calls == 8U);
  strcpy(jsonl, f.output);
  reset_output(&f);
  assert(cai_cli_journal_view_jsonl(view, &sink, NULL) == CAI_OK);
  assert(strcmp(f.output, jsonl) == 0 && f.calls == 11U && f.heads == 1U);
  cai_cli_journal_view_close(view);
}
static void view_failures(void) {
  fixture f;
  cai_cli_journal_view *view = NULL;
  cai_sink sink;
  view_events events;
  unsigned int i;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = receive;
  sink.callbacks.context = &f;
  assert(cai_cli_journal_view_open(NULL, NULL, &view, NULL) ==
             CAI_ERR_INVALID &&
         !view);
  assert(cai_cli_journal_view_open(NULL, NULL, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_cli_journal_view_metadata(NULL, &sink, NULL) == CAI_ERR_INVALID);
  for (i = 0U; i < 4U; i++) {
    seed_fixture(&f);
    assert(cai_cli_journal_view_open(&f.collection, NULL, &view, NULL) ==
           CAI_OK);
    if (i == 0U)
      f.changed_head = 1;
    else if (i == 1U)
      f.cancelled = 1;
    else if (i == 2U)
      f.fail_sink = 1;
    else
      f.no_boundary = 1;
    assert(cai_cli_journal_view_history(view, &sink, NULL) != CAI_OK);
    assert(f.calls == 2U);
    assert(cai_cli_journal_view_jsonl(view, &sink, NULL) != CAI_OK);
    assert(f.calls == 2U);
    cai_cli_journal_view_close(view);
    view = NULL;
  }
  seed_fixture(&f);
  assert(cai_cli_journal_view_open(&f.collection, NULL, &view, NULL) == CAI_OK);
  f.fail_sink = 1;
  memset(&events, 0, sizeof(events));
  events.fixture = &f;
  assert(cai_cli_journal_view_events(view, view_event, &events, NULL) ==
         CAI_ERR_CANCELLED);
  assert(events.begins == 1U && events.ends == 0U && f.calls == 2U);
  cai_cli_journal_view_close(view);
}
static void native_markdown(void) {
  static const char item[] =
      "[{\"content\":[{\"text\":\"native markdown\"}],\"role\":\"assistant\"}]";
  static const char encoded[] =
      "[{\\\"content\\\":[{\\\"text\\\":\\\"native markdown\\\"}],"
      "\\\"role\\\":\\\"assistant\\\"}]";
  fixture f;
  cai_cli_journal_view *view = NULL;
  cai_handover_config config;
  cai_sink sink;
  char record[1024], length[32];
  size_t bytes;
  init(&f);
  snprintf(length, sizeof(length), "%lu", (unsigned long)strlen(item));
  bytes = strlen(length) + 1U + strlen(item) + 1U;
  assert(snprintf(
             record, sizeof(record),
             "{\"record_type\":\"commit\",\"commit_created_at_ns\":1,"
             "\"applied_event_sequence\":0,\"state\":{\"revision\":1,"
             "\"base_bytes\":0,\"end_bytes\":%lu,\"blocks_keep\":0,"
             "\"hidden_keep\":0,\"owned_keep\":0,\"metadata\":{\"version\":1},"
             "\"history_append\":\"%s\\n%s\\n\"}}\n",
             (unsigned long)bytes, length, encoded) < (int)sizeof(record));
  f.count = 1U;
  f.records[0] = record;
  f.ids[0] = "commit-00000000000000000001-00000000000000000000";
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = receive;
  sink.callbacks.context = &f;
  assert(cai_cli_journal_view_open(&f.collection, NULL, &view, NULL) == CAI_OK);
  cai_handover_config_init(&config);
  config.history.write = cai_cli_journal_view_history;
  config.history.context = view;
  assert(cai_handover_write(&config, &sink, NULL) == CAI_OK);
  assert(strstr(f.output, "## Assistant\n\nnative markdown\n\n"));
  assert(f.heads == 1U && f.calls == 3U);
  cai_cli_journal_view_close(view);
}
/** Scan across arbitrary output seams without retaining the handover. */
typedef struct markdown_scan {
  fixture *fixture;
  char tail[8192];
  size_t length;
  unsigned int seen;
} markdown_scan;
static const char *const markdown_needles[] = {
    "restored-model",
    "recorded-model",
    "GLOBAL_MARKDOWN_POLICY",
    "native markdown",
    "goal objective",
    "12345",
    "## Execution history",
    "first second",
    "Execution record 3 could not be decoded",
    "No active durable goal.",
    "unbounded",
    "LOCAL_MUST_NOT_LOAD"};
static int markdown_output(void *context, const void *data, size_t length,
                           cai_error *error) {
  markdown_scan *scan = context;
  size_t i;
  int rc = receive(scan->fixture, data, length, error);
  if (rc != CAI_OK)
    return rc;
  assert(length <= 4096U);
  if (scan->fixture->large && scan->fixture->producing) {
    /* These source chunks contain only generated text. Check every byte in
     * linear time instead of repeatedly searching for header words in them.
     * Goal decoding can legitimately forward one-byte fragments. */
    for (i = 0U; i < length; i++)
      assert(((const char *)data)[i] == 'x');
    scan->length = 0U;
    scan->tail[0] = '\0';
    return CAI_OK;
  }
  assert(scan->length + length < sizeof(scan->tail));
  memcpy(scan->tail + scan->length, data, length);
  scan->length += length;
  scan->tail[scan->length] = '\0';
  for (i = 0U; i < sizeof(markdown_needles) / sizeof(markdown_needles[0]); i++)
    if (strstr(scan->tail, markdown_needles[i]) != NULL)
      scan->seen |= 1U << i;
  /* Retain only the overlap required by the longest searched marker. */
  if (scan->length > 128U) {
    memmove(scan->tail, scan->tail + scan->length - 128U, 128U);
    scan->length = 128U;
    scan->tail[scan->length] = '\0';
  }
  return CAI_OK;
}
static void markdown_seed(fixture *f, char *record, size_t capacity,
                          const char *metadata) {
  static const char item[] =
      "[{\"content\":[{\"text\":\"native markdown\"}],\"role\":\"assistant\"}]";
  static const char encoded[] =
      "[{\\\"content\\\":[{\\\"text\\\":\\\"native markdown\\\"}],"
      "\\\"role\\\":\\\"assistant\\\"}]";
  char length[32];
  size_t bytes;
  init(f);
  f->capture = 0;
  f->read_limit = 10U;
  snprintf(length, sizeof(length), "%lu", (unsigned long)strlen(item));
  bytes = strlen(length) + 1U + strlen(item) + 1U;
  assert(snprintf(record, capacity,
                  "{\"record_type\":\"commit\",\"commit_created_at_ns\":1,"
                  "\"applied_event_sequence\":0,\"state\":{\"revision\":1,"
                  "\"base_bytes\":0,\"end_bytes\":%lu,\"blocks_keep\":0,"
                  "\"hidden_keep\":0,\"owned_keep\":0,\"metadata\":%s,"
                  "\"history_append\":\"%s\\n%s\\n\"}}\n",
                  (unsigned long)bytes, metadata, length,
                  encoded) < (int)capacity);
  f->count = 1U;
  f->records[0] = record;
  f->ids[0] = "commit-00000000000000000001-00000000000000000000";
}
static int markdown_project(fixture *f, cai_cli_pouch *pouch,
                            cai_cli_session *session, markdown_scan *scan) {
  cai_cli_journal_view *view = NULL;
  cai_sink sink;
  cai_error error;
  int rc;
  memset(scan, 0, sizeof(*scan));
  scan->fixture = f;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = markdown_output;
  sink.callbacks.context = scan;
  cai_error_init(&error);
  bounded = 1;
  rc = cai_cli_journal_view_open(&f->collection, NULL, &view, &error);
  if (rc == CAI_OK)
    rc = cai_cli_markdown_export(view, pouch, session, session->workspace, NULL,
                                 &sink, &error);
  cai_cli_journal_view_close(view);
  cai_error_cleanup(&error);
  bounded = 0;
  assert(live_bytes == 0U);
  return rc;
}
/** Exercise the application adapter itself, including instruction policy and
 * bounded execution decoding, on the same default-stack worker as the view. */
static void application_markdown(void) {
  static const char *const invalid[] = {
      "{\"version\":1,\"model\":true}",
      "{\"version\":1,\"model\":\"bad\\u0000model\"}",
      "{\"version\":1,\"reasoning_effort\":[]}",
      "{\"version\":1,\"preset_name\":\"other\"}",
      "{\"version\":1,\"goal_status\":\"unknown\"}",
      "{\"version\":1,\"goal_tokens_used\":-1}",
      "{\"version\":1,\"goal_token_budget\":123}",
      "{\"version\":1,\"goal_objective\":\"x\",\"goal_token_budget\":0}",
      "{\"version\":1,\"goal_objective\":\"x\",\"goal_tokens_used\":1.5}",
      "{\"version\":1,\"goal_tokens_used\":9223372036854775808}"};
  static const char execution1[] =
      "{\"record_type\":\"event\",\"sequence\":1,\"type\":\"execution\","
      "\"data\":\"{\\\"kind\\\":\\\"tool.output\\\",\\\"session_id\\\":"
      "\\\"test\\\","
      "\\\"timestamp_ns\\\":1,\\\"offset\\\":0,\\\"final\\\":false,"
      "\\\"line_start\\\":true,\\\"field\\\":\\\"output\\\","
      "\\\"tool_name\\\":\\\"read_file\\\",\\\"data\\\":\\\"first \\\"}\"}\n";
  static const char execution2[] =
      "{\"record_type\":\"event\",\"sequence\":2,\"type\":\"execution\","
      "\"data\":\"{\\\"kind\\\":\\\"tool.output\\\",\\\"session_id\\\":"
      "\\\"test\\\","
      "\\\"timestamp_ns\\\":1,\\\"offset\\\":6,\\\"final\\\":true,"
      "\\\"line_start\\\":false,\\\"field\\\":\\\"output\\\","
      "\\\"tool_name\\\":\\\"read_file\\\",\\\"data\\\":\\\"second\\\"}\"}\n";
  static const char bad_execution[] =
      "{\"record_type\":\"event\",\"sequence\":3,\"type\":\"execution\","
      "\"data\":\"not-json\"}\n";
  fixture f;
  markdown_scan scan;
  cai_cli_pouch *pouch = calloc(1U, sizeof(*pouch));
  cai_cli_session *session = calloc(1U, sizeof(*session));
  char directory[] = "cai-markdown-XXXXXX", path[512], record[4096];
  FILE *file;
  cai_execution_record warm;
  size_t i, chunk;
  /* Initialize the process-wide JSON engines before tracking per-export
   * ownership. Their lifetime intentionally extends past a single export. */
  cai_execution_record_init(&warm);
  assert(cai_execution_record_parse(
             "{\"kind\":\"tool.completed\",\"session_id\":\"test\","
             "\"timestamp_ns\":1,\"offset\":0,\"final\":true,"
             "\"line_start\":false,\"data\":\"\"}",
             &warm, NULL) == CAI_OK);
  cai_execution_record_cleanup(&warm);
  assert(pouch && session && mkdtemp(directory));
  strcpy(pouch->program_name, "cai");
  strcpy(pouch->config_directory, directory);
  strcpy(session->id, "test");
  snprintf(session->workspace, sizeof(session->workspace), "%s/workspace",
           directory);
  assert(mkdir(session->workspace, 0700) == 0);
  snprintf(path, sizeof(path), "%s/AGENTS.md", directory);
  file = fopen(path, "w");
  assert(file && fputs("GLOBAL_MARKDOWN_POLICY\n", file) >= 0 &&
         fclose(file) == 0);
  snprintf(path, sizeof(path), "%s/workspace/AGENTS.md", directory);
  file = fopen(path, "w");
  assert(file && fputs("LOCAL_MUST_NOT_LOAD\n", file) >= 0 &&
         fclose(file) == 0);
  for (chunk = 1U; chunk <= 23U; chunk += 11U) {
    markdown_seed(&f, record, sizeof(record),
                  "{\"version\":1,\"model\":\"recorded-model\","
                  "\"request_model\":\"restored-model\","
                  "\"reasoning_effort\":\"low\",\"goal_status\":\"paused\","
                  "\"goal_objective\":\"goal objective\","
                  "\"goal_token_budget\":12345,\"goal_tokens_used\":30,"
                  "\"goal_elapsed_seconds\":-5,\"goal_active_started_at\":-1}");
    f.chunk = chunk;
    assert(markdown_project(&f, pouch, session, &scan) == CAI_OK);
    assert((scan.seen & 63U) == 63U && !(scan.seen & (1U << 11U)));
    assert(f.heads == 1U && f.calls == 6U);
  }
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    markdown_seed(&f, record, sizeof(record), invalid[i]);
    assert(markdown_project(&f, pouch, session, &scan) != CAI_OK);
    assert(f.received == 0U);
  }
  markdown_seed(&f, record, sizeof(record), "{\"version\":1}");
  assert(markdown_project(&f, pouch, session, &scan) == CAI_OK);
  assert(scan.seen & (1U << 9U));
  f.records[1] = execution1;
  f.ids[1] = "event-00000000000000000001";
  f.records[2] = execution2;
  f.ids[2] = "event-00000000000000000002";
  f.records[3] = bad_execution;
  f.ids[3] = "event-00000000000000000003";
  f.count = 4U;
  f.calls = f.heads = 0U;
  assert(markdown_project(&f, pouch, session, &scan) == CAI_OK);
  assert((scan.seen & (7U << 6U)) == (7U << 6U));
  markdown_seed(&f, record, sizeof(record), "{\"version\":1}");
  f.failed_read = 1;
  assert(markdown_project(&f, pouch, session, &scan) != CAI_OK);
  markdown_seed(&f, record, sizeof(record), "{\"version\":1}");
  f.fail_sink = 1;
  assert(markdown_project(&f, pouch, session, &scan) == CAI_ERR_CANCELLED);
  for (i = 0U; i < 16U; i++) {
    markdown_seed(&f, record, sizeof(record), "{\"version\":1}");
    allocation_attempts = 0U;
    allocation_fail_at = i;
    assert(markdown_project(&f, pouch, session, &scan) != CAI_OK);
  }
  allocation_fail_at = SIZE_MAX;
  {
    char *setting = malloc(9216U), *commit = malloc(12288U);
    assert(setting && commit);
    memcpy(setting, "{\"version\":1,\"model\":\"", 22U);
    memset(setting + 22U, 'x', 8192U);
    strcpy(setting + 22U + 8192U, "\"}");
    markdown_seed(&f, commit, 12288U, setting);
    assert(markdown_project(&f, pouch, session, &scan) == CAI_ERR_LIMIT);
    free(commit);
    free(setting);
  }
  /* Goal and provider text are independently generated 8 MiB values. Neither
   * can fit the allocation budget, and writes occur during source delivery. */
  init(&f);
  f.count = 1U;
  f.large = f.large_markdown = 1;
  f.capture = 0;
  f.chunk = 4096U;
  f.read_limit = 10U;
  f.ids[0] = "commit-00000000000000000001-00000000000000000000";
  assert(markdown_project(&f, pouch, session, &scan) == CAI_OK);
  assert(f.observed_flow && f.delivered >= 2U * generated_bytes);
  assert(scan.seen & (1U << 10U));
  assert(f.heads == 1U && f.calls == 6U && peak_bytes < 1048576U);
  snprintf(path, sizeof(path), "%s/workspace/AGENTS.md", directory);
  assert(unlink(path) == 0 && rmdir(session->workspace) == 0);
  snprintf(path, sizeof(path), "%s/AGENTS.md", directory);
  assert(unlink(path) == 0 && rmdir(directory) == 0);
  free(pouch);
  free(session);
}
static int view_cancelled(void *context) {
  fixture *f = context;
  return f->callback_cancelled ||
         (f->cancel_after != 0U && f->produced >= f->cancel_after);
}
static void view_cancellation(void) {
  fixture f;
  cai_cli_journal_view *view = NULL;
  cai_cli_journal_options options;
  cai_sink sink;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = receive;
  sink.callbacks.context = &f;
  memset(&options, 0, sizeof(options));
  options.cancelled = view_cancelled;
  options.cancel_context = &f;
  seed_fixture(&f);
  f.callback_cancelled = 1;
  assert(cai_cli_journal_view_open(&f.collection, &options, &view, NULL) ==
         CAI_ERR_CANCELLED);
  assert(view == NULL && f.heads == 0U && f.calls == 0U);
  seed_fixture(&f);
  assert(cai_cli_journal_view_open(&f.collection, &options, &view, NULL) ==
         CAI_OK);
  options.cancelled = NULL; /* The view owns its callback table copy. */
  f.callback_cancelled = 1;
  assert(cai_cli_journal_view_history(view, &sink, NULL) == CAI_ERR_CANCELLED);
  assert(cai_cli_journal_view_jsonl(view, &sink, NULL) == CAI_ERR_CANCELLED);
  assert(f.heads == 1U && f.calls == 1U && f.sdk_cancel_checks == 1U);
  cai_cli_journal_view_close(view);
  view = NULL;
  options.cancelled = view_cancelled;
  init(&f);
  f.count = 1U;
  f.large = 1;
  f.capture = 0;
  f.chunk = 4096U;
  f.cancel_after = 8192U;
  f.ids[0] = "commit-00000000000000000001-00000000000000000000";
  assert(cai_cli_journal_view_open(&f.collection, &options, &view, NULL) ==
         CAI_ERR_CANCELLED);
  assert(view == NULL && f.heads == 1U && f.calls == 1U && f.produced == 8192U);
  f.cancel_after = 0U;
  f.produced = f.calls = f.heads = 0U;
  assert(cai_cli_journal_view_open(&f.collection, &options, &view, NULL) ==
         CAI_OK);
  f.cancel_after = f.produced + 8192U;
  assert(cai_cli_journal_view_history(view, &sink, NULL) == CAI_ERR_CANCELLED);
  assert(cai_cli_journal_view_metadata(view, &sink, NULL) == CAI_ERR_CANCELLED);
  assert(f.calls == 2U && f.produced == f.cancel_after);
  cai_cli_journal_view_close(view);
}
static void *worker(void *unused) {
  fixture f;
  (void)unused;
  edits();
  immutable_prefix();
  errors();
  checkpoint();
  views();
  view_failures();
  native_markdown();
  application_markdown();
  view_cancellation();
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
  assert(f.observed_flow && f.delivered >= 2U * generated_bytes &&
         f.calls == 4U);
  /* One captured native view can feed multiple projections without retaining
   * the generated metadata/history values or reopening its collection. */
  {
    cai_cli_journal_view *view = NULL;
    cai_sink sink;
    init(&f);
    f.count = 1U;
    f.large = 1;
    f.capture = 0;
    f.chunk = 4096U;
    f.read_limit = 8U;
    f.ids[0] = "commit-00000000000000000001-00000000000000000000";
    memset(&sink, 0, sizeof(sink));
    sink.callbacks.write = receive;
    sink.callbacks.context = &f;
    bounded = 1;
    assert(cai_cli_journal_view_open(&f.collection, NULL, &view, NULL) ==
           CAI_OK);
    assert(cai_cli_journal_view_metadata(view, &sink, NULL) == CAI_OK);
    assert(cai_cli_journal_view_history(view, &sink, NULL) == CAI_OK);
    assert(cai_cli_journal_view_history(view, &sink, NULL) == CAI_OK);
    assert(cai_cli_journal_view_jsonl(view, &sink, NULL) == CAI_OK);
    cai_cli_journal_view_close(view);
    bounded = 0;
    assert(live_bytes == 0U && peak_bytes < 1048576U);
    assert(f.observed_flow && f.delivered >= 5U * generated_bytes);
    assert(f.heads == 1U && f.calls == 7U);
  }
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  assert(argc == 1 || (argc == 2 && strcmp(argv[1], "--memory-check") == 0));
  if (argc == 2)
    generated_bytes = 128U * 1024U;
  assert(snprintf(final, sizeof(final), "%s%s", final_prefix, final_suffix) <
         (int)sizeof(final));
  /* Default musl worker stack, never an enlarged export stack. */
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
