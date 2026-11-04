#include <assert.h>
#include <cai/execution.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
static int watch_allocations;
static size_t largest_allocation;
static size_t fail_reallocation;
static void allocation(size_t n) {
  if (watch_allocations && n > largest_allocation)
    largest_allocation = n;
}
void *__wrap_malloc(size_t n) {
  allocation(n);
  return __real_malloc(n);
}
void *__wrap_calloc(size_t count, size_t size) {
  allocation(count != 0U && size > (size_t)-1 / count ? (size_t)-1
                                                      : count * size);
  return __real_calloc(count, size);
}
void *__wrap_realloc(void *p, size_t n) {
  allocation(n);
  if (fail_reallocation != 0U && --fail_reallocation == 0U)
    return NULL;
  return __real_realloc(p, n);
}

/* Deliberately not producer field order. Metadata is authoritative only at
 * finish, and output chunks do not require retaining their encoded JSON. */
static const char document[] =
    "{\"data\":\"one\\nλ\\uD83D\\uDE00\\\"\\\\\",\"offset\":4294967296,"
    "\"kind\":\"tool.output\",\"session_id\":\"s\","
    "\"timestamp_ns\":18446744073709551615,\"utc_offset_seconds\":-20700,"
    "\"call_id\":\"c\",\"tool_name\":\"exec_command\",\"final\":true,"
    "\"line_start\":false,\"code\":-9223372036854775808,\"http_status\":429,"
    "\"parent_session_id\":\"p\",\"parent_call_id\":\"pc\","
    "\"subagent\":\"review\",\"source_sequence\":99,\"field\":\"stdout\","
    "\"model\":\"m\",\"base_url\":\"https://example.test\","
    "\"last_response_id\":\"r\"}";
