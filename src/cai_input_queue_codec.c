#include "cai_input_queue_codec.h"
#include "cai_decimal.h"
#include "cai_internal.h"
#include <limits.h>
#include <string.h>

/** Enum widths are not a wire contract. Use explicit JSON integer storage. */
typedef struct queue_document {
  cai_input_record value;
  lonejson_int64 type, operation, intent, policy_intent, outcome, code,
      entry_intent;
  char *text;
  int request_text, entry_text;
} queue_document;
static const lonejson_field fields[] = {
    LONEJSON_FIELD_I64_REQ(queue_document, type, "type"),
    LONEJSON_FIELD_U64_REQ(queue_document, value.before_revision, "before"),
    LONEJSON_FIELD_U64_REQ(queue_document, value.after_revision, "after"),
    LONEJSON_FIELD_U64_REQ(queue_document, value.client_id, "client"),
    LONEJSON_FIELD_U64(queue_document, value.request.request_id, "request"),
    LONEJSON_FIELD_I64(queue_document, operation, "operation"),
    LONEJSON_FIELD_U64(queue_document, value.request.item_id, "item"),
    LONEJSON_FIELD_U64(queue_document, value.request.item_revision,
                       "item_revision"),
    LONEJSON_FIELD_U64(queue_document, value.request.queue_revision,
                       "queue_revision"),
    LONEJSON_FIELD_U64(queue_document, value.request.edit_token, "edit_token"),
    LONEJSON_FIELD_I64(queue_document, intent, "intent"),
    LONEJSON_FIELD_BOOL(queue_document, value.request.enabled, "enabled"),
    LONEJSON_FIELD_U64(queue_document, value.request.target_request_id,
                       "target"),
    LONEJSON_FIELD_STRING_FIXED(queue_document, value.request.origin, "origin",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(queue_document, policy_intent, "policy_intent"),
    LONEJSON_FIELD_BOOL(queue_document, value.policy.action, "action"),
    LONEJSON_FIELD_BOOL(queue_document, value.policy.independent,
                        "independent"),
    LONEJSON_FIELD_BOOL(queue_document, value.policy.resume_backlog,
                        "resume_backlog"),
    LONEJSON_FIELD_U64(queue_document, value.result.request_id,
                       "result_request"),
    LONEJSON_FIELD_U64(queue_document, value.result.item_id, "result_item"),
    LONEJSON_FIELD_U64(queue_document, value.result.item_revision,
                       "result_revision"),
    LONEJSON_FIELD_U64(queue_document, value.result.edit_token, "result_token"),
    LONEJSON_FIELD_I64(queue_document, outcome, "outcome"),
    LONEJSON_FIELD_I64(queue_document, code, "code"),
    LONEJSON_FIELD_STRING_FIXED(queue_document, value.result.reason, "reason",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(queue_document, value.entry.id, "entry"),
    LONEJSON_FIELD_U64(queue_document, value.entry.revision, "entry_revision"),
    LONEJSON_FIELD_U64(queue_document, value.entry.client_id, "entry_client"),
    LONEJSON_FIELD_U64(queue_document, value.entry.request_id, "entry_request"),
    LONEJSON_FIELD_U64(queue_document, value.entry.edit_owner, "entry_owner"),
    LONEJSON_FIELD_U64(queue_document, value.entry.edit_token, "entry_token"),
    LONEJSON_FIELD_I64(queue_document, entry_intent, "entry_intent"),
    LONEJSON_FIELD_BOOL(queue_document, value.entry.held, "held"),
    LONEJSON_FIELD_BOOL(queue_document, value.entry.stopped, "stopped"),
    LONEJSON_FIELD_BOOL(queue_document, value.entry.edit_held, "edit_held"),
    LONEJSON_FIELD_BOOL(queue_document, value.entry.action, "entry_action"),
    LONEJSON_FIELD_BOOL(queue_document, value.entry.independent,
                        "entry_independent"),
    LONEJSON_FIELD_STRING_FIXED(queue_document, value.entry.origin,
                                "entry_origin", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL(queue_document, request_text, "request_text"),
    LONEJSON_FIELD_BOOL(queue_document, entry_text, "entry_text"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(queue_document, text, "text")};
LONEJSON_MAP_DEFINE(queue_map, queue_document, fields);

#define QUEUE_RECORD_MAX (512U * 1024U)
typedef struct queue_buffer {
  cai_buffer_builder builder;
  cai_error *error;
  int rc;
} queue_buffer;
static lonejson_status write_json(void *context, const void *bytes, size_t n,
                                  lonejson_error *error) {
  queue_buffer *buffer = context;
  (void)error;
  if (n > QUEUE_RECORD_MAX - buffer->builder.length)
    buffer->rc = cai_set_error(buffer->error, CAI_ERR_LIMIT,
                               "input queue transition exceeds 512 KiB");
  else
    buffer->rc = cai_buffer_append(&buffer->builder, bytes, n, buffer->error);
  return buffer->rc == CAI_OK ? LONEJSON_STATUS_OK
                              : LONEJSON_STATUS_CALLBACK_FAILED;
}
int cai_input_record_json(const cai_input_record *record, char **out,
                          cai_error *error) {
  queue_document doc;
  queue_buffer buffer;
  lonejson_error json_error;
  lonejson_status status;
  if (record == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue record is required");
  *out = NULL;
  memset(&doc, 0, sizeof(doc));
  memset(&buffer, 0, sizeof(buffer));
  buffer.error = error;
  doc.value = *record;
  doc.type = record->type;
  doc.operation = record->request.operation;
  doc.intent = record->request.intent;
  doc.policy_intent = record->policy.intent;
  doc.outcome = record->result.outcome;
  doc.code = record->result.code;
  doc.entry_intent = record->entry.intent;
  doc.request_text = record->request.text != NULL &&
                     record->type == CAI_INPUT_RECORD_REQUEST &&
                     (record->request.operation == CAI_INPUT_OFFER ||
                      record->request.operation == CAI_INPUT_END_EDIT);
  /* Claims refer to an immutable pending revision. Do not rewrite its input
   * payload just to remove it; recovery already owns that accepted text. */
  doc.entry_text = record->entry.text != NULL &&
                   record->type == CAI_INPUT_RECORD_REQUEST &&
                   (record->request.operation == CAI_INPUT_OFFER ||
                    record->request.operation == CAI_INPUT_END_EDIT);
  /* Only the temporary serialization view borrows this const string. */
  if (doc.entry_text || doc.request_text)
    memcpy(&doc.text,
           doc.entry_text ? &record->entry.text : &record->request.text,
           sizeof(doc.text));
  lonejson_error_init(&json_error);
  status = CAI_LJ->serialize_sink(CAI_LJ, &queue_map, &doc, write_json, &buffer,
                                  &json_error);
  if (status != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.builder.data);
    return buffer.rc != CAI_OK
               ? buffer.rc
               : cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                      "serialize input queue transition",
                                      json_error.message);
  }
  *out = buffer.builder.data;
  return CAI_OK;
}
void cai_input_record_cleanup(cai_input_record *record) {
  queue_document doc;
  if (record == NULL)
    return;
  /* Parsed text is shared by both borrowed views. Free it exactly once. */
  memset(&doc, 0, sizeof(doc));
  memcpy(&doc.text,
         record->entry.text != NULL ? &record->entry.text
                                    : &record->request.text,
         sizeof(doc.text));
  CAI_LJ->cleanup(CAI_LJ, &queue_map, &doc, NULL);
  memset(record, 0, sizeof(*record));
}
int cai_input_record_parse(const char *json, cai_input_record *out,
                           cai_error *error) {
  queue_document doc;
  lonejson_error local;
  size_t n;
  if (json == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue JSON is required");
  for (n = 0U; n <= QUEUE_RECORD_MAX && json[n] != '\0'; ++n) {
  }
  if (n > QUEUE_RECORD_MAX)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "input queue transition exceeds 512 KiB");
  memset(&doc, 0, sizeof(doc));
  lonejson_error_init(&local);
  if (CAI_LJ->parse_cstr(CAI_LJ, &queue_map, &doc, json, &local) !=
      LONEJSON_STATUS_OK) {
    CAI_LJ->cleanup(CAI_LJ, &queue_map, &doc, NULL);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "parse input queue transition", local.message);
  }
  if (doc.type < CAI_INPUT_RECORD_CLIENT ||
      doc.type > CAI_INPUT_RECORD_RELEASE || doc.operation < CAI_INPUT_OFFER ||
      doc.operation > CAI_INPUT_FORGET_ORIGIN ||
      doc.intent < CAI_INPUT_QUEUED || doc.intent > CAI_INPUT_STEERING ||
      doc.policy_intent < CAI_INPUT_QUEUED ||
      doc.policy_intent > CAI_INPUT_STEERING ||
      doc.entry_intent < CAI_INPUT_QUEUED ||
      doc.entry_intent > CAI_INPUT_STEERING ||
      doc.outcome < CAI_INPUT_ACCEPTED || doc.outcome > CAI_INPUT_UNKNOWN ||
      doc.code < INT_MIN || doc.code > INT_MAX) {
    CAI_LJ->cleanup(CAI_LJ, &queue_map, &doc, NULL);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid input queue transition fields");
  }
  doc.value.type = (cai_input_record_type)doc.type;
  doc.value.request.operation = (cai_input_operation)doc.operation;
  doc.value.request.intent = (cai_input_intent)doc.intent;
  doc.value.policy.intent = (cai_input_intent)doc.policy_intent;
  doc.value.entry.intent = (cai_input_intent)doc.entry_intent;
  doc.value.result.outcome = (cai_input_outcome)doc.outcome;
  doc.value.result.code = (int)doc.code;
  if (doc.entry_text)
    doc.value.entry.text = doc.text;
  if (doc.request_text)
    doc.value.request.text = doc.text;
  if (((doc.entry_text || doc.request_text) && doc.text == NULL) ||
      (!doc.entry_text && !doc.request_text && doc.text != NULL)) {
    CAI_LJ->cleanup(CAI_LJ, &queue_map, &doc, NULL);
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "unexpected input queue text");
  }
  *out = doc.value;
  return CAI_OK;
}

int cai_input_delivery_parse(const cai_agent_session_event *event,
                             cai_input_delivery *out, cai_error *error) {
  const char *at, *end;
  unsigned long long kind, length;
  size_t n;
  if (event == NULL || out == NULL || event->type == NULL ||
      strcmp(event->type, "input_queue_delivery") != 0 || event->data == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input delivery event is required");
  memset(out, 0, sizeof(*out));
  at = event->data;
  if ((at[0] != 'L' && at[0] != 'C') || at[1] != '\n')
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid input delivery scope");
  out->local = at[0] == 'L';
  at += 2U;
  if (cai_parse_unsigned_decimal(at, &end, &length) != 0 || *end != '\n' ||
      length == 0U || length >= sizeof(out->source))
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid input delivery source");
  at = end + 1U;
  n = (size_t)length;
  if (strnlen(at, n + 1U) != n + 1U || at[n] != '\n')
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "truncated input delivery source");
  memcpy(out->source, at, n);
  at += n + 1U;
  if (cai_parse_unsigned_decimal(at, &end, &out->sequence) != 0 ||
      out->sequence == 0U || *end != '\n')
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid input delivery identity");
  if (cai_parse_unsigned_decimal(end + 1U, &at, &kind) != 0 || *at != '\n' ||
      kind > 2U || at[1] == '\0')
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "invalid input delivery kind");
  out->event = *event;
  out->event.type = kind == 1U   ? "steering_queued"
                    : kind == 2U ? "turn_queued"
                                 : "turn_submitted";
  out->event.data = at + 1U;
  return CAI_OK;
}
int cai_agent_session_event_project_input(const cai_agent_session_event *source,
                                          cai_agent_session_event *out,
                                          cai_error *error) {
  cai_input_delivery delivery;
  int rc;
  if (source == NULL || source->type == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session event and output are required");
  if (strcmp(source->type, "input_queue_delivery") != 0) {
    *out = *source;
    return CAI_OK;
  }
  rc = cai_input_delivery_parse(source, &delivery, error);
  if (rc == CAI_OK)
    *out = delivery.event;
  return rc;
}
