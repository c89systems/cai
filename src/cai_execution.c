#include "cai_execution.h"
#include "cai_internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const lonejson_field execution_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, model, "model"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, base_url,
                                          "base_url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record,
                                          last_response_id, "last_response_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_execution_record, kind, "kind"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_execution_record, session_id,
                                    "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(
        cai_execution_record, parent_session_id, "parent_session_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, parent_call_id,
                                          "parent_call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, subagent,
                                          "subagent"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, tool_name,
                                          "tool_name"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, call_id,
                                          "call_id"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_execution_record, field, "field"),
    LONEJSON_FIELD_U64_REQ(cai_execution_record, timestamp_ns, "timestamp_ns"),
    LONEJSON_FIELD_I64(cai_execution_record, utc_offset_seconds,
                       "utc_offset_seconds"),
    LONEJSON_FIELD_U64(cai_execution_record, source_sequence,
                       "source_sequence"),
    LONEJSON_FIELD_U64_REQ(cai_execution_record, offset, "offset"),
    LONEJSON_FIELD_BOOL_REQ(cai_execution_record, final, "final"),
    LONEJSON_FIELD_BOOL_REQ(cai_execution_record, line_start, "line_start"),
    LONEJSON_FIELD_I64(cai_execution_record, code, "code"),
    LONEJSON_FIELD_I64(cai_execution_record, http_status, "http_status"),
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_execution_record, data, "data",
                                    LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(execution_map, cai_execution_record, execution_fields);
typedef char
    execution_schema_mask_fits[sizeof(execution_fields) /
                                           sizeof(execution_fields[0]) <=
                                       sizeof(unsigned long) * CHAR_BIT
                                   ? 1
                                   : -1];

