#include "../src/cai_input_queue_codec.h"
#include <assert.h>
#include <cai/input_queue.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 256U
typedef struct queue_fixture {
  cai_input_queue *queue;
  cai_input_queue_client *clients[2];
  unsigned long long requests[2], view;
  cai_input_record records[RECORDS];
  char *request_text[RECORDS], *entry_text[RECORDS];
  size_t count, changes;
  int fail;
} queue_fixture;
static char *copy(const char *text) {
  char *out;
  if (text == NULL)
    return NULL;
  out = malloc(strlen(text) + 1U);
  assert(out != NULL);
  strcpy(out, text);
  return out;
}
static int store(void *context, const cai_input_record *record,
                 unsigned long long *position, cai_error *error) {
  queue_fixture *fixture = context;
  cai_input_record parsed;
  char *json = NULL;
  size_t at = fixture->count;
  assert(position != NULL && error != NULL);
  if (fixture->fail == 1)
    return CAI_ERR_TRANSPORT;
  assert(at < RECORDS);
  memset(&parsed, 0, sizeof(parsed));
  assert(cai_input_record_json(record, &json, error) == CAI_OK);
  assert(cai_input_record_parse(json, &parsed, error) == CAI_OK);
  free(json);
  fixture->records[at] = parsed;
  fixture->request_text[at] = copy(parsed.request.text);
  fixture->entry_text[at] = copy(parsed.entry.text);
  fixture->records[at].request.text = fixture->request_text[at];
  fixture->records[at].entry.text = fixture->entry_text[at];
  cai_input_record_cleanup(&parsed);
  *position = ++fixture->count;
  return fixture->fail == 2 ? CAI_ERR_TRANSPORT : CAI_OK;
}
static void changed(void *context, const cai_input_change *event) {
  queue_fixture *fixture = context;
  assert(event->previous_view_revision == fixture->view);
  fixture->view = event->state.view_revision;
  assert(fixture->view == event->previous_view_revision + 1U);
  ++fixture->changes;
}
static int classify(void *context, const char *text, cai_input_policy *policy,
                    cai_error *error) {
  (void)context;
  assert(error != NULL);
  policy->action = text[0] == '/';
  policy->independent = policy->action && strcmp(text, "/turn") != 0;
  policy->resume_backlog = !policy->action;
  if (strcmp(text, "/turn") == 0)
    policy->intent = CAI_INPUT_QUEUED;
  return CAI_OK;
}
static void open_queue(queue_fixture *fixture, size_t entries, size_t bytes) {
  cai_input_queue_config config;
  cai_input_queue_config_init(&config);
  config.max_entries = entries;
  config.max_text_bytes = bytes;
  config.persist = store;
  config.persist_context = fixture;
  config.changed = changed;
  config.changed_context = fixture;
  config.classify = classify;
  assert(cai_input_queue_open(&config, &fixture->queue, NULL) == CAI_OK);
  fixture->view = 0U;
}
static void close_queue(queue_fixture *fixture) {
  size_t i;
  for (i = 0U; i < 2U; ++i) {
    cai_input_queue_client_close(fixture->clients[i]);
    fixture->clients[i] = NULL;
  }
  cai_input_queue_close(fixture->queue);
  fixture->queue = NULL;
}
static void clear(queue_fixture *fixture) {
  size_t i;
  close_queue(fixture);
  for (i = 0U; i < fixture->count; ++i) {
    free(fixture->request_text[i]);
    free(fixture->entry_text[i]);
  }
  memset(fixture, 0, sizeof(*fixture));
}
static cai_input_state state(queue_fixture *fixture) {
  cai_input_state out;
  assert(cai_input_queue_get_state(fixture->queue, &out, NULL) == CAI_OK);
  return out;
}
static cai_input_entry entry(queue_fixture *fixture, size_t client,
                             size_t index) {
  cai_input_entry out;
  assert(cai_input_queue_entry_at(fixture->clients[client], index, &out,
                                  NULL) == CAI_OK);
  return out;
}
static cai_input_result send(queue_fixture *fixture, size_t client,
                             cai_input_operation operation, const char *text,
                             const cai_input_entry *item,
                             cai_input_intent intent, int held) {
  cai_input_request request;
  cai_input_result result;
  memset(&request, 0, sizeof(request));
  request.request_id = ++fixture->requests[client];
  request.operation = operation;
  request.text = text;
  request.intent = intent;
  request.enabled = held;
  request.queue_revision = state(fixture).revision;
  if (item != NULL) {
    request.item_id = item->id;
    request.item_revision = item->revision;
    request.edit_token = item->edit_token;
  }
  assert(cai_input_queue_request(fixture->clients[client], &request, &result,
                                 NULL) == CAI_OK);
  return result;
}
static void clients(queue_fixture *fixture) {
  size_t i;
  for (i = 0U; i < 2U; ++i) {
    assert(cai_input_queue_client_open(fixture->queue, &fixture->clients[i],
                                       NULL) == CAI_OK);
    assert(cai_input_queue_client_id(fixture->clients[i]) != 0U);
  }
}
static void edit_and_claim(queue_fixture *fixture) {
  cai_input_result offered, result;
  cai_input_request duplicate;
  cai_input_entry item, hidden;
  cai_input_claim claim;
  size_t records, i;
  open_queue(fixture, 0U, 0U);
  clients(fixture);
  offered =
      send(fixture, 0U, CAI_INPUT_OFFER, "original", NULL, CAI_INPUT_QUEUED, 0);
  assert(offered.outcome == CAI_INPUT_ACCEPTED && state(fixture).count == 1U);
  item = entry(fixture, 0U, 0U);
  assert(!item.ready && !item.held);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_ERR_INVALID);
  assert(cai_input_queue_set_ready(fixture->queue, item.id, 1, NULL) == CAI_OK);
  result =
      send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &item, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED && result.edit_token != 0U);
  item = entry(fixture, 0U, 0U);
  hidden = entry(fixture, 1U, 0U);
  assert(item.edit_held && !item.ready && item.edit_token != 0U &&
         hidden.edit_token == 0U);
  assert(send(fixture, 1U, CAI_INPUT_END_EDIT, "unauthorized", &hidden,
              CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_REJECTED);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_ERR_INVALID);
  assert(send(fixture, 0U, CAI_INPUT_END_EDIT, "edited", &item,
              CAI_INPUT_STEERING, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  item = entry(fixture, 0U, 0U);
  assert(strcmp(item.text, "edited") == 0 && !item.edit_held && !item.ready &&
         item.intent == CAI_INPUT_STEERING);
  assert(cai_input_queue_set_ready(fixture->queue, item.id, 1, NULL) == CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, item.id, offered.item_revision,
                               &claim, NULL) == CAI_ERR_INVALID);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_OK);
  assert(strcmp(claim.text, "edited") == 0 &&
         claim.position == fixture->count && state(fixture).count == 0U);
  cai_input_claim_cleanup(&claim);
  records = fixture->count;
  memset(&duplicate, 0, sizeof(duplicate));
  duplicate.request_id = offered.request_id;
  duplicate.operation = CAI_INPUT_OFFER;
  duplicate.text = "must not replace the original request";
  assert(cai_input_queue_request(fixture->clients[0], &duplicate, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_ACCEPTED &&
         result.item_id == offered.item_id && fixture->count == records &&
         state(fixture).count == 0U);
  for (i = 0U; i < 17U; ++i)
    assert(send(fixture, 0U, CAI_INPUT_SYNCHRONIZE, NULL, NULL,
                CAI_INPUT_QUEUED, 0)
               .outcome == CAI_INPUT_ACCEPTED);
  assert(cai_input_queue_request(fixture->clients[0], &duplicate, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_UNKNOWN && state(fixture).count == 0U);
  close_queue(fixture);
  open_queue(fixture, 0U, 0U);
  for (i = 0U; i < records; ++i)
    assert(cai_input_queue_restore(fixture->queue, &fixture->records[i],
                                   (unsigned long long)i + 1U, NULL) == CAI_OK);
  assert(state(fixture).count == 0U);
  clear(fixture);
}
static void held_recovery(queue_fixture *fixture) {
  cai_input_entry first, second;
  cai_input_result result;
  size_t count, i;
  open_queue(fixture, 2U, 8U);
  clients(fixture);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "first", NULL, CAI_INPUT_QUEUED, 1)
             .outcome == CAI_INPUT_ACCEPTED);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "second", NULL, CAI_INPUT_QUEUED, 1)
             .outcome == CAI_INPUT_ACCEPTED);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "third", NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_REJECTED);
  first = entry(fixture, 0U, 0U);
  second = entry(fixture, 0U, 1U);
  assert(
      send(fixture, 1U, CAI_INPUT_APPROVE, NULL, &second, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_REJECTED);
  assert(send(fixture, 1U, CAI_INPUT_BEGIN_EDIT, NULL, &second,
              CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_REJECTED);
  result = send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &first,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  count = fixture->count;
  close_queue(fixture);
  open_queue(fixture, 2U, 8U);
  for (i = 0U; i < count; ++i)
    assert(cai_input_queue_restore(fixture->queue, &fixture->records[i],
                                   (unsigned long long)i + 1U, NULL) == CAI_OK);
  clients(fixture);
  first = entry(fixture, 0U, 0U);
  assert(first.held && first.edit_held && first.edit_token == 0U &&
         !first.ready);
  result = send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &first,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED && result.edit_token != 0U);
  first = entry(fixture, 0U, 0U);
  assert(
      send(fixture, 0U, CAI_INPUT_ABORT_EDIT, NULL, &first, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  first = entry(fixture, 0U, 0U);
  assert(first.held && !first.edit_held);
  assert(send(fixture, 0U, CAI_INPUT_APPROVE, NULL, &first, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  second = entry(fixture, 0U, 1U);
  assert(
      send(fixture, 0U, CAI_INPUT_APPROVE, NULL, &second, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  clear(fixture);
}
static void uncertain_appends(queue_fixture *fixture) {
  cai_input_result result;
  cai_input_request request;
  cai_input_entry item;
  cai_input_claim claim;
  size_t committed;
  open_queue(fixture, 0U, 0U);
  clients(fixture);
  fixture->fail = 2;
  result = send(fixture, 0U, CAI_INPUT_OFFER, "committed", NULL,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_UNKNOWN && state(fixture).uncertain &&
         state(fixture).count == 0U);
  committed = fixture->count - 1U;
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "not admitted", NULL,
              CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_REJECTED);
  fixture->fail = 0;
  assert(cai_input_queue_restore(fixture->queue, &fixture->records[committed],
                                 (unsigned long long)committed + 1U,
                                 NULL) == CAI_OK &&
         !state(fixture).uncertain && state(fixture).count == 1U);
  memset(&request, 0, sizeof(request));
  request.request_id = ++fixture->requests[0];
  request.operation = CAI_INPUT_RESOLVE;
  request.target_request_id = result.request_id;
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_ACCEPTED);
  assert(cai_input_queue_confirm_absent(fixture->queue, NULL) ==
         CAI_ERR_INVALID);
  committed = fixture->count;
  fixture->fail = 1;
  result =
      send(fixture, 0U, CAI_INPUT_OFFER, "missing", NULL, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_UNKNOWN && state(fixture).uncertain &&
         fixture->count == committed && state(fixture).count == 1U);
  fixture->fail = 0;
  assert(cai_input_queue_confirm_absent(fixture->queue, NULL) == CAI_OK);
  request.request_id = ++fixture->requests[0];
  request.target_request_id = result.request_id;
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_REJECTED && !state(fixture).uncertain &&
         state(fixture).count == 1U);
  item = entry(fixture, 0U, 0U);
  assert(cai_input_queue_set_ready(fixture->queue, item.id, 1, NULL) == CAI_OK);
  fixture->fail = 2;
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_ERR_TRANSPORT &&
         claim.text == NULL && state(fixture).uncertain &&
         state(fixture).count == 1U);
  fixture->fail = 0;
  assert(cai_input_queue_restore(
             fixture->queue, &fixture->records[fixture->count - 1U],
             (unsigned long long)fixture->count, NULL) == CAI_OK &&
         state(fixture).count == 0U);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_ERR_INVALID);
  clear(fixture);
}
static void lanes_and_stop(queue_fixture *fixture) {
  cai_input_entry first, second, steer, inspection;
  cai_input_claim claim;
  cai_input_request request;
  cai_input_result result;
  open_queue(fixture, 4U, 16U);
  clients(fixture);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "first", NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "second", NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  assert(
      send(fixture, 1U, CAI_INPUT_OFFER, "steer", NULL, CAI_INPUT_STEERING, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  assert(
      send(fixture, 1U, CAI_INPUT_OFFER, "/status", NULL, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  first = entry(fixture, 0U, 0U);
  second = entry(fixture, 0U, 1U);
  steer = entry(fixture, 0U, 2U);
  inspection = entry(fixture, 0U, 3U);
  assert(
      cai_input_queue_set_ready(fixture->queue, first.id, 1, NULL) == CAI_OK &&
      cai_input_queue_set_ready(fixture->queue, second.id, 1, NULL) == CAI_OK &&
      cai_input_queue_set_ready(fixture->queue, steer.id, 1, NULL) == CAI_OK &&
      cai_input_queue_set_ready(fixture->queue, inspection.id, 1, NULL) ==
          CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, second.id, second.revision,
                               &claim, NULL) == CAI_ERR_INVALID);
  assert(cai_input_queue_claim(fixture->queue, first.id, first.revision, &claim,
                               NULL) == CAI_ERR_INVALID);
  assert(cai_input_queue_claim(fixture->queue, inspection.id,
                               inspection.revision, &claim, NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(cai_input_queue_claim(fixture->queue, steer.id, steer.revision, &claim,
                               NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(cai_input_queue_claim(fixture->queue, second.id, second.revision,
                               &claim, NULL) == CAI_ERR_INVALID);
  memset(&request, 0, sizeof(request));
  request.request_id = ++fixture->requests[0];
  request.operation = CAI_INPUT_STOP;
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_ACCEPTED);
  assert(state(fixture).stopped && !entry(fixture, 0U, 0U).ready);
  /* Fresh local commands can run without releasing the cancelled backlog,
   * including turn-only commands. Old independent commands stop too. */
  assert(
      send(fixture, 0U, CAI_INPUT_OFFER, "/status", NULL, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(!inspection.stopped && entry(fixture, 0U, 0U).stopped);
  assert(send(fixture, 0U, CAI_INPUT_STOP, NULL, NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(inspection.independent && inspection.stopped && !inspection.ready);
  assert(send(fixture, 0U, CAI_INPUT_OFFER, "/turn", NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 3U);
  assert(cai_input_queue_set_ready(fixture->queue, inspection.id, 1, NULL) ==
         CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, inspection.id,
                               inspection.revision, &claim, NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  /* Promotion releases only that item, preserving the rest of the stop. */
  inspection = entry(fixture, 0U, 2U);
  assert(send(fixture, 0U, CAI_INPUT_SET_INTENT, NULL, &inspection,
              CAI_INPUT_STEERING, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(!inspection.stopped && entry(fixture, 0U, 0U).stopped &&
         state(fixture).stopped);
  assert(cai_input_queue_set_ready(fixture->queue, inspection.id, 1, NULL) ==
         CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, inspection.id,
                               inspection.revision, &claim, NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(
      send(fixture, 0U, CAI_INPUT_OFFER, "/status", NULL, CAI_INPUT_QUEUED, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  assert(send(fixture, 0U, CAI_INPUT_STOP, NULL, NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &inspection,
              CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(send(fixture, 0U, CAI_INPUT_END_EDIT, "/status", &inspection,
              CAI_INPUT_STEERING, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  inspection = entry(fixture, 0U, 2U);
  assert(!inspection.stopped && entry(fixture, 0U, 0U).stopped &&
         state(fixture).stopped);
  assert(cai_input_queue_set_ready(fixture->queue, inspection.id, 1, NULL) ==
         CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, inspection.id,
                               inspection.revision, &claim, NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  request.request_id = ++fixture->requests[0];
  request.operation = CAI_INPUT_RESUME;
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK &&
         result.outcome == CAI_INPUT_REJECTED);
  assert(send(fixture, 0U, CAI_INPUT_RESUME, NULL, NULL, CAI_INPUT_QUEUED, 0)
             .outcome == CAI_INPUT_ACCEPTED);
  assert(cai_input_queue_claim(fixture->queue, first.id, first.revision, &claim,
                               NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(cai_input_queue_claim(fixture->queue, second.id, second.revision,
                               &claim, NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  assert(send(fixture, 0U, CAI_INPUT_OFFER,
              "this text exceeds the configured limit", NULL, CAI_INPUT_QUEUED,
              0)
             .outcome == CAI_INPUT_REJECTED);
  assert(fixture->records[fixture->count - 1U].request.text == NULL);
  assert(
      send(fixture, 0U, CAI_INPUT_OFFER, "/turn", NULL, CAI_INPUT_STEERING, 0)
          .outcome == CAI_INPUT_ACCEPTED);
  assert(entry(fixture, 0U, 0U).intent == CAI_INPUT_QUEUED);
  assert(send(fixture, 0U, CAI_INPUT_CLEAR, NULL, NULL, CAI_INPUT_QUEUED, 0)
                 .outcome == CAI_INPUT_ACCEPTED &&
         state(fixture).count == 0U);
  clear(fixture);
}
static void producer_receipts(queue_fixture *fixture) {
  cai_input_request request;
  cai_input_result result;
  cai_input_entry item;
  cai_input_origin_status status;
  cai_input_claim claim;
  size_t i, records;
  open_queue(fixture, 0U, 0U);
  clients(fixture);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.text = "incoming durable message";
  strcpy(request.origin, "message:opaque-id");
  request.request_id = ++fixture->requests[0];
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK);
  assert(result.outcome == CAI_INPUT_ACCEPTED && state(fixture).count == 1U);
  assert(cai_input_queue_find_origin(fixture->queue, request.origin, &item,
                                     &status, NULL) == CAI_OK);
  assert(status == CAI_INPUT_ORIGIN_PENDING);
  request.request_id = ++fixture->requests[1];
  request.text = "a retry cannot replace the accepted message";
  assert(cai_input_queue_request(fixture->clients[1], &request, &result,
                                 NULL) == CAI_OK);
  assert(result.outcome == CAI_INPUT_ACCEPTED && state(fixture).count == 1U);
  assert(cai_input_queue_set_ready(fixture->queue, item.id, 1, NULL) == CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_OK);
  assert(strcmp(claim.text, "incoming durable message") == 0);
  cai_input_claim_cleanup(&claim);
  request.request_id = ++fixture->requests[0];
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK);
  assert(result.outcome == CAI_INPUT_ACCEPTED && state(fixture).count == 0U);
  assert(cai_input_queue_find_origin(fixture->queue, request.origin, &item,
                                     &status, NULL) == CAI_OK);
  assert(status == CAI_INPUT_ORIGIN_CLAIMED && item.text == NULL);
  records = fixture->count;
  close_queue(fixture);
  open_queue(fixture, 0U, 0U);
  for (i = 0U; i < records; ++i)
    assert(cai_input_queue_restore(fixture->queue, &fixture->records[i],
                                   (unsigned long long)i + 1U, NULL) == CAI_OK);
  assert(cai_input_queue_find_origin(fixture->queue, request.origin, &item,
                                     &status, NULL) == CAI_OK);
  assert(status == CAI_INPUT_ORIGIN_CLAIMED && item.text == NULL);
  clients(fixture);
  request.request_id = ++fixture->requests[0];
  request.operation = CAI_INPUT_FORGET_ORIGIN;
  request.text = NULL;
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  assert(cai_input_queue_find_origin(fixture->queue, request.origin, &item,
                                     &status, NULL) == CAI_ERR_INVALID);
  clear(fixture);
}
static void checkpoint_restore(queue_fixture *fixture) {
  cai_input_queue_config config;
  cai_input_checkpoint_restore *parser = NULL;
  cai_source *source = NULL;
  cai_input_queue *restored = NULL;
  cai_input_request request;
  cai_input_result result;
  cai_input_entry item;
  cai_input_origin_status status;
  cai_input_claim claim;
  cai_input_state before, after;
  const unsigned long long *events;
  size_t event_count;
  cai_error error;
  unsigned long long through;
  char bytes[17];
  size_t n, records;
  static const char bad[] =
      "{\"revision\":0,\"serial\":0,\"client_serial\":0,\"token\":0,"
      "\"through\":42,\"pending_count\":1,\"receipt_count\":0,\"stopped\":"
      "false,\"auto_approval\":false,\"pending\":[],\"receipts\":[]}";
  cai_error_init(&error);
  open_queue(fixture, 0U, 0U);
  clients(fixture);
  result = send(fixture, 0U, CAI_INPUT_OFFER, "held at cut", NULL,
                CAI_INPUT_QUEUED, 1);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  result = send(fixture, 0U, CAI_INPUT_OFFER, "editing at cut", NULL,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  item = entry(fixture, 0U, 1U);
  result =
      send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &item, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  item = entry(fixture, 0U, 1U);
  result = send(fixture, 0U, CAI_INPUT_END_EDIT, "edited at cut", &item,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  item = entry(fixture, 0U, 1U);
  result =
      send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &item, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  memset(&request, 0, sizeof(request));
  request.operation = CAI_INPUT_OFFER;
  request.request_id = ++fixture->requests[0];
  request.text = "receipt survives the cut";
  strcpy(request.origin, "message:at-cut");
  assert(cai_input_queue_request(fixture->clients[0], &request, &result,
                                 NULL) == CAI_OK);
  item = entry(fixture, 0U, 2U);
  assert(cai_input_queue_set_ready(fixture->queue, item.id, 1, NULL) == CAI_OK);
  assert(cai_input_queue_claim(fixture->queue, item.id, item.revision, &claim,
                               NULL) == CAI_OK);
  cai_input_claim_cleanup(&claim);
  result = send(fixture, 0U, CAI_INPUT_STOP, NULL, NULL, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  before = state(fixture);
  cai_input_queue_config_init(&config);
  config.persist = store;
  config.persist_context = fixture;
  assert(cai_input_checkpoint_source(fixture->queue, 42U, &source, &error) ==
         CAI_OK);
  assert(cai_input_checkpoint_begin(&config, &parser, &error) == CAI_OK);
  while ((n = cai_source_read(source, bytes, sizeof(bytes), &error)) != 0U)
    assert(cai_input_checkpoint_feed(parser, bytes, n, &error) == CAI_OK);
  assert(error.code == CAI_OK);
  assert(cai_input_checkpoint_finish(parser, &restored, &through, &error) ==
         CAI_OK);
  assert(through == 42U);
  cai_source_close(source);
  source = NULL;
  cai_input_checkpoint_close(parser);
  parser = NULL;
  assert(cai_input_queue_get_state(restored, &after, &error) == CAI_OK);
  assert(after.count == before.count && after.revision == before.revision &&
         after.stopped);
  assert(cai_input_queue_find_origin(restored, request.origin, &item, &status,
                                     &error) == CAI_OK);
  assert(status == CAI_INPUT_ORIGIN_CLAIMED && item.text == NULL);
  close_queue(fixture);
  fixture->queue = restored;
  clients(fixture);
  item = entry(fixture, 0U, 0U);
  assert(item.held && item.stopped && !item.ready);
  item = entry(fixture, 0U, 1U);
  assert(item.edit_held && !item.edit_token);
  assert(cai_input_queue_input_events(fixture->queue, item.id, &events,
                                      &event_count, &error) == CAI_OK);
  assert(event_count == 2U && events[0] != 0U && events[0] < events[1] &&
         events[1] <= through);
  records = fixture->count;
  result =
      send(fixture, 0U, CAI_INPUT_BEGIN_EDIT, NULL, &item, CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED && result.edit_token != 0U);
  assert(fixture->count == records + 1U);
  /* The generator detects mutation before following its borrowed node cursor.
   */
  assert(cai_input_checkpoint_source(fixture->queue, 43U, &source, &error) ==
         CAI_OK);
  result = send(fixture, 0U, CAI_INPUT_OFFER, "changed during source", NULL,
                CAI_INPUT_QUEUED, 0);
  assert(result.outcome == CAI_INPUT_ACCEPTED);
  assert(cai_source_read(source, bytes, sizeof(bytes), &error) == 0U &&
         error.code == CAI_ERR_INVALID);
  cai_source_close(source);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_input_checkpoint_begin(NULL, &parser, &error) == CAI_OK);
  assert(cai_input_checkpoint_feed(parser, bad, sizeof(bad) - 1U, &error) ==
         CAI_OK);
  restored = NULL;
  assert(cai_input_checkpoint_finish(parser, &restored, &through, &error) ==
         CAI_ERR_PROTOCOL);
  assert(restored == NULL);
  cai_input_checkpoint_close(parser);
  cai_error_cleanup(&error);
  clear(fixture);
}
static void delivery_projection(void) {
  cai_agent_session_event event, projected;
  cai_input_delivery delivery;
  char full[256], id[CAI_AGENT_SESSION_ID_MAX];
  static const char *const invalid[] = {
      "",
      "L",
      "X\n1\na\n1\n0\ntext",
      "L\n0\n\n1\n0\ntext",
      "L\n129\na\n1\n0\ntext",
      "L\n3\na",
      "L\n1\na\n0\n0\ntext",
      "L\n1\na\n18446744073709551616\n0\ntext",
      "L\n1\na\n1\n3\ntext",
      "L\n1\na\n1\n0\n"};
  size_t i;
  memset(&event, 0, sizeof(event));
  event.type = "input_queue_delivery";
  event.sequence = 91U;
  event.timestamp_ns = 123U;
  event.data = "C\n3\na\nb\n7\n1\nsteer\ntext";
  assert(cai_input_delivery_parse(&event, &delivery, NULL) == CAI_OK);
  assert(!delivery.local && strcmp(delivery.source, "a\nb") == 0 &&
         delivery.sequence == 7U);
  assert(cai_agent_session_event_project_input(&event, &projected, NULL) ==
         CAI_OK);
  assert(projected.sequence == 91U && projected.timestamp_ns == 123U &&
         strcmp(projected.type, "steering_queued") == 0 &&
         strcmp(projected.data, "steer\ntext") == 0);
  memset(id, 'a', sizeof(id) - 1U);
  id[sizeof(id) - 1U] = '\0';
  snprintf(full, sizeof(full), "L\n128\n%s\n9\n2\nqueued", id);
  event.data = full;
  assert(cai_input_delivery_parse(&event, &delivery, NULL) == CAI_OK &&
         delivery.local && strcmp(delivery.source, id) == 0);
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    event.data = invalid[i];
    assert(cai_agent_session_event_project_input(&event, &projected, NULL) ==
           CAI_ERR_PROTOCOL);
  }
  event.type = "ordinary";
  event.data = "borrowed";
  assert(cai_agent_session_event_project_input(&event, &projected, NULL) ==
             CAI_OK &&
         projected.data == event.data);
}
int main(void) {
  queue_fixture *fixture = calloc(1U, sizeof(*fixture));
  assert(fixture != NULL);
  delivery_projection();
  producer_receipts(fixture);
  checkpoint_restore(fixture);
  edit_and_claim(fixture);
  held_recovery(fixture);
  uncertain_appends(fixture);
  lanes_and_stop(fixture);
  free(fixture);
  return 0;
}
