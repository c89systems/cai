/** @file Pull serialization shared by durable event storage and observation. */
#include "cai_internal.h"
#include <cai/network.h>
#include <stdlib.h>
#include <string.h>

typedef struct event_document {
  char record_type[6];
  lonejson_uint64 sequence, timestamp_ns;
  lonejson_int64 utc_offset_seconds;
  const char *type, *data;
} event_document;
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(event_document, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64_REQ(event_document, sequence, "sequence"),
    LONEJSON_FIELD_U64(event_document, timestamp_ns, "timestamp_ns"),
    LONEJSON_FIELD_I64(event_document, utc_offset_seconds,
                       "utc_offset_seconds"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(event_document, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC(event_document, data, "data")};
LONEJSON_MAP_DEFINE(event_map, event_document, fields);
typedef struct event_source {
  event_document document;
  lonejson *json;
  lonejson_generator generator;
} event_source;
static size_t read_event(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  event_source *source = context;
  size_t count = 0U;
  int eof = 0;
  if (lonejson_generator_read(&source->generator, bytes, capacity, &count,
                              &eof) != LONEJSON_STATUS_OK) {
    cai_set_error(error, CAI_ERR_PROTOCOL, source->generator.error.message);
    return 0U;
  }
  return count;
}
static void close_event(void *context) {
  event_source *source = context;
  lonejson_generator_cleanup(&source->generator);
  lonejson_free(source->json);
  free(source);
}
int cai_session_event_source(const cai_agent_session_event *event,
                             cai_source **out, cai_error *error) {
  event_source *source;
  cai_source_callbacks callbacks;
  lonejson_error local;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (event == NULL || event->type == NULL || out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "session event and output required");
  source = calloc(1U, sizeof(*source));
  if (source == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate event generator");
  strcpy(source->document.record_type, "event");
  source->document.sequence = event->sequence;
  source->document.timestamp_ns = event->timestamp_ns;
  source->document.utc_offset_seconds = event->utc_offset_seconds;
  source->document.type = event->type;
  source->document.data = event->data;
  lonejson_error_init(&local);
  source->json = lonejson_new(NULL, &local);
  if (source->json == NULL) {
    free(source);
    return cai_set_error(error, CAI_ERR_NOMEM, local.message);
  }
  if (lonejson_generator_init(source->json, &source->generator, &event_map,
                              &source->document) != LONEJSON_STATUS_OK) {
    rc =
        cai_set_error(error, CAI_ERR_PROTOCOL, source->generator.error.message);
    close_event(source);
    return rc;
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = read_event;
  callbacks.close = close_event;
  callbacks.context = source;
  rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    close_event(source);
  return rc;
}