void cai_execution_record_init(cai_execution_record *record) {
  if (record != NULL)
    memset(record, 0, sizeof(*record));
}
void cai_execution_record_cleanup(cai_execution_record *record) {
  if (record != NULL) {
    size_t i;
    for (i = 0U; i < execution_map.field_count; ++i)
      if (execution_fields[i].kind == LONEJSON_FIELD_KIND_STRING &&
          execution_fields[i].storage == LONEJSON_STORAGE_DYNAMIC) {
        void *owned;
        memcpy(&owned,
               (const unsigned char *)record +
                   execution_fields[i].struct_offset,
               sizeof(owned));
        cai_free_mem(NULL, owned);
      }
    memset(record, 0, sizeof(*record));
  }
}
typedef struct execution_utf8 {
  unsigned long codepoint, minimum;
  unsigned int remaining;
} execution_utf8;
static lonejson_status execution_text_begin(void *context,
                                            lonejson_error *error) {
  (void)error;
  memset(context, 0, sizeof(execution_utf8));
  return LONEJSON_STATUS_OK;
}
static lonejson_status execution_text_end(void *context,
                                          lonejson_error *error) {
  execution_utf8 *text = context;
  (void)error;
  return text->remaining == 0U ? LONEJSON_STATUS_OK
                               : LONEJSON_STATUS_INVALID_JSON;
}
static lonejson_status execution_text_chunk(void *context, const char *bytes,
                                            size_t count,
                                            lonejson_error *error) {
  execution_utf8 *text = context;
  size_t i;
  (void)error;
  for (i = 0U; i < count; i++) {
    unsigned char c = (unsigned char)bytes[i];
    if (text->remaining == 0U) {
      if (c == 0U)
        return LONEJSON_STATUS_INVALID_JSON;
      if (c < 0x80U)
        continue;
      if (c >= 0xc2U && c <= 0xdfU) {
        text->remaining = 1U;
        text->codepoint = c & 0x1fU;
        text->minimum = 0x80UL;
      } else if (c >= 0xe0U && c <= 0xefU) {
        text->remaining = 2U;
        text->codepoint = c & 0x0fU;
        text->minimum = 0x800UL;
      } else if (c >= 0xf0U && c <= 0xf4U) {
        text->remaining = 3U;
        text->codepoint = c & 0x07U;
        text->minimum = 0x10000UL;
      } else
        return LONEJSON_STATUS_INVALID_JSON;
    } else {
      if ((c & 0xc0U) != 0x80U)
        return LONEJSON_STATUS_INVALID_JSON;
      text->codepoint = (text->codepoint << 6) | (c & 0x3fU);
      if (--text->remaining == 0U &&
          (text->codepoint < text->minimum || text->codepoint > 0x10ffffUL ||
           (text->codepoint >= 0xd800UL && text->codepoint <= 0xdfffUL)))
        return LONEJSON_STATUS_INVALID_JSON;
    }
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status execution_validate_json(const char *json, size_t length,
                                               lonejson_error *error) {
  lonejson_value_visitor visitor = lonejson_default_value_visitor();
  execution_utf8 text;
  lonejson_error_init(error);
  memset(&text, 0, sizeof(text));
  visitor.string_begin = execution_text_begin;
  visitor.string_chunk = execution_text_chunk;
  visitor.string_end = execution_text_end;
  visitor.object_key_begin = execution_text_begin;
  visitor.object_key_chunk = execution_text_chunk;
  visitor.object_key_end = execution_text_end;
  return CAI_LJ->visit_value_buffer(CAI_LJ, json, length, &visitor, &text,
                                    error);
}
typedef struct execution_validation {
  lonejson_json_value record;
} execution_validation;
static const lonejson_field validation_fields[] = {
    LONEJSON_FIELD_JSON_VALUE_REQ(execution_validation, record, "record")};
LONEJSON_MAP_DEFINE(validation_map, execution_validation, validation_fields);
struct cai_execution_decoder {
  lonejson_curl_parse validation;
  lonejson_value_visitor visitor;
  execution_validation value;
  execution_utf8 utf8;
  cai_execution_record record;
  size_t received;
  size_t depth, key_length, string_length, string_capacity, number_length;
  unsigned long seen;
  int field, string_field, number_field, root_done;
  char key[64], number[32];
  int started, finished, failed;
  char message[192], detail[512];
};
static int decoder_failure(cai_execution_decoder *d, int code,
                           const char *message, const char *detail,
                           cai_error *error) {
  if (d->failed == CAI_OK) {
    d->failed = code;
    snprintf(d->message, sizeof(d->message), "%s", message);
    snprintf(d->detail, sizeof(d->detail), "%s", detail != NULL ? detail : "");
  }
  return cai_set_error_detail(error, d->failed, d->message, d->detail);
}
static int decoder_check(cai_execution_decoder *d, cai_error *error) {
  if (d == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "execution decoder required");
  if (d->failed != CAI_OK)
    return cai_set_error_detail(error, d->failed, d->message, d->detail);
  if (d->finished)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "reset the completed execution decoder first");
  return CAI_OK;
}
static int decoder_parse_failure(cai_execution_decoder *d, cai_error *error) {
  int allocation =
      d->validation.error.code == LONEJSON_STATUS_ALLOCATION_FAILED;
  return decoder_failure(d, allocation ? CAI_ERR_NOMEM : CAI_ERR_PROTOCOL,
                         allocation ? "allocate execution parser state"
                                    : "invalid execution JSON text",
                         d->validation.error.message, error);
}
static lonejson_status decoder_invalid(cai_execution_decoder *d,
                                       const char *message) {
  (void)decoder_failure(d, CAI_ERR_PROTOCOL, "invalid execution record",
                        message, NULL);
  return LONEJSON_STATUS_INVALID_JSON;
}
static void *decoder_field(cai_execution_decoder *d, int index) {
  return (unsigned char *)&d->record + execution_fields[index].struct_offset;
}
static void decoder_seen(cai_execution_decoder *d) {
  if (d->field >= 0)
    d->seen |= 1UL << (unsigned int)d->field;
  d->field = -1;
}
static lonejson_status decoder_object_begin(void *context,
                                            lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if ((d->depth == 0U && d->root_done) || (d->depth == 1U && d->field >= 0))
    return decoder_invalid(d, "object does not match execution field");
  ++d->depth;
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_array_begin(void *context,
                                           lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if (d->depth == 0U || (d->depth == 1U && d->field >= 0))
    return decoder_invalid(d, "array does not match execution field");
  ++d->depth;
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_container_end(void *context,
                                             lonejson_error *error) {
  cai_execution_decoder *d = context;
  size_t i;
  (void)error;
  if (d->depth == 0U)
    return decoder_invalid(d, "unbalanced execution object");
  if (--d->depth == 0U) {
    for (i = 0U; i < execution_map.field_count; ++i)
      if ((execution_fields[i].flags & LONEJSON_FIELD_REQUIRED) != 0U &&
          (d->seen & (1UL << i)) == 0U)
        return decoder_invalid(d, "missing required execution field");
    d->root_done = 1;
  }
  d->field = -1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_key_begin(void *context, lonejson_error *error) {
  cai_execution_decoder *d = context;
  d->field = -1;
  d->key_length = 0U;
  return execution_text_begin(&d->utf8, error);
}
static lonejson_status decoder_key_chunk(void *context, const char *bytes,
                                         size_t length, lonejson_error *error) {
  cai_execution_decoder *d = context;
  if (execution_text_chunk(&d->utf8, bytes, length, error) !=
      LONEJSON_STATUS_OK)
    return decoder_invalid(d,
                           "invalid UTF-8 or embedded NUL in execution text");
  if (d->depth == 1U) {
    size_t available = sizeof(d->key) - 1U - d->key_length;
    size_t n = length < available ? length : available;
    memcpy(d->key + d->key_length, bytes, n);
    d->key_length += n;
    /* A saturated prefix cannot match any schema key (all are shorter). */
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_key_end(void *context, lonejson_error *error) {
  cai_execution_decoder *d = context;
  size_t i;
  if (execution_text_end(&d->utf8, error) != LONEJSON_STATUS_OK)
    return decoder_invalid(d,
                           "invalid UTF-8 or embedded NUL in execution text");
  d->key[d->key_length] = '\0';
  if (d->depth == 1U)
    for (i = 0U; i < execution_map.field_count; ++i)
      if (strcmp(d->key, execution_fields[i].json_key) == 0) {
        if ((d->seen & (1UL << i)) != 0U)
          return decoder_invalid(d, "duplicate execution field");
        d->field = (int)i;
        break;
      }
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_string_begin(void *context,
                                            lonejson_error *error) {
  cai_execution_decoder *d = context;
  if (d->depth == 0U || (d->field >= 0 && execution_fields[d->field].kind !=
                                              LONEJSON_FIELD_KIND_STRING))
    return decoder_invalid(d, "string does not match execution field");
  d->string_field = d->depth == 1U ? d->field : -1;
  d->string_length = d->string_capacity = 0U;
  return execution_text_begin(&d->utf8, error);
}
static lonejson_status decoder_string_chunk(void *context, const char *bytes,
                                            size_t length,
                                            lonejson_error *error) {
  cai_execution_decoder *d = context;
  char *text;
  size_t needed, capacity;
  const lonejson_field *field;
  if (execution_text_chunk(&d->utf8, bytes, length, error) !=
      LONEJSON_STATUS_OK)
    return decoder_invalid(d,
                           "invalid UTF-8 or embedded NUL in execution text");
  if (d->string_field < 0)
    return LONEJSON_STATUS_OK;
  field = &execution_fields[d->string_field];
  needed = d->string_length + length + 1U;
  if (field->storage == LONEJSON_STORAGE_FIXED) {
    if (needed > field->fixed_capacity)
      return decoder_invalid(d, "execution data exceeds its decoded chunk cap");
    text = decoder_field(d, d->string_field);
  } else {
    memcpy(&text, decoder_field(d, d->string_field), sizeof(text));
    if (needed > d->string_capacity) {
      char *grown;
      capacity = d->string_capacity != 0U ? d->string_capacity : 32U;
      while (capacity < needed)
        capacity *= 2U;
      grown = cai_realloc_mem(NULL, text, capacity);
      if (grown == NULL) {
        (void)decoder_failure(d, CAI_ERR_NOMEM, "allocate execution metadata",
                              NULL, NULL);
        return LONEJSON_STATUS_ALLOCATION_FAILED;
      }
      text = grown;
      d->string_capacity = capacity;
      memcpy(decoder_field(d, d->string_field), &text, sizeof(text));
    }
  }
  if (length != 0U)
    memcpy(text + d->string_length, bytes, length);
  d->string_length += length;
  text[d->string_length] = '\0';
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_string_end(void *context,
                                          lonejson_error *error) {
  cai_execution_decoder *d = context;
  if (execution_text_end(&d->utf8, error) != LONEJSON_STATUS_OK)
    return decoder_invalid(d,
                           "invalid UTF-8 or embedded NUL in execution text");
  if (d->string_field >= 0 && d->string_length == 0U &&
      decoder_string_chunk(d, "", 0U, error) != LONEJSON_STATUS_OK)
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  decoder_seen(d);
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_boolean(void *context, int value,
                                       lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if (d->depth == 0U || (d->field >= 0 && execution_fields[d->field].kind !=
                                              LONEJSON_FIELD_KIND_BOOL))
    return decoder_invalid(d, "boolean does not match execution field");
  if (d->field >= 0)
    memcpy(decoder_field(d, d->field), &value, sizeof(value));
  decoder_seen(d);
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_null(void *context, lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if (d->depth == 0U ||
      (d->field >= 0 &&
       (execution_fields[d->field].kind != LONEJSON_FIELD_KIND_STRING ||
        execution_fields[d->field].storage != LONEJSON_STORAGE_DYNAMIC ||
        (execution_fields[d->field].flags & LONEJSON_FIELD_REQUIRED) != 0U)))
    return decoder_invalid(d, "null does not match execution field");
  decoder_seen(d);
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_number_begin(void *context,
                                            lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if (d->depth == 0U ||
      (d->field >= 0 &&
       execution_fields[d->field].kind != LONEJSON_FIELD_KIND_U64 &&
       execution_fields[d->field].kind != LONEJSON_FIELD_KIND_I64))
    return decoder_invalid(d, "number does not match execution field");
  d->number_field = d->field;
  d->number_length = 0U;
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_number_chunk(void *context, const char *bytes,
                                            size_t length,
                                            lonejson_error *error) {
  cai_execution_decoder *d = context;
  (void)error;
  if (d->number_field < 0)
    return LONEJSON_STATUS_OK;
  if (length > sizeof(d->number) - d->number_length)
    return decoder_invalid(d, "execution integer is too long");
  memcpy(d->number + d->number_length, bytes, length);
  d->number_length += length;
  return LONEJSON_STATUS_OK;
}
static lonejson_status decoder_number_end(void *context,
                                          lonejson_error *error) {
  cai_execution_decoder *d = context;
  unsigned long long value = 0U, limit;
  size_t i;
  int negative;
  (void)error;
  if (d->number_field < 0)
    return LONEJSON_STATUS_OK;
  negative = d->number_length != 0U && d->number[0] == '-';
  limit = execution_fields[d->number_field].kind == LONEJSON_FIELD_KIND_U64
              ? ULLONG_MAX
              : (unsigned long long)LLONG_MAX + (unsigned int)negative;
  if (negative &&
      execution_fields[d->number_field].kind == LONEJSON_FIELD_KIND_U64)
    return decoder_invalid(d, "negative execution counter");
  for (i = negative ? 1U : 0U; i < d->number_length; ++i) {
    unsigned int digit = (unsigned int)(unsigned char)d->number[i] - '0';
    if (digit > 9U || value > (limit - digit) / 10U)
      return decoder_invalid(d, "invalid or overflowing execution integer");
    value = value * 10U + digit;
  }
  if (execution_fields[d->number_field].kind == LONEJSON_FIELD_KIND_U64)
    memcpy(decoder_field(d, d->number_field), &value, sizeof(value));
  else {
    long long number = negative ? (value == (unsigned long long)LLONG_MAX + 1U
                                       ? LLONG_MIN
                                       : -(long long)value)
                                : (long long)value;
    memcpy(decoder_field(d, d->number_field), &number, sizeof(number));
  }
  decoder_seen(d);
  return LONEJSON_STATUS_OK;
}
static int decoder_start(cai_execution_decoder *d, cai_error *error) {
  lonejson_error detail;
  lonejson_status rc;
  char prefix[] = "{\"record\":";
  d->started = 1;
  d->field = d->string_field = d->number_field = -1;
  d->visitor = lonejson_default_value_visitor();
  d->visitor.object_begin = decoder_object_begin;
  d->visitor.array_begin = decoder_array_begin;
  d->visitor.object_end = d->visitor.array_end = decoder_container_end;
  d->visitor.object_key_begin = decoder_key_begin;
  d->visitor.object_key_chunk = decoder_key_chunk;
  d->visitor.object_key_end = decoder_key_end;
  d->visitor.string_begin = decoder_string_begin;
  d->visitor.string_chunk = decoder_string_chunk;
  d->visitor.string_end = decoder_string_end;
  d->visitor.boolean_value = decoder_boolean;
  d->visitor.null_value = decoder_null;
  d->visitor.number_begin = decoder_number_begin;
  d->visitor.number_chunk = decoder_number_chunk;
  d->visitor.number_end = decoder_number_end;
  lonejson_json_value_init(CAI_LJ_PRESERVE, &d->value.record);
  rc = lonejson_json_value_set_parse_visitor(&d->value.record, &d->visitor, d,
                                             &detail);
  if (rc == LONEJSON_STATUS_OK)
    rc = lonejson_curl_parse_init(&d->validation, CAI_LJ_PRESERVE,
                                  &validation_map, &d->value);
  if (rc != LONEJSON_STATUS_OK)
    return decoder_failure(d, CAI_ERR_NOMEM, "initialize execution decoder",
                           NULL, error);
  /* The public push parser accepts mapped roots. This constant wrapper routes
   * the original JSON into its noncapturing SAX validator; no JSON bytes are
   * retained or reserialized. The SAX visitor uses execution_fields directly
   * to fill the bounded decoded record, validating text in that same pass. */
  if (lonejson_curl_write_callback(prefix, 1U, sizeof(prefix) - 1U,
                                   &d->validation) != sizeof(prefix) - 1U)
    return decoder_parse_failure(d, error);
  return CAI_OK;
}
int cai_execution_decoder_open(cai_execution_decoder **out, cai_error *error) {
  cai_execution_decoder *d;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "execution decoder output required");
  *out = NULL;
  d = cai_alloc(NULL, sizeof(*d));
  if (d == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate execution decoder");
  memset(d, 0, sizeof(*d));
  *out = d;
  return CAI_OK;
}
int cai_execution_decoder_feed(cai_execution_decoder *d, const void *bytes,
                               size_t length, cai_error *error) {
  const unsigned char *at = bytes;
  char chunk[2048];
  int rc = decoder_check(d, error);
  if (rc != CAI_OK)
    return rc;
  if (length != 0U && bytes == NULL)
    return decoder_failure(d, CAI_ERR_INVALID, "execution bytes required", NULL,
                           error);
  if (length > CAI_EXECUTION_RECORD_MAX_BYTES - d->received)
    return decoder_failure(d, CAI_ERR_LIMIT, "execution record exceeds 64 KiB",
                           NULL, error);
  if (length == 0U)
    return CAI_OK;
  if (!d->started && (rc = decoder_start(d, error)) != CAI_OK)
    return rc;
  d->received += length;
  while (length != 0U) {
    size_t n = length < sizeof(chunk) ? length : sizeof(chunk);
    memcpy(chunk, at, n);
    if (lonejson_curl_write_callback(chunk, 1U, n, &d->validation) != n)
      return decoder_parse_failure(d, error);
    at += n;
    length -= n;
  }
  return CAI_OK;
}
int cai_execution_decoder_finish(cai_execution_decoder *d,
                                 cai_execution_record *out, cai_error *error) {
  char suffix[] = "}";
  int rc = decoder_check(d, error);
  if (rc != CAI_OK)
    return rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "initialized execution record output required");
  if (!d->started && (rc = decoder_start(d, error)) != CAI_OK)
    return rc;
  if (lonejson_curl_write_callback(suffix, 1U, 1U, &d->validation) != 1U ||
      lonejson_curl_parse_finish(&d->validation) != LONEJSON_STATUS_OK)
    return decoder_parse_failure(d, error);
  if (!d->root_done || d->record.kind == NULL || d->record.kind[0] == '\0' ||
      d->record.session_id == NULL || d->record.session_id[0] == '\0')
    return decoder_failure(d, CAI_ERR_PROTOCOL,
                           "execution record has no kind or session", NULL,
                           error);
  lonejson_curl_parse_cleanup(&d->validation);
  cai_execution_record_cleanup(out);
  *out = d->record;
  cai_execution_record_init(&d->record);
  d->finished = 1;
  return CAI_OK;
}
int cai_execution_decoder_reset(cai_execution_decoder *d, cai_error *error) {
  if (d == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "execution decoder required");
  lonejson_curl_parse_cleanup(&d->validation);
  if (d->started)
    lonejson_json_value_cleanup(&d->value.record);
  cai_execution_record_cleanup(&d->record);
  memset(d, 0, sizeof(*d));
  return CAI_OK;
}
void cai_execution_decoder_close(cai_execution_decoder *d) {
  if (d != NULL) {
    (void)cai_execution_decoder_reset(d, NULL);
    cai_free_mem(NULL, d);
  }
}
int cai_execution_record_parse(const char *json, cai_execution_record *record,
                               cai_error *error) {
  cai_execution_decoder *decoder = NULL;
  int rc;
  size_t length;
  if (json == NULL || record == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "execution record is required");
  for (length = 0U;
       length <= CAI_EXECUTION_RECORD_MAX_BYTES && json[length] != '\0';
       length++) {
  }
  if (length > CAI_EXECUTION_RECORD_MAX_BYTES)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "execution record exceeds 64 KiB");
  rc = cai_execution_decoder_open(&decoder, error);
  if (rc == CAI_OK)
    rc = cai_execution_decoder_feed(decoder, json, length, error);
  if (rc == CAI_OK)
    rc = cai_execution_decoder_finish(decoder, record, error);
  cai_execution_decoder_close(decoder);
  return rc;
}
typedef struct execution_json_buffer {
  cai_buffer_builder buffer;
  cai_error *error;
  int rc;
} execution_json_buffer;

static lonejson_status execution_write(void *context, const void *bytes,
                                       size_t count, lonejson_error *error) {
  execution_json_buffer *buffer = context;
  (void)error;
  if (count > CAI_EXECUTION_RECORD_MAX_BYTES - buffer->buffer.length) {
    buffer->rc = cai_set_error(buffer->error, CAI_ERR_LIMIT,
                               "execution record exceeds 64 KiB");
    return LONEJSON_STATUS_OVERFLOW;
  }
  buffer->rc = cai_buffer_append(&buffer->buffer, bytes, count, buffer->error);
  return buffer->rc == CAI_OK ? LONEJSON_STATUS_OK
                              : LONEJSON_STATUS_ALLOCATION_FAILED;
}
int cai_execution_record_json(const cai_execution_record *record, char **out,
                              cai_error *error) {
  execution_json_buffer buffer;
  lonejson_error local;
  memset(&buffer, 0, sizeof(buffer));
  buffer.error = error;
  *out = NULL;
  if (CAI_LJ->serialize_sink(CAI_LJ, &execution_map, record, execution_write,
                             &buffer, &local) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.buffer.data);
    if (buffer.rc != CAI_OK)
      return buffer.rc;
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "serialize execution record", local.message);
  }
  if (execution_validate_json(buffer.buffer.data, buffer.buffer.length,
                              &local) != LONEJSON_STATUS_OK) {
    cai_free_mem(NULL, buffer.buffer.data);
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "execution record is not valid UTF-8 text",
                                local.message);
  }
  *out = buffer.buffer.data;
  return CAI_OK;
}
static int execution_text(cai_sink *sink, const char *text, cai_error *error) {
  return cai_sink_write(sink, text, strlen(text), error);
}
/* Metadata may originate in provider IDs or imported records. Keep control
 * characters out of headings too, without altering the retained record. */
static int execution_label(cai_sink *sink, const char *text, cai_error *error) {
  size_t i, begin = 0U;
  int rc = CAI_OK;
  char escaped[8];
  for (i = 0U; text[i] != '\0' && rc == CAI_OK; i++) {
    unsigned int control = (unsigned char)text[i];
    if (control < 32U || control == 127U ||
        (control == 0xc2U && (unsigned char)text[i + 1U] >= 0x80U &&
         (unsigned char)text[i + 1U] <= 0x9fU)) {
      if (i > begin)
        rc = cai_sink_write(sink, text + begin, i - begin, error);
      if (control == 0xc2U)
        control = (unsigned char)text[++i];
      snprintf(escaped, sizeof(escaped), "\\u%04x", control);
      if (rc == CAI_OK)
        rc = execution_text(sink, escaped, error);
      begin = i + 1U;
    }
  }
  if (rc == CAI_OK && i > begin)
    rc = cai_sink_write(sink, text + begin, i - begin, error);
  return rc;
}
int cai_execution_record_write_markdown(const cai_execution_record *r,
                                        int compact, cai_sink *sink,
                                        cai_error *error) {
  size_t i, begin;
  int rc, line_start;
  int failed;
  const char *label;
  char status[96];
  if (r == NULL || r->kind == NULL || sink == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "execution record and sink are required");
  failed = strstr(r->kind, "failed") != NULL;
  if (compact && strcmp(r->kind, "tool.started") == 0)
    return CAI_OK;
  if (compact &&
      (strncmp(r->kind, "turn.", 5U) == 0 ||
       strncmp(r->kind, "response.", 9U) == 0) &&
      !failed && strcmp(r->kind, "turn.cancelled") != 0)
    return CAI_OK;
  if (compact && !failed && r->field != NULL &&
      strcmp(r->kind, "hosted.tool") != 0)
    return CAI_OK;
  label = strcmp(r->kind, "reasoning.receipt") == 0      ? "Reasoning"
          : strcmp(r->kind, "tool.started") == 0         ? "Tool called"
          : strcmp(r->kind, "tool.arguments") == 0       ? "Tool arguments"
          : strcmp(r->kind, "tool.output") == 0          ? "Tool result"
          : strcmp(r->kind, "tool.partial_output") == 0  ? "Partial tool result"
          : strcmp(r->kind, "tool.completed") == 0       ? "Tool completed"
          : strcmp(r->kind, "tool.failed") == 0          ? "Tool failed"
          : strcmp(r->kind, "turn.failed") == 0          ? "Turn failed"
          : strcmp(r->kind, "compaction.failed") == 0    ? "Compaction failed"
          : strcmp(r->kind, "request.failed") == 0       ? "Request failed"
          : strcmp(r->kind, "request.retry") == 0        ? "Request retry"
          : strcmp(r->kind, "turn.started") == 0         ? "Turn started"
          : strcmp(r->kind, "turn.completed") == 0       ? "Turn completed"
          : strcmp(r->kind, "turn.cancelled") == 0       ? "Turn cancelled"
          : strcmp(r->kind, "response.completed") == 0   ? "Response completed"
          : strcmp(r->kind, "compaction.started") == 0   ? "Compaction started"
          : strcmp(r->kind, "compaction.completed") == 0 ? "Context compacted"
          : strcmp(r->kind, "goal.changed") == 0         ? "Goal status changed"
          : strcmp(r->kind, "subagent.input") == 0 ? "Subagent instruction"
          : strcmp(r->kind, "hosted.tool") == 0    ? "Hosted tool"
                                                   : r->kind;
  rc = CAI_OK;
  if (r->offset == 0U) {
    rc = execution_text(sink, "\n\n**", error);
    if (rc == CAI_OK)
      rc = execution_label(sink, label, error);
    if (rc == CAI_OK && r->tool_name != NULL) {
      rc = execution_text(sink, ": ", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->tool_name, error);
    }
    if (rc == CAI_OK)
      rc = execution_text(sink, "**", error);
    if (rc == CAI_OK && r->field == NULL && !compact && r->call_id != NULL) {
      rc = execution_text(sink, " — call `", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->call_id, error);
      if (rc == CAI_OK)
        rc = execution_text(sink, "`", error);
    }
    if (rc == CAI_OK && !compact && r->subagent != NULL) {
      rc = execution_text(sink, " — subagent `", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->subagent, error);
      if (rc == CAI_OK)
        rc = execution_text(sink, "`", error);
    }
    if (rc == CAI_OK && r->field == NULL &&
        (r->code != 0LL || r->http_status != 0LL)) {
      if (r->code != 0LL && r->http_status != 0LL)
        snprintf(status, sizeof(status), " (code %lld; HTTP %lld)", r->code,
                 r->http_status);
      else if (r->code != 0LL)
        snprintf(status, sizeof(status), " (code %lld)", r->code);
      else
        snprintf(status, sizeof(status), " (HTTP %lld)", r->http_status);
      rc = execution_text(sink, status, error);
    }
    if (rc == CAI_OK && r->field != NULL) {
      rc = execution_text(sink, " — ", error);
      if (rc == CAI_OK)
        rc = execution_label(sink, r->field, error);
    }
    if (rc == CAI_OK)
      rc = execution_text(sink, "\n\n", error);
  }
  line_start = r->line_start;
  if (compact && !failed)
    return rc;
  begin = 0U;
  for (i = 0U; r->data[i] != '\0' && rc == CAI_OK; i++) {
    if (line_start) {
      rc = execution_text(sink, "    ", error);
      line_start = 0;
    }
    if (((unsigned char)r->data[i] < 32U && r->data[i] != '\n' &&
         r->data[i] != '\t') ||
        (unsigned char)r->data[i] == 127U ||
        ((unsigned char)r->data[i] == 0xc2U &&
         (unsigned char)r->data[i + 1U] >= 0x80U &&
         (unsigned char)r->data[i + 1U] <= 0x9fU)) {
      unsigned int control = (unsigned char)r->data[i];
      if (i > begin && rc == CAI_OK)
        rc = cai_sink_write(sink, r->data + begin, i - begin, error);
      if (control == 0xc2U)
        control = (unsigned char)r->data[++i];
      snprintf(status, sizeof(status), "\\u%04x", control);
      if (rc == CAI_OK)
        rc = execution_text(sink, status, error);
      begin = i + 1U;
      continue;
    }
    if (r->data[i] == '\n') {
      if (rc == CAI_OK)
        rc = cai_sink_write(sink, r->data + begin, i + 1U - begin, error);
      begin = i + 1U;
      line_start = 1;
    }
  }
  if (rc == CAI_OK && i > begin)
    rc = cai_sink_write(sink, r->data + begin, i - begin, error);
  if (rc == CAI_OK && r->final && !line_start)
    rc = execution_text(sink, "\n", error);
  return rc;
}
