#define _POSIX_C_SOURCE 200809L
#include "../cli/journal_projection.h"
#include "../src/cai_input_queue_codec.h"
#include <assert.h>
#include <cai/handover.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bounded_allocator.h"

/* The fake receiver implements the same authenticated entry seams and head
 * precondition as LC. Payload generation remains bounded and synchronous. */
typedef struct fixture {
  lc_collection collection;
  const char *records[8], *ids[8];
  size_t count, chunk, calls, produced, received, delivered, heads, read_limit;
  char output[8192];
  size_t length, cancel_after, sdk_cancel_checks;
  int large, capture, producing, observed_flow, failed_read, changed_head,
      cancelled, no_boundary, incomplete, fail_sink, callback_cancelled;
} fixture;
static int head(lc_collection *collection, lc_collection_head_res *out,
                lc_error *error) {
  fixture *f = collection->impl;
  f->heads++;
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
  assert(f->calls <= f->read_limit); /* Never a read per context item. */
  assert(options->expected_head != NULL &&
         strcmp(options->expected_head, "stable-head") == 0);
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
  errors();
  checkpoint();
  views();
  view_failures();
  native_markdown();
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
  assert(f.observed_flow && f.delivered > 16000000U && f.calls == 4U);
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
    assert(f.observed_flow && f.delivered >= 5U * 8U * 1024U * 1024U);
    assert(f.heads == 1U && f.calls == 7U);
  }
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
