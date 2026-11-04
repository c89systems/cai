#ifndef CAI_SESSION_CUT_H
#define CAI_SESSION_CUT_H
/* Shared private journal-header reader and cut planner for the file and Pouch
 * stores. The including store supplies CAI_CUT_FAIL(error, message). Bodies
 * flow through a discard sink; only bounded headers/ranges are retained. */
#include <cai/session_store.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define CAI_CUT_MAX_RANGES 64U
/** Prefix metadata locating retained context; history bytes are discarded. */
typedef struct cai_cut_edit_header {
  lonejson_uint64 revision, base_bytes, end_bytes;
  lonejson_json_value history;
} cai_cut_edit_header;
static const lonejson_field cai_cut_edit_fields[] = {
    LONEJSON_FIELD_U64(cai_cut_edit_header, revision, "revision"),
    LONEJSON_FIELD_U64(cai_cut_edit_header, base_bytes, "base_bytes"),
    LONEJSON_FIELD_U64(cai_cut_edit_header, end_bytes, "end_bytes"),
    LONEJSON_FIELD_JSON_VALUE(cai_cut_edit_header, history, "history_append")};
LONEJSON_MAP_DEFINE(cai_cut_edit_map, cai_cut_edit_header, cai_cut_edit_fields);
/** Bounded envelope fields retained from each record during cut planning. */
typedef struct cai_cut_header {
  char record_type[24];
  lonejson_uint64 sequence;
  cai_cut_edit_header state;
  lonejson_json_value data;
} cai_cut_header;
static const lonejson_field cai_cut_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(cai_cut_header, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(cai_cut_header, sequence, "sequence"),
    LONEJSON_FIELD_OBJECT(cai_cut_header, state, "state", &cai_cut_edit_map),
    LONEJSON_FIELD_JSON_VALUE(cai_cut_header, data, "data")};
LONEJSON_MAP_DEFINE(cai_cut_map, cai_cut_header, cai_cut_fields);
/** Store-position deletion interval: after is exclusive, through inclusive. */
typedef struct cai_cut_range {
  unsigned long long after, through;
} cai_cut_range;
/** Atomic-cut candidate with an owned parser and fixed range sets.
 * Authenticate each stored entry before advancing the plan. */
typedef struct cai_cut_plan {
  const cai_agent_session_trim_request *request;
  unsigned long long anchor, revision, bytes, last_position, last_sequence;
  unsigned long long last_revision, last_bytes, suffix;
  cai_cut_range events[CAI_CUT_MAX_RANGES], commits[CAI_CUT_MAX_RANGES];
  cai_cut_range ranges[CAI_CUT_MAX_RANGES * 2U + 1U];
  size_t event_count, commit_count, count;
  lonejson *json;
  lonejson_curl_parse parser;
  cai_cut_header record;
  int initialized;
} cai_cut_plan;
/** Open a parser retaining bounded headers and discarding record bodies. */
static lonejson *cai_cut_json(lonejson_error *error) {
  lonejson_config config = lonejson_default_config();
  /* Preserve installed discard sinks. Streamed bodies need no materialized
   * JSON-value/string limit; bounded headers keep their normal limits. */
  config.clear_destination_by_default = 0;
  config.json_value_max_total_bytes = 0U;
  config.json_value_max_string_bytes = 0U;
  return lonejson_new(&config, error);
}
/** Consume payload bytes without retaining context or event text. */
static lonejson_status cai_cut_discard(void *context, const void *bytes,
                                       size_t length, lonejson_error *error) {
  (void)context;
  (void)bytes;
  (void)length;
  (void)error;
  return LONEJSON_STATUS_OK;
}
/** Release the current record while preserving accumulated deletion ranges. */
static void cai_cut_record_close(cai_cut_plan *p) {
  if (p->initialized) {
    lonejson_curl_parse_cleanup(&p->parser);
    lonejson_cleanup(&cai_cut_map, &p->record);
    p->initialized = 0;
  }
}
/** Feed JSON fragments; cai_cut_end authenticates the enclosing stored entry.
 */
static int cai_cut_feed(cai_cut_plan *p, const void *bytes, size_t length,
                        cai_error *error) {
  const unsigned char *input = bytes;
  char chunk[2048];
  size_t amount;
  lonejson_error local;
  if (!p->initialized) {
    memset(&p->record, 0, sizeof(p->record));
    lonejson_json_value_init(p->json, &p->record.data);
    lonejson_json_value_init(p->json, &p->record.state.history);
    p->initialized = 1;
    if (lonejson_json_value_set_parse_sink(&p->record.data, cai_cut_discard,
                                           NULL,
                                           &local) != LONEJSON_STATUS_OK ||
        lonejson_json_value_set_parse_sink(&p->record.state.history,
                                           cai_cut_discard, NULL,
                                           &local) != LONEJSON_STATUS_OK ||
        lonejson_curl_parse_init(&p->parser, p->json, &cai_cut_map,
                                 &p->record) != LONEJSON_STATUS_OK)
      return CAI_CUT_FAIL(error, "prepare history cut parser");
  }
  while (length != 0U) {
    amount = length < sizeof(chunk) ? length : sizeof(chunk);
    memcpy(chunk, input, amount);
    if (lonejson_curl_write_callback(chunk, 1U, amount, &p->parser) != amount)
      return CAI_CUT_FAIL(error, p->parser.error.message);
    input += amount;
    length -= amount;
  }
  return CAI_OK;
}
/** Add a selected position, merging adjacency within the atomic range cap. */
static int cai_cut_add(cai_cut_range *ranges, size_t *count,
                       unsigned long long position, cai_error *error) {
  if (*count != 0U && ranges[*count - 1U].through == position - 1U) {
    ranges[*count - 1U].through = position;
    return CAI_OK;
  }
  if (*count == CAI_CUT_MAX_RANGES)
    return CAI_CUT_FAIL(error, "history cut exceeds atomic range limit");
  ranges[*count].after = position - 1U;
  ranges[*count].through = position;
  (*count)++;
  return CAI_OK;
}
/** Accept an authenticated ordered entry, validate its prefix and select cuts.
 */