static void assert_record(const cai_execution_record *r) {
  assert(strcmp(r->data, "one\nλ😀\"\\") == 0);
  assert(strcmp(r->kind, "tool.output") == 0 && strcmp(r->call_id, "c") == 0);
  assert(strcmp(r->session_id, "s") == 0 &&
         strcmp(r->parent_session_id, "p") == 0 &&
         strcmp(r->parent_call_id, "pc") == 0 &&
         strcmp(r->subagent, "review") == 0 &&
         strcmp(r->tool_name, "exec_command") == 0 &&
         strcmp(r->field, "stdout") == 0 && strcmp(r->model, "m") == 0 &&
         strcmp(r->base_url, "https://example.test") == 0 &&
         strcmp(r->last_response_id, "r") == 0);
  assert(r->offset == 4294967296ULL &&
         r->timestamp_ns == 18446744073709551615ULL &&
         r->code == (-9223372036854775807LL - 1LL) && r->http_status == 429 &&
         r->utc_offset_seconds == -20700 && r->final && !r->line_start &&
         r->source_sequence == 99U);
}
static void fragmentation(cai_execution_decoder *d, cai_execution_record *r) {
  size_t size, offset, n, length = strlen(document);
  char *input = malloc(length);
  assert(input != NULL);
  for (size = 1U; size <= length; ++size) {
    assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
    memcpy(input, document, length);
    for (offset = 0U; offset < length; offset += n) {
      n = length - offset;
      if (n > size)
        n = size;
      assert(cai_execution_decoder_feed(d, input + offset, n, NULL) == CAI_OK);
      memset(input + offset, '!', n); /* No source bytes borrowed by decoder. */
    }
    assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
    assert_record(r);
    assert(cai_execution_decoder_feed(d, "", 0U, NULL) == CAI_ERR_INVALID);
    assert(cai_execution_decoder_finish(d, r, NULL) == CAI_ERR_INVALID);
    assert_record(r);
  }
  free(input);
  /* Every truncated prefix fails without replacing the previous owned value. */
  for (n = 0U; n < length; ++n) {
    int rc;
    assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
    rc = cai_execution_decoder_feed(d, document, n, NULL);
    if (rc == CAI_OK)
      rc = cai_execution_decoder_finish(d, r, NULL);
    assert(rc == CAI_ERR_PROTOCOL);
    assert_record(r);
    assert(cai_execution_decoder_feed(d, "}", 1U, NULL) == rc);
  }
}
static void limits(cai_execution_decoder *d, cai_execution_record *r) {
  static const char prefix[] =
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":1,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"data\":\"";
  char *encoded = malloc(CAI_EXECUTION_RECORD_MAX_BYTES + 1U);
  size_t offset, i;
  assert(encoded != NULL);
  memcpy(encoded, document, strlen(document));
  memset(encoded + strlen(document), ' ',
         CAI_EXECUTION_RECORD_MAX_BYTES + 1U - strlen(document));
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, encoded, CAI_EXECUTION_RECORD_MAX_BYTES,
                                    NULL) == CAI_OK);
  assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
  assert_record(r);
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, encoded,
                                    CAI_EXECUTION_RECORD_MAX_BYTES + 1U,
                                    NULL) == CAI_ERR_LIMIT);
  assert_record(r);
  /* Maximum decoded chunk, represented by six-byte JSON escapes. */
  offset = sizeof(prefix) - 1U;
  memcpy(encoded, prefix, offset);
  for (i = 0U; i < CAI_EXECUTION_CHUNK_BYTES; ++i) {
    memcpy(encoded + offset, "\\u03bb", 6U);
    offset += 6U;
  }
  /* Lambda is two UTF-8 bytes, so half this number exactly fills data. */
  offset = sizeof(prefix) - 1U + (CAI_EXECUTION_CHUNK_BYTES / 2U) * 6U;
  memcpy(encoded + offset, "\"}", 2U);
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, encoded, offset + 2U, NULL) == CAI_OK);
  assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
  assert(strlen(r->data) == CAI_EXECUTION_CHUNK_BYTES && r->data[0] == '\316');
  encoded[offset] = 'x';
  memcpy(encoded + offset + 1U, "\"}", 2U);
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, encoded, offset + 3U, NULL) ==
         CAI_ERR_PROTOCOL);
  assert(strlen(r->data) == CAI_EXECUTION_CHUNK_BYTES);
  free(encoded);
}
static void invalid(cai_execution_decoder *d, cai_execution_record *r) {
  static const char *values[] = {
      "{}",
      "[]",
      "null",
      "{",
      "{}{}",
      "{\"kind\":\"\"}",
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":0,"
      "\"offset\":-1,\"final\":true,\"line_start\":true,\"data\":\"\"}",
      "{\"kind\":\"tool.output\",\"session_id\":\"s\",\"timestamp_ns\":0,"
      "\"offset\":0,\"final\":true,\"line_start\":true,\"data\":\"\\u0000\"}",
      "{\"data\":\"\300\"}",
      "{\"unknown\":[\"\\u0000\"]}",
      "{\"unk\\u0000nown\":true}",
      "{\"unknown\":\"\\ud800\"}",
      "{\"data\":\"\\udc00\"}",
      "{\"data\":\"\\ud800x\"}"};
  size_t i, offset;
  for (i = 0U; i < sizeof(values) / sizeof(values[0]); ++i) {
    int rc = CAI_OK;
    assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
    for (offset = 0U; rc == CAI_OK && values[i][offset] != '\0'; ++offset)
      rc = cai_execution_decoder_feed(d, values[i] + offset, 1U, NULL);
    if (rc == CAI_OK)
      rc = cai_execution_decoder_finish(d, r, NULL);
    assert(rc == CAI_ERR_PROTOCOL && r->data[0] != '\0');
  }
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, NULL, 1U, NULL) == CAI_ERR_INVALID);
  assert(cai_execution_decoder_feed(d, document, strlen(document), NULL) ==
         CAI_ERR_INVALID);
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_finish(d, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_execution_decoder_feed(d, document, strlen(document), NULL) ==
         CAI_OK);
  assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
  assert_record(r);
}
static void no_json_capture(cai_execution_decoder *d, cai_execution_record *r) {
  static const char prefix[] = "{\"unknown\":\"";
  size_t count = 60000U, offset, length;
  char *input = malloc(CAI_EXECUTION_RECORD_MAX_BYTES);
  assert(input != NULL);
  offset = sizeof(prefix) - 1U;
  memcpy(input, prefix, offset);
  memset(input + offset, 'x', count);
  offset += count;
  memcpy(input + offset, "\",", 2U);
  offset += 2U;
  memcpy(input + offset, document + 1U, strlen(document) - 1U);
  length = offset + strlen(document) - 1U;
  largest_allocation = 0U;
  watch_allocations = 1;
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  for (offset = 0U; offset < length;) {
    size_t n = length - offset;
    cai_error error;
    int rc;
    if (n > 257U)
      n = 257U;
    cai_error_init(&error);
    rc = cai_execution_decoder_feed(d, input + offset, n, &error);
    if (rc != CAI_OK)
      fprintf(stderr, "execution chunk at %lu: %s: %s (largest %lu)\n",
              (unsigned long)offset, error.message != NULL ? error.message : "",
              error.detail != NULL ? error.detail : "",
              (unsigned long)largest_allocation);
    cai_error_cleanup(&error);
    assert(rc == CAI_OK);
    offset += n;
  }
  assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
  watch_allocations = 0;
  assert_record(r);
  /* Known metadata/data are small. Neither parser may gather the long skipped
   * JSON string or the whole encoded record behind its push interface. */
  assert(largest_allocation < 32768U);
  free(input);
}
static void types(cai_execution_decoder *d, cai_execution_record *r) {
  static const struct {
    const char *needle, *replacement;
    int valid;
  } cases[] = {
      {"\"offset\":4294967296", "\"offset\":18446744073709551616", 0},
      {"\"offset\":4294967296", "\"offset\":-1", 0},
      {"\"offset\":4294967296", "\"offset\":1.0", 0},
      {"\"offset\":4294967296", "\"offset\":1e1", 0},
      {"\"offset\":4294967296", "\"offset\":\"1\"", 0},
      {"\"offset\":4294967296", "\"offset\":null", 0},
      {"\"offset\":4294967296", "\"offset\":[]", 0},
      {"\"offset\":4294967296", "\"offset\":{}", 0},
      {"\"offset\":4294967296", "\"offset\":true", 0},
      {"\"code\":-9223372036854775808", "\"code\":-9223372036854775809", 0},
      {"\"code\":-9223372036854775808", "\"code\":9223372036854775808", 0},
      {"\"code\":-9223372036854775808", "\"code\":0.5", 0},
      {"\"final\":true", "\"final\":0", 0},
      {"\"final\":true", "\"final\":null", 0},
      {"\"final\":true", "\"final\":\"true\"", 0},
      {"\"session_id\":\"s\"", "\"session_id\":\"\"", 0},
      {"\"session_id\":\"s\"", "\"session_id\":null", 0},
      {"\"session_id\":\"s\"", "\"session_id\":12", 0},
      {"\"session_id\":\"s\"", "\"session_id\":true", 0},
      {"\"kind\":\"tool.output\"", "\"kind\":\"\"", 0},
      {"\"kind\":\"tool.output\"",
       "\"kind\":\"tool.output\",\"kind\":\"tool.failed\"", 0},
      {"\"model\":\"m\"", "\"model\":null", 1},
      {"\"model\":\"m\"", "\"model\":\"\"", 1},
      {"\"model\":\"m\"",
       "\"model\":\"m\",\"unknown\":["
       "{\"kind\":false,\"data\":null},[],true,1.25e30,\"λ\"]",
       2}};
  char *json = malloc(2048U);
  size_t i;
  assert(json != NULL);
  for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const char *found = strstr(document, cases[i].needle);
    size_t prefix, replacement, suffix, n;
    int rc = CAI_OK;
    assert(found != NULL);
    prefix = (size_t)(found - document);
    replacement = strlen(cases[i].replacement);
    suffix = prefix + strlen(cases[i].needle);
    memcpy(json, document, prefix);
    memcpy(json + prefix, cases[i].replacement, replacement);
    memcpy(json + prefix + replacement, document + suffix,
           strlen(document) - suffix + 1U);
    assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
    for (n = 0U; rc == CAI_OK && json[n] != '\0'; ++n)
      rc = cai_execution_decoder_feed(d, json + n, 1U, NULL);
    if (rc == CAI_OK)
      rc = cai_execution_decoder_finish(d, r, NULL);
    assert(rc == (cases[i].valid ? CAI_OK : CAI_ERR_PROTOCOL));
    if (!cases[i].valid || cases[i].valid == 2)
      assert_record(r);
    else
      assert((r->model == NULL || r->model[0] == '\0') &&
             strcmp(r->call_id, "c") == 0);
  }
  free(json);
}
static void allocation_failures(cai_execution_decoder *d,
                                cai_execution_record *r) {
  size_t i;
  for (i = 1U; i <= 11U; ++i) {
    int rc;
    assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
    fail_reallocation = i;
    rc = cai_execution_decoder_feed(d, document, strlen(document), NULL);
    fail_reallocation = 0U;
    assert(rc == CAI_ERR_NOMEM);
    assert_record(
        r); /* No partial replacement or loss of the previous chunk. */
    assert(cai_execution_decoder_finish(d, r, NULL) == CAI_ERR_NOMEM);
  }
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, document, strlen(document), NULL) ==
         CAI_OK);
  assert(cai_execution_decoder_finish(d, r, NULL) == CAI_OK);
  assert_record(r);
}
static void *run(void *context) {
  cai_execution_decoder *d = NULL;
  cai_execution_record *record = calloc(1U, sizeof(*record));
  (void)context;
  assert(record != NULL);
  assert(cai_execution_decoder_open(&d, NULL) == CAI_OK);
  fragmentation(d, record);
  limits(d, record);
  invalid(d, record);
  no_json_capture(d, record);
  types(d, record);
  allocation_failures(d, record);
  assert(cai_execution_decoder_reset(d, NULL) == CAI_OK);
  assert(cai_execution_decoder_feed(d, document, 5U, NULL) == CAI_OK);
  cai_execution_decoder_close(d); /* Discard without a published record. */
  assert_record(record);
  cai_execution_record_cleanup(record);
  free(record);
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(cai_execution_decoder_open(NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_execution_decoder_feed(NULL, NULL, 0U, NULL) == CAI_ERR_INVALID);
  assert(cai_execution_decoder_reset(NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_execution_decoder_finish(NULL, NULL, NULL) == CAI_ERR_INVALID);
  cai_execution_decoder_close(NULL);
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
