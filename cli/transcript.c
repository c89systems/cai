#define LONEJSON_WITH_CURL
#include "transcript.h"
#include "pouch_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct transcript_record {
  char record_type[24], type[129];
  lonejson_uint64 sequence, timestamp_ns, commit_created_at_ns,
      applied_event_sequence;
  lonejson_int64 utc_offset_seconds;
  lonejson_json_value data, state;
} transcript_record;
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(transcript_record, record_type,
                                    "record_type", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(transcript_record, sequence, "sequence"),
    LONEJSON_FIELD_U64(transcript_record, timestamp_ns, "timestamp_ns"),
    LONEJSON_FIELD_I64(transcript_record, utc_offset_seconds,
                       "utc_offset_seconds"),
    LONEJSON_FIELD_U64(transcript_record, commit_created_at_ns,
                       "commit_created_at_ns"),
    LONEJSON_FIELD_U64(transcript_record, applied_event_sequence,
                       "applied_event_sequence"),
    LONEJSON_FIELD_STRING_FIXED(transcript_record, type, "type",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_JSON_VALUE(transcript_record, data, "data"),
    LONEJSON_FIELD_JSON_VALUE(transcript_record, state, "state")};
LONEJSON_MAP_DEFINE(record_map, transcript_record, fields);
struct cai_cli_transcript {
  lonejson *json;
  lonejson_curl_parse parser;
  lonejson_value_visitor visitor;
  transcript_record record;
  cai_cli_transcript_event event;
  cai_cli_transcript_fn callback;
  void *context;
  cai_error *error;
  unsigned long long position, sequence;
  char type[129], failure[256];
  int active, data_started, data_done, notifying, rc;
};
static int fail(cai_cli_transcript *t, int rc, const char *message,
                cai_error *error) {
  if (t->rc == CAI_OK) {
    t->rc = rc;
    snprintf(t->failure, sizeof(t->failure), "%s", message);
  }
  return cai_cli_error(error, t->rc, t->failure);
}
static int check(cai_cli_transcript *t, cai_error *error) {
  if (t == NULL || t->notifying)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "non-reentrant transcript reader required");
  return t->rc == CAI_OK ? CAI_OK : cai_cli_error(error, t->rc, t->failure);
}
static int emit(cai_cli_transcript *t, cai_cli_transcript_kind kind,
                const void *data, size_t length) {
  int rc;
  t->event.kind = kind;
  t->event.data = data;
  t->event.length = length;
  t->notifying = 1;
  rc = t->callback(t->context, &t->event, t->error);
  t->notifying = 0;
  if (rc != CAI_OK)
    return fail(t, rc,
                t->error != NULL && t->error->message != NULL
                    ? t->error->message
                    : "transcript consumer failed",
                NULL);
  return CAI_OK;
}
static lonejson_status bad_value(void *context, lonejson_error *error) {
  cai_cli_transcript *t = context;
  (void)error;
  (void)fail(t, CAI_ERR_PROTOCOL, "journal event data must be string or null",
             NULL);
  return LONEJSON_STATUS_INVALID_JSON;
}
static lonejson_status bad_boolean(void *context, int value,
                                   lonejson_error *error) {
  (void)value;
  return bad_value(context, error);
}
static lonejson_status begin_data(cai_cli_transcript *t, int has_data) {
  if (t->data_started || strcmp(t->record.record_type, "event") != 0 ||
      t->record.sequence == 0U || t->record.sequence <= t->sequence ||
      t->record.type[0] == '\0') {
    (void)fail(t, CAI_ERR_PROTOCOL,
               "journal event headers must precede its data", NULL);
    return LONEJSON_STATUS_INVALID_JSON;
  }
  t->data_started = 1;
  strcpy(t->type, t->record.type);
  memset(&t->event, 0, sizeof(t->event));
  t->event.type = t->type;
  t->event.sequence = t->record.sequence;
  t->event.timestamp_ns = t->record.timestamp_ns;
  t->event.utc_offset_seconds = t->record.utc_offset_seconds;
  t->event.has_data = has_data;
  return emit(t, CAI_CLI_TRANSCRIPT_BEGIN, NULL, 0U) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_IO_ERROR;
}
static lonejson_status string_begin(void *context, lonejson_error *error) {
  (void)error;
  return begin_data(context, 1);
}
static lonejson_status string_chunk(void *context, const char *data,
                                    size_t length, lonejson_error *error) {
  cai_cli_transcript *t = context;
  (void)error;
  return emit(t, CAI_CLI_TRANSCRIPT_DATA, data, length) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_IO_ERROR;
}
static lonejson_status string_end(void *context, lonejson_error *error) {
  cai_cli_transcript *t = context;
  (void)error;
  t->data_done = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status null_value(void *context, lonejson_error *error) {
  cai_cli_transcript *t = context;
  lonejson_status rc;
  (void)error;
  rc = begin_data(t, 0);
  t->data_done = rc == LONEJSON_STATUS_OK;
  return rc;
}
static lonejson_status discard(void *context, const void *bytes, size_t count,
                               lonejson_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return LONEJSON_STATUS_OK;
}
static void record_clear(cai_cli_transcript *t) {
  lonejson_curl_parse_cleanup(&t->parser);
  lonejson_cleanup(&record_map, &t->record, NULL);
  memset(&t->parser, 0, sizeof(t->parser));
  memset(&t->record, 0, sizeof(t->record));
  t->active = t->data_started = t->data_done = 0;
}
static int record_start(cai_cli_transcript *t, cai_error *error) {
  lonejson_error detail;
  lonejson_status rc;
  t->active = 1;
  lonejson_json_value_init(t->json, &t->record.data);
  lonejson_json_value_init(t->json, &t->record.state);
  rc = lonejson_json_value_set_parse_visitor(&t->record.data, &t->visitor, t,
                                             &detail);
  if (rc == LONEJSON_STATUS_OK)
    rc = lonejson_json_value_set_parse_sink(&t->record.state, discard, NULL,
                                            &detail);
  if (rc == LONEJSON_STATUS_OK) {
    rc = lonejson_curl_parse_init(&t->parser, t->json, &record_map, &t->record);
    detail = t->parser.error;
  }
  return rc == LONEJSON_STATUS_OK
             ? CAI_OK
             : fail(t, CAI_ERR_PROTOCOL, detail.message, error);
}
int cai_cli_transcript_open(cai_cli_transcript_fn callback, void *context,
                            cai_cli_transcript **out, cai_error *error) {
  cai_cli_transcript *t;
  lonejson_error detail;
  lonejson_config config = lonejson_default_config();
  if (out != NULL)
    *out = NULL;
  if (callback == NULL || out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "transcript consumer and output required");
  t = calloc(1U, sizeof(*t));
  if (t == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate transcript reader");
  config.clear_destination_by_default = 0;
  config.reject_duplicate_keys_by_default = 1;
  config.json_value_max_total_bytes = config.json_value_max_string_bytes = 0U;
  t->json = lonejson_new(&config, &detail);
  if (t->json == NULL) {
    free(t);
    return cai_cli_lj_error(error, &detail);
  }
  t->callback = callback;
  t->context = context;
  t->visitor = lonejson_default_value_visitor();
  t->visitor.object_begin = t->visitor.array_begin = t->visitor.number_begin =
      bad_value;
  t->visitor.boolean_value = bad_boolean;
  t->visitor.string_begin = string_begin;
  t->visitor.string_chunk = string_chunk;
  t->visitor.string_end = string_end;
  t->visitor.null_value = null_value;
  *out = t;
  return CAI_OK;
}
int cai_cli_transcript_feed(cai_cli_transcript *t, const void *bytes,
                            size_t count, cai_error *error) {
  const unsigned char *at = bytes;
  int rc = check(t, error);
  if (rc != CAI_OK)
    return rc;
  if (count != 0U && bytes == NULL)
    return fail(t, CAI_ERR_INVALID, "transcript bytes required", error);
  if (count == 0U)
    return CAI_OK;
  t->error = error;
  if (!t->active)
    rc = record_start(t, error);
  while (count != 0U && rc == CAI_OK) {
    char chunk[4096];
    size_t n = count < sizeof(chunk) ? count : sizeof(chunk);
    memcpy(chunk, at, n);
    if (lonejson_curl_write_callback(chunk, 1U, n, &t->parser) != n)
      rc = t->rc != CAI_OK
               ? t->rc
               : fail(t, CAI_ERR_PROTOCOL, t->parser.error.message, NULL);
    count -= n;
    at += n;
  }
  t->error = NULL;
  return rc == CAI_OK ? CAI_OK : cai_cli_error(error, t->rc, t->failure);
}
int cai_cli_transcript_boundary(cai_cli_transcript *t,
                                unsigned long long position,
                                const char *append_id, cai_error *error) {
  char expected[64];
  int rc = check(t, error);
  if (rc != CAI_OK)
    return rc;
  if (!t->active || position <= t->position || append_id == NULL)
    return fail(t, CAI_ERR_PROTOCOL, "invalid journal entry boundary", error);
  t->error = error;
  if (lonejson_curl_parse_finish(&t->parser) != LONEJSON_STATUS_OK) {
    t->error = NULL;
    return fail(t, CAI_ERR_PROTOCOL, t->parser.error.message, error);
  }
  if (strcmp(t->record.record_type, "commit") == 0) {
    snprintf(expected, sizeof(expected), "commit-%020llu-%020llu",
             (unsigned long long)t->record.commit_created_at_ns,
             (unsigned long long)t->record.applied_event_sequence);
    if (t->data_started || t->record.commit_created_at_ns == 0U ||
        strcmp(expected, append_id) != 0)
      rc = fail(t, CAI_ERR_PROTOCOL, "invalid stored commit identity", NULL);
  } else if (strcmp(t->record.record_type, "event") == 0) {
    if (!t->data_started && begin_data(t, 0) == LONEJSON_STATUS_OK)
      t->data_done = 1;
    snprintf(expected, sizeof(expected), "event-%020llu",
             (unsigned long long)t->record.sequence);
    if (t->rc != CAI_OK)
      rc = t->rc;
    else if (!t->data_done || strcmp(expected, append_id) != 0 ||
             t->event.sequence != t->record.sequence ||
             t->event.timestamp_ns != t->record.timestamp_ns ||
             t->event.utc_offset_seconds != t->record.utc_offset_seconds ||
             strcmp(t->type, t->record.type) != 0)
      rc = fail(t, CAI_ERR_PROTOCOL, "invalid stored event identity", NULL);
    else {
      t->event.position = position;
      rc = emit(t, CAI_CLI_TRANSCRIPT_END, NULL, 0U);
      if (rc == CAI_OK)
        t->sequence = t->record.sequence;
    }
  } else
    rc = fail(t, CAI_ERR_PROTOCOL, "unknown journal record type", NULL);
  t->error = NULL;
  if (rc != CAI_OK)
    return cai_cli_error(error, t->rc, t->failure);
  t->position = position;
  record_clear(t);
  return CAI_OK;
}
int cai_cli_transcript_finish(cai_cli_transcript *t, cai_error *error) {
  int rc = check(t, error);
  if (rc != CAI_OK)
    return rc;
  return t->active
             ? fail(t, CAI_ERR_PROTOCOL, "journal entry has no boundary", error)
             : CAI_OK;
}
void cai_cli_transcript_close(cai_cli_transcript *t) {
  if (t == NULL || t->notifying)
    return;
  record_clear(t);
  lonejson_free(t->json);
  free(t);
}