static int cai_cut_end(cai_cut_plan *p, unsigned long long position,
                       cai_error *error) {
  size_t i;
  int rc = CAI_OK, selected = 0;
  if (position == 0U || position <= p->last_position || !p->initialized ||
      lonejson_curl_parse_finish(&p->parser) != LONEJSON_STATUS_OK)
    return CAI_CUT_FAIL(error, "invalid ordered history cut record");
  p->last_position = position;
  if (strcmp(p->record.record_type, "commit") == 0) {
    if (p->last_revision == ULLONG_MAX ||
        p->record.state.revision != p->last_revision + 1U ||
        p->record.state.base_bytes > p->last_bytes ||
        p->record.state.end_bytes < p->record.state.base_bytes)
      rc = CAI_CUT_FAIL(error, "history cut has an invalid commit prefix");
    else {
      p->last_revision = p->record.state.revision;
      p->last_bytes = p->record.state.end_bytes;
      if (p->suffix == 0U && p->last_bytes <= p->request->history_bytes) {
        p->anchor = position;
        p->revision = p->last_revision;
        p->bytes = p->last_bytes;
        p->commit_count = 0U;
      } else if (p->suffix == 0U)
        rc = cai_cut_add(p->commits, &p->commit_count, position, error);
    }
  } else if (strcmp(p->record.record_type, "event") == 0) {
    if (p->record.sequence == 0U || p->record.sequence <= p->last_sequence ||
        p->record.sequence > p->request->through_sequence)
      rc = CAI_CUT_FAIL(error, "history changed during atomic cut preparation");
    else {
      p->last_sequence = p->record.sequence;
      if (p->last_sequence >= p->request->first_sequence && p->suffix == 0U)
        p->suffix = position;
      if (p->suffix == 0U) {
        for (i = 0U; i < p->request->range_count; i++)
          if (p->last_sequence >= p->request->ranges[i].first &&
              p->last_sequence <= p->request->ranges[i].last)
            selected = 1;
        if (selected)
          rc = cai_cut_add(p->events, &p->event_count, position, error);
      }
    }
  } else
    rc = CAI_CUT_FAIL(error, "unknown history cut record type");
  cai_cut_record_close(p);
  return rc;
}
/** Sort deletion intervals by their exclusive lower store position. */
static int cai_cut_compare(const void *a, const void *b) {
  const cai_cut_range *left = a, *right = b;
  return left->after < right->after ? -1 : left->after != right->after;
}
/** Merge selected event/commit ranges and the discarded suffix into one plan.
 */
static int cai_cut_finish(cai_cut_plan *p, cai_error *error) {
  size_t i, count = 0U;
  if (p->last_revision == 0U || p->bytes > (size_t)-1)
    return CAI_CUT_FAIL(error, "history cut has no valid commit");
  memcpy(p->ranges, p->events, p->event_count * sizeof(*p->ranges));
  memcpy(p->ranges + p->event_count, p->commits,
         p->commit_count * sizeof(*p->ranges));
  p->count = p->event_count + p->commit_count;
  if (p->suffix != 0U) {
    p->ranges[p->count].after = p->suffix - 1U;
    p->ranges[p->count++].through = p->last_position;
  }
  qsort(p->ranges, p->count, sizeof(*p->ranges), cai_cut_compare);
  for (i = 0U; i < p->count; i++) {
    if (count != 0U && p->ranges[i].after <= p->ranges[count - 1U].through) {
      if (p->ranges[i].through > p->ranges[count - 1U].through)
        p->ranges[count - 1U].through = p->ranges[i].through;
    } else
      p->ranges[count++] = p->ranges[i];
  }
  p->count = count;
  return count <= CAI_CUT_MAX_RANGES
             ? CAI_OK
             : CAI_CUT_FAIL(error, "history cut exceeds atomic range limit");
}
/** Test whether a store position is included in the prepared deletion plan. */
static int cai_cut_removed(const cai_cut_plan *p, unsigned long long position) {
  size_t i;
  for (i = 0U; i < p->count; i++)
    if (position > p->ranges[i].after && position <= p->ranges[i].through)
      return 1;
  return 0;
}
#endif
