#define LONEJSON_WITH_CURL
#include "journal_projection.h"
#include "pouch_stream.h"
#include "transcript.h"
#include <cai/history.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These are indexes in the durable journal schema, not model payloads. A block
 * summary is the bounded UI label used by the history selector. */
typedef struct block {
  lonejson_int64 first_item, event_start, input_sequence, kind;
  char summary[1024];
} block;
typedef struct hidden {
  lonejson_int64 first, last;
} hidden;
typedef struct owned {
  lonejson_int64 entry, sequence;
} owned;
static const lonejson_field block_fields[] = {
    LONEJSON_FIELD_I64_REQ(block, first_item, "first_item"),
    LONEJSON_FIELD_I64_REQ(block, event_start, "event_start"),
    LONEJSON_FIELD_I64_REQ(block, input_sequence, "input_sequence"),
    LONEJSON_FIELD_I64_REQ(block, kind, "kind"),
    LONEJSON_FIELD_STRING_FIXED(block, summary, "summary",
                                LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(block_map, block, block_fields);
static const lonejson_field hidden_fields[] = {
    LONEJSON_FIELD_I64_REQ(hidden, first, "first"),
    LONEJSON_FIELD_I64_REQ(hidden, last, "last")};
LONEJSON_MAP_DEFINE(hidden_map, hidden, hidden_fields);
static const lonejson_field owned_fields[] = {
    LONEJSON_FIELD_I64_REQ(owned, entry, "entry"),
    LONEJSON_FIELD_I64_REQ(owned, sequence, "sequence")};
LONEJSON_MAP_DEFINE(owned_map, owned, owned_fields);
typedef struct metadata {
  lonejson_int64 version;
  lonejson_object_array blocks, hidden, owned;
  lonejson_json_value history;
  lonejson_json_value strings[15];
} metadata;
static const lonejson_field metadata_fields[] = {
    LONEJSON_FIELD_I64_REQ(metadata, version, "version"),
    LONEJSON_FIELD_OBJECT_ARRAY(metadata, blocks, "history_blocks", block,
                                &block_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(metadata, hidden, "history_hidden_events",
                                hidden, &hidden_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_OBJECT_ARRAY(metadata, owned, "history_owned_events", owned,
                                &owned_map, LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_JSON_VALUE(metadata, history, "history"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[0], "model"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[1], "model_compaction_hash"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[2], "request_model"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[3], "reasoning_effort"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[4], "reasoning_summary"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[5], "service_tier"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[6], "todo_board"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[7], "preset_name"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[8], "preset_prompt_version"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[9], "previous_response_id"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[10], "conversation_id"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[11], "goal_objective"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[12], "goal_status"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[13], "goal_input_applied"),
    LONEJSON_FIELD_JSON_VALUE(metadata, strings[14],
                              "input_ack_start_sequence")};
LONEJSON_MAP_DEFINE(metadata_map, metadata, metadata_fields);
typedef struct accounting {
  lonejson_int64 input, cached, output, reasoning, total;
  double usd, eur;
} accounting;
static const lonejson_field accounting_fields[] = {
    LONEJSON_FIELD_I64(accounting, input, "input_tokens"),
    LONEJSON_FIELD_I64(accounting, cached, "cached_tokens"),
    LONEJSON_FIELD_I64(accounting, output, "output_tokens"),
    LONEJSON_FIELD_I64(accounting, reasoning, "reasoning_tokens"),
    LONEJSON_FIELD_I64(accounting, total, "total_tokens"),
    LONEJSON_FIELD_F64(accounting, usd, "usd"),
    LONEJSON_FIELD_F64(accounting, eur, "eur")};
LONEJSON_MAP_DEFINE(accounting_map, accounting, accounting_fields);
typedef struct edit {
  lonejson_uint64 revision, base, end, blocks_keep, hidden_keep, owned_keep;
  metadata metadata;
  accounting accounting;
  lonejson_json_value history, checkpoint;
} edit;
static const lonejson_field edit_fields[] = {
    LONEJSON_FIELD_U64_REQ(edit, revision, "revision"),
    LONEJSON_FIELD_U64_REQ(edit, base, "base_bytes"),
    LONEJSON_FIELD_U64_REQ(edit, end, "end_bytes"),
    LONEJSON_FIELD_U64_REQ(edit, blocks_keep, "blocks_keep"),
    LONEJSON_FIELD_U64_REQ(edit, hidden_keep, "hidden_keep"),
    LONEJSON_FIELD_U64_REQ(edit, owned_keep, "owned_keep"),
    LONEJSON_FIELD_OBJECT(edit, metadata, "metadata", &metadata_map),
    LONEJSON_FIELD_OBJECT(edit, accounting, "accounting", &accounting_map),
    LONEJSON_FIELD_JSON_VALUE_REQ(edit, history, "history_append"),
    LONEJSON_FIELD_JSON_VALUE(edit, checkpoint, "input_checkpoint")};
LONEJSON_MAP_DEFINE(edit_map, edit, edit_fields);
typedef struct record {
  char record_type[24];
  lonejson_uint64 created, applied;
  edit state;
  lonejson_json_value data;
} record;
static const lonejson_field record_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(record, record_type, "record_type",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_U64(record, created, "commit_created_at_ns"),
    LONEJSON_FIELD_U64(record, applied, "applied_event_sequence"),
    LONEJSON_FIELD_OBJECT(record, state, "state", &edit_map),
    LONEJSON_FIELD_JSON_VALUE(record, data, "data")};
LONEJSON_MAP_DEFINE(record_map, record, record_fields);
typedef struct copy_record {
  lonejson_json_value state, data;
} copy_record;
static const lonejson_field copy_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(copy_record, state, "state"),
    LONEJSON_FIELD_JSON_VALUE(copy_record, data, "data")};
LONEJSON_MAP_DEFINE(copy_map, copy_record, copy_fields);

typedef struct span {
  uint64_t position, revision, length;
} span;
typedef enum pass { INDEX, EVENTS, METADATA, CHECKPOINT, HISTORY } pass;
typedef struct projection {
  lc_collection *collection;
  lc_collection_head_res head;
  cai_sink *sink;
  cai_error *error;
  lonejson *json;
  lonejson_writer writer;
  lonejson_curl_parse parser;
  lonejson_curl_parse frame_parser;
  lonejson_value_visitor history_visitor, checkpoint_visitor, forbidden_visitor;
  lonejson_path_value_visitor copy_visitor;
  record record;
  copy_record copy;
  copy_record frame;
  metadata metadata;
  accounting accounting;
  span *spans;
  size_t count, capacity, next_span;
  uint64_t revision, bytes, position, last_commit, checkpoint;
  uint64_t created, applied, decoded, remaining;
  uint64_t current_revision, frame_length, frame_remaining, frame_count;
  unsigned int revision_digits, frame_digits;
  int frame_phase, frame_started;
  char key[128];
  size_t key_length;
  int active, rc, history_seen, checkpoint_seen, copied;
  pass pass;
  cai_cli_transcript *transcript;
  int visible;
} projection;
static int fail(projection *p, int code, const char *message) {
  if (p->rc == CAI_OK) {
    char detail[320];
    static const char *const names[] = {"index", "events", "metadata",
                                        "checkpoint", "history"};
    snprintf(detail, sizeof(detail),
             "journal export %s after position %llu: %s", names[p->pass],
             (unsigned long long)p->position, message);
    p->rc = cai_cli_error(p->error, code, detail);
  }
  return p->rc;
}
static lonejson_status status(projection *p, lonejson_status rc,
                              lonejson_error *error) {
  if (rc != LONEJSON_STATUS_OK)
    fail(p, CAI_ERR_PROTOCOL, error->message);
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
static lonejson_status bad(void *context, lonejson_error *error) {
  projection *p = context;
  (void)error;
  fail(p, CAI_ERR_PROTOCOL, "invalid journal projection field type");
  return LONEJSON_STATUS_INVALID_JSON;
}
static lonejson_status bad_bool(void *context, int value,
                                lonejson_error *error) {
  (void)value;
  return bad(context, error);
}
static lonejson_status history_begin(void *context, lonejson_error *error) {
  projection *p = context;
  edit *e = &p->record.state;
  if (strcmp(p->record.record_type, "commit") != 0 || p->history_seen ||
      p->revision == UINT64_MAX || e->revision != p->revision + 1U ||
      e->base > p->bytes || e->end < e->base)
    return bad(context, error);
  p->history_seen = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status history_chunk(void *context, const char *data,
                                     size_t length, lonejson_error *error) {
  projection *p = context;
  (void)data;
  (void)error;
  if (length > UINT64_MAX - p->decoded)
    return bad(context, error);
  p->decoded += length;
  return LONEJSON_STATUS_OK;
}
static lonejson_status checkpoint_begin(void *context, lonejson_error *error) {
  projection *p = context;
  if (strcmp(p->record.record_type, "commit") != 0)
    return bad(context, error);
  p->checkpoint_seen = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status checkpoint_scalar(void *context, lonejson_error *error) {
  projection *p = context;
  return p->checkpoint_seen ? LONEJSON_STATUS_OK : bad(context, error);
}
static lonejson_status checkpoint_bool(void *context, int value,
                                       lonejson_error *error) {
  (void)value;
  return checkpoint_scalar(context, error);
}
static int fold(projection *p, lonejson_object_array *out,
                const lonejson_object_array *tail, uint64_t keep,
                size_t width) {
  void *items;
  size_t count;
  if (keep > out->count || tail->count > SIZE_MAX / width - (size_t)keep)
    return fail(p, CAI_ERR_PROTOCOL, "invalid journal metadata prefix");
  count = (size_t)keep + tail->count;
  if (count > out->capacity) {
    items = realloc(out->items, count * width);
    if (items == NULL)
      return fail(p, CAI_ERR_NOMEM, "allocate journal metadata index");
    out->items = items;
    out->capacity = count;
  }
  if (tail->count != 0U)
    memcpy((char *)out->items + (size_t)keep * width, tail->items,
           tail->count * width);
  out->count = count;
  return CAI_OK;
}
static int index_commit(projection *p, uint64_t position) {
  edit *e = &p->record.state;
  uint64_t at = 0U;
  size_t i;
  span *spans;
  if (p->revision == UINT64_MAX || e->revision != p->revision + 1U ||
      !p->history_seen || e->base > p->bytes || e->end < e->base ||
      p->decoded != e->end - e->base || e->metadata.version != 1 ||
      p->record.created == 0U || p->record.applied < p->applied)
    return fail(p, CAI_ERR_PROTOCOL,
                "invalid journal edit revision or byte range");
  if (e->accounting.input < 0 || e->accounting.cached < 0 ||
      e->accounting.output < 0 || e->accounting.reasoning < 0 ||
      e->accounting.total < 0 || !(e->accounting.usd >= 0.0) ||
      !(e->accounting.eur >= 0.0))
    return fail(p, CAI_ERR_PROTOCOL, "invalid journal usage accounting");
  for (i = 0U; i < p->count && at < e->base; i++) {
    uint64_t length = p->spans[i].length;
    if (length > e->base - at)
      p->spans[i].length = length = e->base - at;
    at += length;
  }
  p->count = i;
  if (p->decoded != 0U) {
    if (p->count == p->capacity) {
      size_t capacity = p->capacity == 0U ? 16U : p->capacity * 2U;
      if (capacity < p->capacity || capacity > SIZE_MAX / sizeof(*spans))
        return fail(p, CAI_ERR_NOMEM, "journal index exceeds address space");
      spans = realloc(p->spans, capacity * sizeof(*spans));
      if (spans == NULL)
        return fail(p, CAI_ERR_NOMEM, "allocate journal byte references");
      p->spans = spans;
      p->capacity = capacity;
    }
    p->spans[p->count].position = position;
    p->spans[p->count].revision = e->revision;
    p->spans[p->count++].length = p->decoded;
  }
  if (fold(p, &p->metadata.blocks, &e->metadata.blocks, e->blocks_keep,
           sizeof(block)) != CAI_OK ||
      fold(p, &p->metadata.hidden, &e->metadata.hidden, e->hidden_keep,
           sizeof(hidden)) != CAI_OK ||
      fold(p, &p->metadata.owned, &e->metadata.owned, e->owned_keep,
           sizeof(owned)) != CAI_OK)
    return p->rc;
  p->revision = e->revision;
  p->bytes = e->end;
  p->last_commit = position;
  p->created = p->record.created;
  p->applied = p->record.applied;
  p->accounting = e->accounting;
  if (p->checkpoint_seen)
    p->checkpoint = position;
  return CAI_OK;
}

/* Path forwarding copies only the selected field. In metadata, root array
 * members are replaced by their folded prefixes/tails after this read ends.
 * Keys are schema identifiers; unbounded provider text is always a value. */
static int segment(const lonejson_value_path *path, size_t n, const char *key) {
  return n < path->segment_count && path->segments[n].len == strlen(key) &&
         memcmp(path->segments[n].data, key, strlen(key)) == 0;
}
static int array_key(const char *key, size_t length) {
  static const char *const names[] = {"history_blocks", "history_hidden_events",
                                      "history_owned_events"};
  size_t i;
  for (i = 0U; i < 3U; i++)
    if (strlen(names[i]) == length && memcmp(names[i], key, length) == 0)
      return 1;
  return 0;
}
static int selected(projection *p, const lonejson_value_path *path) {
  const char *field = p->pass == METADATA ? "metadata" : "input_checkpoint";
  if (p->pass == HISTORY)
    return 0;
  if (!segment(path, 0U, field))
    return 0;
  if (p->pass == METADATA && path->segment_count > 1U &&
      array_key(path->segments[1].data, path->segments[1].len))
    return 0;
  return 1;
}
#define FORWARD_EVENT(name, method)                                            \
  static lonejson_status name(void *context, const lonejson_value_path *path,  \
                              lonejson_error *error) {                         \
    projection *p = context;                                                   \
    return !selected(p, path)                                                  \
               ? LONEJSON_STATUS_OK                                            \
               : status(p, p->writer.method(&p->writer, error), error);        \
  }
FORWARD_EVENT(copy_array_begin, begin_array)
FORWARD_EVENT(copy_array_end, end_array)
FORWARD_EVENT(copy_string_end, string_end)
FORWARD_EVENT(copy_null, null_fn)
#undef FORWARD_EVENT
static lonejson_status copy_object_begin(void *context,
                                         const lonejson_value_path *path,
                                         lonejson_error *error) {
  projection *p = context;
  if (!selected(p, path))
    return LONEJSON_STATUS_OK;
  if (path->segment_count == 1U)
    p->copied++;
  return status(p, p->writer.begin_object(&p->writer, error), error);
}
static lonejson_status copy_object_end(void *context,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection *p = context;
  if (!selected(p, path) || (p->pass == METADATA && path->segment_count == 1U))
    return LONEJSON_STATUS_OK;
  return status(p, p->writer.end_object(&p->writer, error), error);
}
static lonejson_status copy_string_begin(void *context,
                                         const lonejson_value_path *path,
                                         lonejson_error *error) {
  projection *p = context;
  if (p->pass == HISTORY)
    return LONEJSON_STATUS_OK;
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p, p->writer.string_begin(&p->writer, error), error);
}
static int parse_bytes(lonejson_curl_parse *parser, const char *data,
                       size_t length) {
  while (length != 0U) {
    char chunk[4096];
    size_t n = length < sizeof(chunk) ? length : sizeof(chunk);
    memcpy(chunk, data, n);
    if (lonejson_curl_write_callback(chunk, 1U, n, parser) != n)
      return 0;
    data += n;
    length -= n;
  }
  return 1;
}
/* Validate the selected length-framed JSON arrays as they pass. The wrapper
 * provides lonejson's ordinary incremental JSON-value parser; the value sink
 * discards tokens, without constructing provider items or a DOM. */
static int frame_bytes(projection *p, const char *data, size_t length) {
  lonejson_error local;
  while (length != 0U && p->rc == CAI_OK) {
    if (p->frame_phase == 0) {
      unsigned char ch = (unsigned char)*data++;
      length--;
      if (ch == '\n' && p->frame_digits != 0U && p->frame_length != 0U) {
        lonejson_json_value_init(p->json, &p->frame.state);
        if (lonejson_json_value_set_parse_sink(&p->frame.state, discard, NULL,
                                               &local) != LONEJSON_STATUS_OK ||
            lonejson_curl_parse_init(&p->frame_parser, p->json, &copy_map,
                                     &p->frame) != LONEJSON_STATUS_OK ||
            !parse_bytes(&p->frame_parser, "{\"state\":", 9U))
          return fail(p, CAI_ERR_PROTOCOL,
                      "initialize retained history validator");
        p->frame_remaining = p->frame_length;
        p->frame_phase = 1;
        p->frame_started = 0;
      } else if (ch < '0' || ch > '9' || p->frame_digits++ >= 20U ||
                 p->frame_length > (UINT64_MAX - (ch - '0')) / 10U)
        return fail(p, CAI_ERR_PROTOCOL, "invalid retained history length");
      else
        p->frame_length = p->frame_length * 10U + ch - '0';
    } else if (p->frame_phase == 1) {
      size_t n = length;
      if (n > p->frame_remaining)
        n = (size_t)p->frame_remaining;
      if (!p->frame_started) {
        size_t i;
        for (i = 0U; i < n; i++) {
          if (data[i] == ' ' || data[i] == '\r' || data[i] == '\n' ||
              data[i] == '\t')
            continue;
          if (data[i] != '[')
            return fail(p, CAI_ERR_PROTOCOL,
                        "retained history must contain JSON arrays");
          p->frame_started = 1;
          break;
        }
      }
      if (!parse_bytes(&p->frame_parser, data, n))
        return fail(p, CAI_ERR_PROTOCOL, p->frame_parser.error.message);
      data += n;
      length -= n;
      p->frame_remaining -= n;
      if (p->frame_remaining == 0U) {
        if (!p->frame_started || !parse_bytes(&p->frame_parser, "}", 1U) ||
            lonejson_curl_parse_finish(&p->frame_parser) != LONEJSON_STATUS_OK)
          return fail(p, CAI_ERR_PROTOCOL, "invalid retained provider JSON");
        lonejson_curl_parse_cleanup(&p->frame_parser);
        lonejson_cleanup(&copy_map, &p->frame);
        memset(&p->frame_parser, 0, sizeof(p->frame_parser));
        memset(&p->frame, 0, sizeof(p->frame));
        p->frame_phase = 2;
        p->frame_count++;
      }
    } else {
      if (*data++ != '\n')
        return fail(p, CAI_ERR_PROTOCOL, "invalid retained history delimiter");
      length--;
      p->frame_phase = 0;
      p->frame_digits = 0U;
      p->frame_length = 0U;
    }
  }
  return p->rc;
}
static lonejson_status copy_string_chunk(void *context,
                                         const lonejson_value_path *path,
                                         const char *data, size_t length,
                                         lonejson_error *error) {
  projection *p = context;
  if (p->pass == HISTORY) {
    if (!segment(path, 0U, "history_append") || path->segment_count != 1U)
      return LONEJSON_STATUS_OK;
    if (length > p->remaining)
      length = (size_t)p->remaining;
    if (frame_bytes(p, data, length) != CAI_OK)
      return LONEJSON_STATUS_INVALID_JSON;
    p->remaining -= length;
    return status(p, p->writer.string_chunk(&p->writer, data, length, error),
                  error);
  }
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p,
                      p->writer.string_chunk(&p->writer, data, length, error),
                      error);
}
static lonejson_status copy_number_begin(void *context,
                                         const lonejson_value_path *path,
                                         lonejson_error *error) {
  projection *p = context;
  if (p->pass == HISTORY && path->segment_count == 1U &&
      segment(path, 0U, "revision")) {
    p->current_revision = 0U;
    p->revision_digits = 0U;
  }
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p, p->writer.number_begin(&p->writer, error), error);
}
static lonejson_status copy_number_chunk(void *context,
                                         const lonejson_value_path *path,
                                         const char *data, size_t length,
                                         lonejson_error *error) {
  projection *p = context;
  if (p->pass == HISTORY && path->segment_count == 1U &&
      segment(path, 0U, "revision")) {
    size_t i;
    for (i = 0U; i < length; i++) {
      if (data[i] < '0' || data[i] > '9' || p->revision_digits++ >= 20U ||
          p->current_revision > (UINT64_MAX - (data[i] - '0')) / 10U)
        return bad(p, error);
      p->current_revision = p->current_revision * 10U + data[i] - '0';
    }
  }
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p,
                      p->writer.number_chunk(&p->writer, data, length, error),
                      error);
}
static lonejson_status copy_number_end(void *context,
                                       const lonejson_value_path *path,
                                       lonejson_error *error) {
  projection *p = context;
  if (p->pass == HISTORY && path->segment_count == 1U &&
      segment(path, 0U, "revision") && p->next_span < p->count &&
      p->current_revision == p->spans[p->next_span].revision)
    p->remaining = p->spans[p->next_span].length;
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p, p->writer.number_end(&p->writer, error), error);
}
static lonejson_status copy_bool(void *context, const lonejson_value_path *path,
                                 int value, lonejson_error *error) {
  projection *p = context;
  return !selected(p, path)
             ? LONEJSON_STATUS_OK
             : status(p, p->writer.bool_fn(&p->writer, value, error), error);
}
static lonejson_status copy_key_begin(void *context,
                                      const lonejson_value_path *path,
                                      lonejson_error *error) {
  projection *p = context;
  (void)error;
  if (selected(p, path))
    p->key_length = 0U;
  return LONEJSON_STATUS_OK;
}
static lonejson_status copy_key_chunk(void *context,
                                      const lonejson_value_path *path,
                                      const char *data, size_t length,
                                      lonejson_error *error) {
  projection *p = context;
  if (!selected(p, path))
    return LONEJSON_STATUS_OK;
  if (length > sizeof(p->key) - p->key_length)
    return bad(p, error);
  memcpy(p->key + p->key_length, data, length);
  p->key_length += length;
  return LONEJSON_STATUS_OK;
}
static lonejson_status copy_key_end(void *context,
                                    const lonejson_value_path *path,
                                    lonejson_error *error) {
  projection *p = context;
  if (!selected(p, path) || (p->pass == METADATA && path->segment_count == 1U &&
                             array_key(p->key, p->key_length)))
    return LONEJSON_STATUS_OK;
  return status(p, p->writer.key(&p->writer, p->key, p->key_length, error),
                error);
}

static void clear_record(projection *p) {
  lonejson_curl_parse_cleanup(&p->parser);
  lonejson_cleanup(p->pass == INDEX ? &record_map : &copy_map,
                   p->pass == INDEX ? (void *)&p->record : (void *)&p->copy);
  memset(&p->parser, 0, sizeof(p->parser));
  memset(&p->record, 0, sizeof(p->record));
  memset(&p->copy, 0, sizeof(p->copy));
  p->active = p->history_seen = p->checkpoint_seen = 0;
  p->decoded = 0U;
}
static int start_record(projection *p) {
  lonejson_error e;
  lonejson_status rc;
  size_t i;
  p->active = 1;
  if (p->pass == INDEX) {
    lonejson_json_value_init(p->json, &p->record.state.history);
    lonejson_json_value_init(p->json, &p->record.state.checkpoint);
    lonejson_json_value_init(p->json, &p->record.state.metadata.history);
    lonejson_json_value_init(p->json, &p->record.data);
    rc = lonejson_json_value_set_parse_visitor(&p->record.state.history,
                                               &p->history_visitor, p, &e);
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_json_value_set_parse_visitor(&p->record.state.checkpoint,
                                                 &p->checkpoint_visitor, p, &e);
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_json_value_set_parse_visitor(
          &p->record.state.metadata.history, &p->forbidden_visitor, p, &e);
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_json_value_set_parse_sink(&p->record.data, discard, NULL,
                                              &e);
    for (i = 0U; i < sizeof(p->record.state.metadata.strings) /
                         sizeof(p->record.state.metadata.strings[0]) &&
                 rc == LONEJSON_STATUS_OK;
         i++) {
      lonejson_json_value_init(p->json, &p->record.state.metadata.strings[i]);
      rc = lonejson_json_value_set_parse_sink(
          &p->record.state.metadata.strings[i], discard, NULL, &e);
    }
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_curl_parse_init(&p->parser, p->json, &record_map,
                                    &p->record);
  } else {
    lonejson_json_value_init(p->json, &p->copy.state);
    lonejson_json_value_init(p->json, &p->copy.data);
    rc = lonejson_json_value_set_parse_path_visitor(&p->copy.state,
                                                    &p->copy_visitor, p, &e);
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_json_value_set_parse_sink(&p->copy.data, discard, NULL, &e);
    if (rc == LONEJSON_STATUS_OK)
      rc = lonejson_curl_parse_init(&p->parser, p->json, &copy_map, &p->copy);
  }
  if (rc != LONEJSON_STATUS_OK)
    return fail(p, CAI_ERR_PROTOCOL, "initialize journal projection parser");
  return CAI_OK;
}
static int write_record(lc_sink *sink, const void *bytes, size_t count,
                        lc_error *error) {
  projection *p = sink->impl;
  const char *at = bytes;
  (void)error;
  if (p->pass == EVENTS) {
    p->rc = cai_cli_transcript_feed(p->transcript, bytes, count, p->error);
    return p->rc == CAI_OK;
  }
  if (count != 0U && !p->active && start_record(p) != CAI_OK)
    return 0;
  while (count != 0U && p->rc == CAI_OK) {
    char chunk[4096];
    size_t n = count < sizeof(chunk) ? count : sizeof(chunk);
    memcpy(chunk, at, n);
    if (lonejson_curl_write_callback(chunk, 1U, n, &p->parser) != n)
      fail(p, CAI_ERR_PROTOCOL, p->parser.error.message);
    at += n;
    count -= n;
  }
  return p->rc == CAI_OK;
}
static int boundary(void *context, const lc_collection_read_metadata *entry,
                    lc_error *error) {
  projection *p = context;
  char identity[80];
  (void)error;
  if (entry->kind != LC_COLLECTION_READ_METADATA_ENTRY)
    return LC_OK;
  if (p->pass == EVENTS) {
    p->rc = cai_cli_transcript_boundary(p->transcript, entry->position,
                                        entry->append_id, p->error);
    return p->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
  }
  if (!p->active || entry->position <= p->position)
    fail(p, CAI_ERR_PROTOCOL, "invalid journal projection boundary");
  if (p->rc == CAI_OK &&
      lonejson_curl_parse_finish(&p->parser) != LONEJSON_STATUS_OK)
    fail(p, CAI_ERR_PROTOCOL, p->parser.error.message);
  if (p->rc == CAI_OK && p->pass == INDEX) {
    if (strcmp(p->record.record_type, "commit") == 0) {
      snprintf(identity, sizeof(identity), "commit-%020llu-%020llu",
               (unsigned long long)p->record.created,
               (unsigned long long)p->record.applied);
      if (entry->append_id == NULL || strcmp(identity, entry->append_id) != 0)
        fail(p, CAI_ERR_PROTOCOL, "invalid journal commit identity");
      else
        index_commit(p, entry->position);
    } else if (strcmp(p->record.record_type, "event") != 0)
      fail(p, CAI_ERR_PROTOCOL, "unknown journal record type");
  }
  if (p->pass == HISTORY && p->remaining != 0U)
    fail(p, CAI_ERR_PROTOCOL, "retained history reference exceeds its source");
  if (p->pass == HISTORY && p->next_span < p->count &&
      p->spans[p->next_span].position == entry->position) {
    if (p->current_revision != p->spans[p->next_span].revision)
      fail(p, CAI_ERR_PROTOCOL, "retained history identity changed");
    p->next_span++;
  }
  p->position = entry->position;
  clear_record(p);
  return p->rc == CAI_OK ? LC_OK : LC_ERR_INVALID;
}
static int read_range(projection *p, pass kind, uint64_t after,
                      uint64_t through) {
  lc_collection_read_opts options;
  lc_collection_read_res result;
  lc_error detail;
  lc_sink sink;
  int rc;
  p->pass = kind;
  p->position = after;
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&result);
  lc_error_init(&detail);
  memset(&sink, 0, sizeof(sink));
  options.expected_head = p->head.etag;
  options.has_after = options.has_through = 1;
  options.after = after;
  options.through = through;
  options.on_metadata = boundary;
  options.metadata_context = p;
  sink.write = write_record;
  sink.impl = p;
  rc = p->collection->read(p->collection, &options, &sink, &result, &detail);
  if (p->rc == CAI_OK && rc != LC_OK)
    p->rc = cai_cli_lc_error(p->error, &detail);
  if (p->rc == CAI_OK && result.stop_reason == LC_COLLECTION_READ_CANCELLED)
    fail(p, CAI_ERR_CANCELLED, "journal projection cancelled");
  if (p->rc == CAI_OK && (!result.complete || p->active))
    fail(p, CAI_ERR_PROTOCOL, "incomplete journal projection range");
  if (p->rc == CAI_OK && kind == EVENTS)
    p->rc = cai_cli_transcript_finish(p->transcript, p->error);
  lc_collection_read_res_cleanup(&result);
  lc_error_cleanup(&detail);
  return p->rc;
}
static lonejson_status output(void *context, const void *bytes, size_t count,
                              lonejson_error *error) {
  projection *p = context;
  (void)error;
  if (p->rc == CAI_OK)
    p->rc = cai_sink_write(p->sink, bytes, count, p->error);
  return p->rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_IO_ERROR;
}
static int event(void *context, const cai_cli_transcript_event *e,
                 cai_error *error) {
  projection *p = context;
  hidden *ranges = p->metadata.hidden.items;
  lonejson_error local;
  size_t i;
  lonejson_status rc = LONEJSON_STATUS_OK;
  (void)error;
  if (e->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    p->visible = 1;
    for (i = 0U; i < p->metadata.hidden.count; i++)
      if (ranges[i].first >= 0 && e->sequence >= (uint64_t)ranges[i].first &&
          e->sequence <= (uint64_t)ranges[i].last)
        p->visible = 0;
    if (!p->visible)
      return CAI_OK;
#define W(call)                                                                \
  do {                                                                         \
    if (rc == LONEJSON_STATUS_OK)                                              \
      rc = (call);                                                             \
  } while (0)
#define KEY(name) W(p->writer.key(&p->writer, name, sizeof(name) - 1U, &local))
    W(p->writer.begin_object(&p->writer, &local));
    KEY("record_type");
    W(p->writer.string(&p->writer, "event", 5U, &local));
    KEY("sequence");
    W(p->writer.u64(&p->writer, e->sequence, &local));
    KEY("timestamp_ns");
    W(p->writer.u64(&p->writer, e->timestamp_ns, &local));
    KEY("utc_offset_seconds");
    W(p->writer.i64(&p->writer, e->utc_offset_seconds, &local));
    KEY("type");
    W(p->writer.string(&p->writer, e->type, strlen(e->type), &local));
    KEY("data");
    if (e->has_data)
      W(p->writer.string_begin(&p->writer, &local));
    else
      W(p->writer.null_fn(&p->writer, &local));
  } else if (p->visible && e->kind == CAI_CLI_TRANSCRIPT_DATA)
    W(p->writer.string_chunk(&p->writer, e->data, e->length, &local));
  else if (p->visible) {
    if (e->has_data)
      W(p->writer.string_end(&p->writer, &local));
    W(p->writer.end_object(&p->writer, &local));
    W(p->writer.finish(&p->writer, &local));
    if (rc == LONEJSON_STATUS_OK)
      p->rc = cai_sink_write(p->sink, "\n", 1U, p->error);
    lonejson_writer_cleanup(&p->writer);
    if (p->rc == CAI_OK && rc == LONEJSON_STATUS_OK)
      rc = lonejson_writer_init_sink(p->json, &p->writer, output, p, &local);
  }
  status(p, rc, &local);
  return p->rc;
}

static int seed(projection *p) {
  lonejson_error local;
  lonejson_status rc = LONEJSON_STATUS_OK;
  size_t i;
  block *blocks = p->metadata.blocks.items;
  hidden *ranges = p->metadata.hidden.items;
  owned *owners = p->metadata.owned.items;
  W(p->writer.begin_object(&p->writer, &local));
  KEY("record_type");
  W(p->writer.string(&p->writer, "commit", 6U, &local));
  KEY("commit_created_at_ns");
  W(p->writer.u64(&p->writer, p->created, &local));
  KEY("applied_event_sequence");
  W(p->writer.u64(&p->writer, p->applied, &local));
  KEY("state");
  W(p->writer.begin_object(&p->writer, &local));
  KEY("revision");
  W(p->writer.u64(&p->writer, 1U, &local));
  KEY("base_bytes");
  W(p->writer.u64(&p->writer, 0U, &local));
  KEY("end_bytes");
  W(p->writer.u64(&p->writer, p->bytes, &local));
  KEY("blocks_keep");
  W(p->writer.u64(&p->writer, 0U, &local));
  KEY("hidden_keep");
  W(p->writer.u64(&p->writer, 0U, &local));
  KEY("owned_keep");
  W(p->writer.u64(&p->writer, 0U, &local));
  KEY("accounting");
  W(p->writer.begin_object(&p->writer, &local));
  KEY("input_tokens");
  W(p->writer.i64(&p->writer, p->accounting.input, &local));
  KEY("cached_tokens");
  W(p->writer.i64(&p->writer, p->accounting.cached, &local));
  KEY("output_tokens");
  W(p->writer.i64(&p->writer, p->accounting.output, &local));
  KEY("reasoning_tokens");
  W(p->writer.i64(&p->writer, p->accounting.reasoning, &local));
  KEY("total_tokens");
  W(p->writer.i64(&p->writer, p->accounting.total, &local));
  KEY("usd");
  W(p->writer.f64(&p->writer, p->accounting.usd, &local));
  KEY("eur");
  W(p->writer.f64(&p->writer, p->accounting.eur, &local));
  W(p->writer.end_object(&p->writer, &local));
  /* Metadata and accounting are emitted from the final edit, not an agent's
   * freshly recomputed usage, instructions or goal clock. */
  KEY("metadata");
  if (status(p, rc, &local) != LONEJSON_STATUS_OK)
    return p->rc;
  p->copied = 0;
  if (read_range(p, METADATA, p->last_commit - 1U, p->last_commit) != CAI_OK)
    return p->rc;
  if (p->copied != 1)
    return fail(p, CAI_ERR_PROTOCOL, "missing journal metadata");
  KEY("history_blocks");
  W(p->writer.begin_array(&p->writer, &local));
  for (i = 0U; i < p->metadata.blocks.count && rc == LONEJSON_STATUS_OK; i++) {
    W(p->writer.begin_object(&p->writer, &local));
    KEY("first_item");
    W(p->writer.i64(&p->writer, blocks[i].first_item, &local));
    KEY("event_start");
    W(p->writer.i64(&p->writer, blocks[i].event_start, &local));
    KEY("input_sequence");
    W(p->writer.i64(&p->writer, blocks[i].input_sequence, &local));
    KEY("kind");
    W(p->writer.i64(&p->writer, blocks[i].kind, &local));
    KEY("summary");
    W(p->writer.string(&p->writer, blocks[i].summary, strlen(blocks[i].summary),
                       &local));
    W(p->writer.end_object(&p->writer, &local));
  }
  W(p->writer.end_array(&p->writer, &local));
  KEY("history_hidden_events");
  W(p->writer.begin_array(&p->writer, &local));
  for (i = 0U; i < p->metadata.hidden.count && rc == LONEJSON_STATUS_OK; i++) {
    W(p->writer.begin_object(&p->writer, &local));
    KEY("first");
    W(p->writer.i64(&p->writer, ranges[i].first, &local));
    KEY("last");
    W(p->writer.i64(&p->writer, ranges[i].last, &local));
    W(p->writer.end_object(&p->writer, &local));
  }
  W(p->writer.end_array(&p->writer, &local));
  KEY("history_owned_events");
  W(p->writer.begin_array(&p->writer, &local));
  for (i = 0U; i < p->metadata.owned.count && rc == LONEJSON_STATUS_OK; i++) {
    W(p->writer.begin_object(&p->writer, &local));
    KEY("entry");
    W(p->writer.i64(&p->writer, owners[i].entry, &local));
    KEY("sequence");
    W(p->writer.i64(&p->writer, owners[i].sequence, &local));
    W(p->writer.end_object(&p->writer, &local));
  }
  W(p->writer.end_array(&p->writer, &local));
  W(p->writer.end_object(&p->writer, &local));
  if (p->checkpoint != 0U) {
    KEY("input_checkpoint");
    if (status(p, rc, &local) != LONEJSON_STATUS_OK)
      return p->rc;
    p->copied = 0;
    if (read_range(p, CHECKPOINT, p->checkpoint - 1U, p->checkpoint) != CAI_OK)
      return p->rc;
    if (p->copied != 1)
      return fail(p, CAI_ERR_PROTOCOL, "missing input checkpoint");
  }
  KEY("history_append");
  W(p->writer.string_begin(&p->writer, &local));
  if (status(p, rc, &local) != LONEJSON_STATUS_OK)
    return p->rc;
  if (p->count != 0U && read_range(p, HISTORY, p->spans[0].position - 1U,
                                   p->spans[p->count - 1U].position) != CAI_OK)
    return p->rc;
  if (p->next_span != p->count || p->frame_phase != 0 || p->frame_digits != 0U)
    return fail(p, CAI_ERR_PROTOCOL, "incomplete retained history");
  for (i = 0U; i < p->metadata.blocks.count; i++)
    if (blocks[i].first_item < 0 ||
        (uint64_t)blocks[i].first_item > p->frame_count ||
        (i == 0U && blocks[i].first_item != 0) ||
        (i != 0U && blocks[i].first_item < blocks[i - 1U].first_item) ||
        blocks[i].event_start < 0 || blocks[i].input_sequence < 0 ||
        blocks[i].kind < CAI_AGENT_HISTORY_TURN ||
        blocks[i].kind > CAI_AGENT_HISTORY_COMPACTION)
      return fail(p, CAI_ERR_PROTOCOL, "invalid retained history boundary");
  W(p->writer.string_end(&p->writer, &local));
  W(p->writer.end_object(&p->writer, &local));
  W(p->writer.end_object(&p->writer, &local));
  W(p->writer.finish(&p->writer, &local));
  if (status(p, rc, &local) == LONEJSON_STATUS_OK)
    p->rc = cai_sink_write(p->sink, "\n", 1U, p->error);
  return p->rc;
}
#undef W
#undef KEY
int cai_cli_journal_project(lc_collection *collection, cai_sink *sink,
                            cai_error *error) {
  projection *p;
  lonejson_error local;
  lc_error detail;
  lonejson_config config = lonejson_default_config();
  int rc;
  if (collection == NULL || sink == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "journal collection and destination required");
  p = calloc(1U, sizeof(*p));
  if (p == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate journal projection");
  p->collection = collection;
  p->sink = sink;
  p->error = error;
  config.clear_destination_by_default = 0;
  config.reject_duplicate_keys_by_default = 1;
  config.json_value_max_total_bytes = config.json_value_max_string_bytes = 0U;
  p->json = lonejson_new(&config, &local);
  if (p->json == NULL)
    fail(p, CAI_ERR_NOMEM, "allocate journal parser");
  p->history_visitor = lonejson_default_value_visitor();
  p->history_visitor.object_begin = p->history_visitor.array_begin =
      p->history_visitor.number_begin = p->history_visitor.null_value = bad;
  p->history_visitor.boolean_value = bad_bool;
  p->history_visitor.string_begin = history_begin;
  p->history_visitor.string_chunk = history_chunk;
  p->checkpoint_visitor = lonejson_default_value_visitor();
  p->checkpoint_visitor.object_begin = checkpoint_begin;
  p->checkpoint_visitor.string_begin = p->checkpoint_visitor.array_begin =
      p->checkpoint_visitor.number_begin = p->checkpoint_visitor.null_value =
          checkpoint_scalar;
  p->checkpoint_visitor.boolean_value = checkpoint_bool;
  p->forbidden_visitor = lonejson_default_value_visitor();
  p->forbidden_visitor.object_begin = p->forbidden_visitor.array_begin =
      p->forbidden_visitor.string_begin = p->forbidden_visitor.number_begin =
          bad;
  p->forbidden_visitor.null_value = bad;
  p->forbidden_visitor.boolean_value = bad_bool;
  p->copy_visitor = lonejson_default_path_value_visitor();
  p->copy_visitor.object_begin = copy_object_begin;
  p->copy_visitor.object_end = copy_object_end;
  p->copy_visitor.object_key_begin = copy_key_begin;
  p->copy_visitor.object_key_chunk = copy_key_chunk;
  p->copy_visitor.object_key_end = copy_key_end;
  p->copy_visitor.array_begin = copy_array_begin;
  p->copy_visitor.array_end = copy_array_end;
  p->copy_visitor.string_begin = copy_string_begin;
  p->copy_visitor.string_chunk = copy_string_chunk;
  p->copy_visitor.string_end = copy_string_end;
  p->copy_visitor.number_begin = copy_number_begin;
  p->copy_visitor.number_chunk = copy_number_chunk;
  p->copy_visitor.number_end = copy_number_end;
  p->copy_visitor.boolean_value = copy_bool;
  p->copy_visitor.null_value = copy_null;
  lc_error_init(&detail);
  if (p->rc == CAI_OK &&
      collection->head(collection, &p->head, &detail) != LC_OK)
    p->rc = cai_cli_lc_error(error, &detail);
  lc_error_cleanup(&detail);
  if (p->rc == CAI_OK)
    read_range(p, INDEX, 0U, p->head.upper_position);
  if (p->rc == CAI_OK && p->last_commit == 0U)
    fail(p, CAI_ERR_PROTOCOL, "journal has no initialization commit");
  if (p->rc == CAI_OK) {
    size_t i;
    hidden *ranges = p->metadata.hidden.items;
    owned *owners = p->metadata.owned.items;
    for (i = 0U; i < p->metadata.hidden.count; i++)
      if (ranges[i].first < 1 || ranges[i].last < ranges[i].first)
        fail(p, CAI_ERR_PROTOCOL, "invalid trimmed journal range");
    for (i = 0U; i < p->metadata.owned.count; i++)
      if (owners[i].entry < 0 ||
          (uint64_t)owners[i].entry >= p->metadata.blocks.count ||
          owners[i].sequence <= 0)
        fail(p, CAI_ERR_PROTOCOL, "invalid retained event ownership");
  }
  if (p->rc == CAI_OK)
    status(p, lonejson_writer_init_sink(p->json, &p->writer, output, p, &local),
           &local);
  if (p->rc == CAI_OK)
    p->rc = cai_cli_transcript_open(event, p, &p->transcript, error);
  if (p->rc == CAI_OK)
    read_range(p, EVENTS, 0U, p->head.upper_position);
  if (p->rc == CAI_OK)
    seed(p);
  rc = p->rc;
  cai_cli_transcript_close(p->transcript);
  clear_record(p);
  lonejson_curl_parse_cleanup(&p->frame_parser);
  lonejson_cleanup(&copy_map, &p->frame);
  lonejson_writer_cleanup(&p->writer);
  /* Folded indexes use our realloc, unlike parser-owned array tails. */
  free(p->metadata.blocks.items);
  free(p->metadata.hidden.items);
  free(p->metadata.owned.items);
  lonejson_free(p->json);
  free(p->spans);
  free(p);
  return rc;
}
