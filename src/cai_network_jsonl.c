/** @file Observe completed JSONL records without retaining record bodies. */
#include "cai_network_jsonl.h"
#include "cai_internal.h"
#include <stdlib.h>
#include <string.h>
typedef struct observed_event {
  char record_type[8], type[128];
  lonejson_uint64 sequence;
  lonejson_json_value data;
} observed_event;
static const lonejson_field fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(observed_event, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(observed_event, type, "type",
                                    LONEJSON_OVERFLOW_TRUNCATE_SILENT),
    LONEJSON_FIELD_U64_REQ(observed_event, sequence, "sequence"),
    LONEJSON_FIELD_JSON_VALUE(observed_event, data, "data")};
LONEJSON_MAP_DEFINE(observed_map, observed_event, fields);
struct cai_network_jsonl {
  lonejson *json;
  lonejson_curl_parse parser;
  observed_event event;
  cai_network_jsonl_stats stats;
  int started, failed;
};
static lonejson_status discard(void *context, const void *bytes, size_t count,
                               lonejson_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return LONEJSON_STATUS_OK;
}
int cai_network_jsonl_open(cai_network_jsonl **out, cai_error *error) {
  cai_network_jsonl *reader = calloc(1U, sizeof(*reader));
  lonejson_config config;
  lonejson_error local;
  *out = NULL;
  if (reader == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate event JSONL validator");
  config = lonejson_default_config();
  config.max_alloc_bytes = 65536U;
  config.max_dynamic_string_bytes = 1024U;
  config.spool_default.memory_limit = config.spool_default.max_bytes = 1024U;
  config.spool_blob = config.spool_large_text = config.spool_default;
  lonejson_error_init(&local);
  reader->json = lonejson_new(&config, &local);
  if (reader->json == NULL) {
    free(reader);
    return cai_set_error(error, CAI_ERR_NOMEM, local.message);
  }
  *out = reader;
  return CAI_OK;
}
void cai_network_jsonl_feed(cai_network_jsonl *reader, void *bytes,
                            size_t count) {
  unsigned char *p = bytes;
  while (count) {
    unsigned char *newline = memchr(p, '\n', count);
    size_t length = newline != NULL ? (size_t)(newline - p) : count;
    if (!reader->started) {
      lonejson_error local;
      lonejson_error_init(&local);
      lonejson_json_value_init(reader->json, &reader->event.data);
      reader->failed =
          lonejson_json_value_set_parse_sink(&reader->event.data, discard, NULL,
                                             &local) != LONEJSON_STATUS_OK;
      if (!reader->failed)
        reader->failed = lonejson_curl_parse_init(
                             &reader->parser, reader->json, &observed_map,
                             &reader->event) != LONEJSON_STATUS_OK;
      reader->started = 1;
    }
    if (!reader->failed && length &&
        lonejson_curl_write_callback((char *)p, 1U, length, &reader->parser) !=
            length)
      reader->failed = 1;
    reader->stats.partial_line = 1;
    if (newline != NULL) {
      if (!reader->failed &&
          lonejson_curl_parse_finish(&reader->parser) == LONEJSON_STATUS_OK &&
          !strcmp(reader->event.record_type, "event") &&
          reader->event.sequence && reader->event.type[0])
        reader->stats.events++;
      else
        reader->stats.invalid_lines++;
      lonejson_curl_parse_cleanup(&reader->parser);
      lonejson_cleanup(&observed_map, &reader->event);
      memset(&reader->event, 0, sizeof(reader->event));
      reader->started = reader->failed = reader->stats.partial_line = 0;
      length++;
    }
    p += length;
    count -= length;
  }
}
void cai_network_jsonl_get(cai_network_jsonl *reader,
                           cai_network_jsonl_stats *out) {
  *out = reader->stats;
}
void cai_network_jsonl_close(cai_network_jsonl *reader) {
  if (reader != NULL) {
    if (reader->started)
      lonejson_curl_parse_cleanup(&reader->parser);
    lonejson_cleanup(&observed_map, &reader->event);
    lonejson_free(reader->json);
    free(reader);
  }
}
