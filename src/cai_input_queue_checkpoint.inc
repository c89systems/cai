/* A cut-local recovery baseline, not an ordinary journal record or a copy of
 * model context. Both directions retain only one serialized item at a time. */
typedef struct input_checkpoint_item {
  cai_input_entry entry;
  lonejson_int64 intent, status;
  lonejson_json_value events_source;
  lonejson_mapped_array_stream events_parse;
} input_checkpoint_item;
#define INPUT_CHECKPOINT_ITEM_FIELDS                                           \
  LONEJSON_FIELD_U64_REQ(input_checkpoint_item, entry.id, "id"),               \
      LONEJSON_FIELD_U64_REQ(input_checkpoint_item, entry.revision,            \
                             "revision"),                                      \
      LONEJSON_FIELD_U64_REQ(input_checkpoint_item, entry.client_id,           \
                             "client"),                                        \
      LONEJSON_FIELD_U64_REQ(input_checkpoint_item, entry.request_id,          \
                             "request"),                                       \
      LONEJSON_FIELD_U64(input_checkpoint_item, entry.edit_owner,              \
                         "edit_owner"),                                        \
      LONEJSON_FIELD_U64(input_checkpoint_item, entry.edit_token,              \
                         "edit_token"),                                        \
      LONEJSON_FIELD_I64_REQ(input_checkpoint_item, intent, "intent"),         \
      LONEJSON_FIELD_I64_REQ(input_checkpoint_item, status, "status"),         \
      LONEJSON_FIELD_BOOL(input_checkpoint_item, entry.held, "held"),          \
      LONEJSON_FIELD_BOOL(input_checkpoint_item, entry.stopped, "stopped"),    \
      LONEJSON_FIELD_BOOL(input_checkpoint_item, entry.edit_held,              \
                          "edit_held"),                                        \
      LONEJSON_FIELD_BOOL(input_checkpoint_item, entry.action, "action"),      \
      LONEJSON_FIELD_BOOL(input_checkpoint_item, entry.independent,            \
                          "independent"),                                      \
      LONEJSON_FIELD_STRING_FIXED(input_checkpoint_item, entry.origin,         \
                                  "origin", LONEJSON_OVERFLOW_FAIL),           \
      LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(input_checkpoint_item, entry.text, \
                                            "text")
static const lonejson_field input_checkpoint_item_fields[] = {
    INPUT_CHECKPOINT_ITEM_FIELDS,
    LONEJSON_FIELD_JSON_VALUE_REQ(input_checkpoint_item, events_source,
                                  "input_events")};
LONEJSON_MAP_DEFINE(input_checkpoint_item_map, input_checkpoint_item,
                    input_checkpoint_item_fields);

static const lonejson_field input_checkpoint_item_parse_fields[] = {
    INPUT_CHECKPOINT_ITEM_FIELDS,
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(input_checkpoint_item, events_parse,
                                           "input_events")};
LONEJSON_MAP_DEFINE(input_checkpoint_item_parse_map, input_checkpoint_item,
                    input_checkpoint_item_parse_fields);
#undef INPUT_CHECKPOINT_ITEM_FIELDS

typedef struct input_checkpoint_position {
  lonejson_uint64 sequence;
} input_checkpoint_position;
static const lonejson_field input_checkpoint_position_fields[] = {
    LONEJSON_FIELD_U64_REQ(input_checkpoint_position, sequence, "sequence")};
LONEJSON_MAP_DEFINE(input_checkpoint_position_map, input_checkpoint_position,
                    input_checkpoint_position_fields);
typedef struct input_checkpoint_positions {
  const unsigned long long *events;
  size_t count, index, begin, end;
  int phase;
  char bytes[64];
} input_checkpoint_positions;
static lonejson_read_result
input_checkpoint_positions_read(void *context, unsigned char *bytes,
                                size_t capacity) {
  input_checkpoint_positions *positions = context;
  lonejson_read_result result = lonejson_default_read_result();
  while (result.bytes_read < capacity) {
    size_t n;
    if (positions->begin < positions->end) {
      n = positions->end - positions->begin;
      if (n > capacity - result.bytes_read)
        n = capacity - result.bytes_read;
      memcpy(bytes + result.bytes_read, positions->bytes + positions->begin, n);
      positions->begin += n;
      result.bytes_read += n;
    } else if (positions->phase == 0) {
      bytes[result.bytes_read++] = '[';
      positions->phase = 1;
    } else if (positions->phase == 2) {
      result.eof = 1;
      break;
    } else if (positions->index == positions->count) {
      bytes[result.bytes_read++] = ']';
      positions->phase = 2;
    } else {
      int length =
          snprintf(positions->bytes, sizeof(positions->bytes),
                   "%s{\"sequence\":%llu}", positions->index == 0U ? "" : ",",
                   positions->events[positions->index]);
      if (length < 0 || (size_t)length >= sizeof(positions->bytes)) {
        result.error_code = LONEJSON_STATUS_OVERFLOW;
        break;
      }
      positions->begin = 0U;
      positions->end = (size_t)length;
      ++positions->index;
    }
  }
  return result;
}

typedef struct input_checkpoint_header {
  lonejson_uint64 revision, serial, client_serial, token, through,
      pending_count, receipt_count;
  int stopped, auto_approval;
} input_checkpoint_header;
#define INPUT_CHECKPOINT_HEADER_FIELDS(type)                                   \
  LONEJSON_FIELD_U64_REQ(type, header.revision, "revision"),                   \
      LONEJSON_FIELD_U64_REQ(type, header.serial, "serial"),                   \
      LONEJSON_FIELD_U64_REQ(type, header.client_serial, "client_serial"),     \
      LONEJSON_FIELD_U64_REQ(type, header.token, "token"),                     \
      LONEJSON_FIELD_U64_REQ(type, header.through, "through"),                 \
      LONEJSON_FIELD_U64_REQ(type, header.pending_count, "pending_count"),     \
      LONEJSON_FIELD_U64_REQ(type, header.receipt_count, "receipt_count"),     \
      LONEJSON_FIELD_BOOL_REQ(type, header.stopped, "stopped"),                \
      LONEJSON_FIELD_BOOL_REQ(type, header.auto_approval, "auto_approval")
typedef struct input_checkpoint_document {
  input_checkpoint_header header;
  lonejson_json_value pending, receipts;
} input_checkpoint_document;
static const lonejson_field input_checkpoint_fields[] = {
    INPUT_CHECKPOINT_HEADER_FIELDS(input_checkpoint_document),
    LONEJSON_FIELD_JSON_VALUE_REQ(input_checkpoint_document, pending,
                                  "pending"),
    LONEJSON_FIELD_JSON_VALUE_REQ(input_checkpoint_document, receipts,
                                  "receipts")};
LONEJSON_MAP_DEFINE(input_checkpoint_map, input_checkpoint_document,
                    input_checkpoint_fields);

typedef struct input_checkpoint_array {
  input_node *node;
  input_checkpoint_item item;
  input_checkpoint_positions positions;
  lonejson_generator generator;
  int phase, active, comma;
} input_checkpoint_array;
static lonejson_read_result input_checkpoint_array_read(void *context,
                                                        unsigned char *bytes,
                                                        size_t capacity) {
  input_checkpoint_array *array = context;
  lonejson_read_result result = lonejson_default_read_result();
  while (result.bytes_read < capacity) {
    size_t n = 0U;
    int eof = 0;
    if (array->phase == 0) {
      bytes[result.bytes_read++] = '[';
      array->phase = 1;
      continue;
    }
    if (array->phase == 2) {
      result.eof = 1;
      break;
    }
    if (array->node == NULL) {
      bytes[result.bytes_read++] = ']';
      array->phase = 2;
      continue;
    }
    if (!array->active) {
      if (array->comma) {
        bytes[result.bytes_read++] = ',';
        array->comma = 0;
        continue;
      }
      memset(&array->item, 0, sizeof(array->item));
      array->item.entry = array->node->value;
      array->item.entry.text = array->node->text;
      array->item.intent = array->node->value.intent;
      array->item.status = array->node->origin_status;
      memset(&array->positions, 0, sizeof(array->positions));
      array->positions.events = array->node->input_events;
      array->positions.count = array->node->input_event_count;
      CAI_LJ->json_value_init(CAI_LJ, &array->item.events_source);
      {
        lonejson_error local;
        lonejson_error_init(&local);
        if (array->item.events_source.methods->set_reader(
                &array->item.events_source, input_checkpoint_positions_read,
                &array->positions, &local) != LONEJSON_STATUS_OK) {
          result.error_code = LONEJSON_STATUS_ALLOCATION_FAILED;
          break;
        }
      }
      if (lonejson_generator_init(CAI_LJ, &array->generator,
                                  &input_checkpoint_item_map,
                                  &array->item) != LONEJSON_STATUS_OK) {
        result.error_code = LONEJSON_STATUS_ALLOCATION_FAILED;
        break;
      }
      array->active = 1;
    }
    if (lonejson_generator_read(&array->generator, bytes + result.bytes_read,
                                capacity - result.bytes_read, &n,
                                &eof) != LONEJSON_STATUS_OK) {
      result.error_code = LONEJSON_STATUS_CALLBACK_FAILED;
      break;
    }
    result.bytes_read += n;
    if (eof) {
      lonejson_generator_cleanup(&array->generator);
      array->active = 0;
      CAI_LJ->json_value_cleanup(CAI_LJ, &array->item.events_source);
      array->node = array->node->next;
      array->comma = 1;
    } else if (n == 0U) {
      result.error_code = LONEJSON_STATUS_CALLBACK_FAILED;
      break;
    }
  }
  return result;
}
typedef struct input_checkpoint_source {
  cai_input_queue *queue;
  input_checkpoint_document document;
  input_checkpoint_array arrays[2];
  lonejson_generator generator;
} input_checkpoint_source;
static size_t input_checkpoint_read(void *context, void *bytes, size_t capacity,
                                    cai_error *error) {
  input_checkpoint_source *source = context;
  size_t n = 0U;
  int eof = 0;
  if (source->queue->closed ||
      source->queue->revision != source->document.header.revision) {
    cai_set_error(error, CAI_ERR_INVALID,
                  "input queue changed during its atomic cut");
    return 0U;
  }
  if (lonejson_generator_read(&source->generator, bytes, capacity, &n, &eof) !=
      LONEJSON_STATUS_OK)
    cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                         "read pending input checkpoint",
                         source->generator.error.message);
  return n;
}
static void input_checkpoint_source_close(void *context) {
  input_checkpoint_source *source = context;
  size_t i;
  lonejson_generator_cleanup(&source->generator);
  for (i = 0U; i < 2U; ++i) {
    if (source->arrays[i].active)
      lonejson_generator_cleanup(&source->arrays[i].generator);
    CAI_LJ->json_value_cleanup(CAI_LJ, &source->arrays[i].item.events_source);
  }
  CAI_LJ->json_value_cleanup(CAI_LJ, &source->document.pending);
  CAI_LJ->json_value_cleanup(CAI_LJ, &source->document.receipts);
  queue_release(source->queue);
  cai_free_mem(NULL, source);
}
int cai_input_checkpoint_source(cai_input_queue *queue,
                                unsigned long long through, cai_source **out,
                                cai_error *error) {
  input_checkpoint_source *source;
  input_node *node;
  cai_source_callbacks callbacks;
  lonejson_error local;
  int rc = queue_valid(queue, error);
  if (rc != CAI_OK || out == NULL)
    return rc != CAI_OK ? rc
                        : cai_set_error(error, CAI_ERR_INVALID,
                                        "checkpoint output is required");
  *out = NULL;
  if (queue->uncertain || queue->references == (size_t)-1)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input queue requires recovery before trimming");
  source = cai_alloc(NULL, sizeof(*source));
  if (source == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate input checkpoint source");
  memset(source, 0, sizeof(*source));
  source->queue = queue;
  ++queue->references;
  source->document.header.revision = queue->revision;
  source->document.header.serial = queue->serial;
  source->document.header.client_serial = queue->client_serial;
  source->document.header.token = queue->token;
  source->document.header.through = through;
  source->document.header.pending_count = queue->count;
  source->document.header.stopped = queue->stopped;
  source->document.header.auto_approval = queue->auto_approval;
  for (node = queue->receipts; node != NULL; node = node->next)
    ++source->document.header.receipt_count;
  source->arrays[0].node = queue->head;
  source->arrays[1].node = queue->receipts;
  lonejson_error_init(&local);
  CAI_LJ->json_value_init(CAI_LJ, &source->document.pending);
  CAI_LJ->json_value_init(CAI_LJ, &source->document.receipts);
  if (source->document.pending.methods->set_reader(
          &source->document.pending, input_checkpoint_array_read,
          &source->arrays[0], &local) != LONEJSON_STATUS_OK ||
      source->document.receipts.methods->set_reader(
          &source->document.receipts, input_checkpoint_array_read,
          &source->arrays[1], &local) != LONEJSON_STATUS_OK ||
      lonejson_generator_init(CAI_LJ, &source->generator, &input_checkpoint_map,
                              &source->document) != LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "prepare streamed input checkpoint");
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = source;
  callbacks.read = input_checkpoint_read;
  callbacks.close = input_checkpoint_source_close;
  if (rc == CAI_OK)
    rc = cai_source_from_callbacks(&callbacks, out, error);
  if (rc != CAI_OK)
    input_checkpoint_source_close(source);
  return rc;
}

typedef struct input_checkpoint_parse_document {
  input_checkpoint_header header;
  lonejson_mapped_array_stream pending, receipts;
} input_checkpoint_parse_document;
static const lonejson_field input_checkpoint_parse_fields[] = {
    INPUT_CHECKPOINT_HEADER_FIELDS(input_checkpoint_parse_document),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(input_checkpoint_parse_document,
                                           pending, "pending"),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(input_checkpoint_parse_document,
                                           receipts, "receipts")};
LONEJSON_MAP_DEFINE(input_checkpoint_parse_map, input_checkpoint_parse_document,
                    input_checkpoint_parse_fields);
typedef struct input_checkpoint_parse_lane {
  struct cai_input_checkpoint_restore *restore;
  int receipt;
  input_node provenance;
  input_checkpoint_position position;
} input_checkpoint_parse_lane;
struct cai_input_checkpoint_restore {
  cai_input_queue *queue;
  lonejson *json;
  lonejson_curl_parse parser;
  input_checkpoint_parse_document document;
  input_checkpoint_item items[2];
  input_checkpoint_parse_lane lanes[2];
  size_t receipts;
};
static lonejson_status input_checkpoint_position_parsed(void *context,
                                                        void *value,
                                                        lonejson_error *error) {
  input_checkpoint_parse_lane *lane = context;
  input_checkpoint_position *position = value;
  input_node *node = &lane->provenance;
  (void)error;
  if (position->sequence == 0U ||
      (node->input_event_count != 0U &&
       node->input_events[node->input_event_count - 1U] >= position->sequence))
    return LONEJSON_STATUS_INVALID_JSON;
  if (reserve_input_event(node, NULL) != CAI_OK)
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  node->input_events[node->input_event_count++] = position->sequence;
  return LONEJSON_STATUS_OK;
}
static lonejson_status input_checkpoint_item_parsed(void *context, void *value,
                                                    lonejson_error *error) {
  input_checkpoint_parse_lane *lane = context;
  cai_input_checkpoint_restore *restore = lane->restore;
  input_checkpoint_item *item = value;
  cai_input_queue *queue = restore->queue;
  input_node *node, *at;
  size_t n;
  (void)error;
  if (item->entry.id == 0U || item->entry.revision == 0U ||
      item->entry.client_id == 0U || item->entry.request_id == 0U ||
      item->intent < CAI_INPUT_QUEUED || item->intent > CAI_INPUT_STEERING ||
      (lane->receipt ? item->status != CAI_INPUT_ORIGIN_CLAIMED &&
                           item->status != CAI_INPUT_ORIGIN_WITHDRAWN
                     : item->status != CAI_INPUT_ORIGIN_PENDING) ||
      (lane->receipt ? item->entry.text != NULL || item->entry.origin[0] == '\0'
                     : !text_valid(queue, item->entry.text)) ||
      (lane->receipt && lane->provenance.input_event_count != 0U) ||
      (lane->receipt ? restore->receipts : queue->count) >=
          queue->config.max_entries ||
      find_node(queue, item->entry.id, NULL, NULL) != NULL ||
      find_origin(queue, item->entry.origin) != NULL)
    return LONEJSON_STATUS_INVALID_JSON;
  for (at = queue->receipts; at != NULL; at = at->next)
    if (at->value.id == item->entry.id)
      return LONEJSON_STATUS_INVALID_JSON;
  node = cai_alloc(NULL, sizeof(*node));
  if (node == NULL)
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  memset(node, 0, sizeof(*node));
  node->value = item->entry;
  node->value.intent = (cai_input_intent)item->intent;
  node->origin_status = (cai_input_origin_status)item->status;
  if (item->entry.text != NULL) {
    n = strlen(item->entry.text);
    node->text = cai_alloc(NULL, n + 1U);
    if (node->text == NULL) {
      node_free(node);
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    }
    memcpy(node->text, item->entry.text, n + 1U);
  }
  node->value.text = node->text;
  node->input_events = lane->provenance.input_events;
  node->input_event_count = lane->provenance.input_event_count;
  node->input_event_capacity = lane->provenance.input_event_capacity;
  memset(&lane->provenance, 0, sizeof(lane->provenance));
  if (lane->receipt) {
    node->next = queue->receipts;
    queue->receipts = node;
    ++restore->receipts;
  } else {
    if (queue->tail != NULL)
      queue->tail->next = node;
    else
      queue->head = node;
    queue->tail = node;
    ++queue->count;
  }
  if (node->value.origin[0] != '\0')
    ++queue->origin_count;
  return LONEJSON_STATUS_OK;
}
void cai_input_checkpoint_close(cai_input_checkpoint_restore *restore) {
  size_t i;
  if (restore == NULL)
    return;
  for (i = 0U; i < 2U; ++i)
    cai_free_mem(NULL, restore->lanes[i].provenance.input_events);
  lonejson_curl_parse_cleanup(&restore->parser);
  if (restore->json != NULL) {
    restore->json->cleanup(restore->json, &input_checkpoint_parse_map,
                           &restore->document);
    lonejson_free(restore->json);
  }
  cai_input_queue_close(restore->queue);
  cai_free_mem(NULL, restore);
}
int cai_input_checkpoint_begin(const cai_input_queue_config *config,
                               cai_input_checkpoint_restore **out,
                               cai_error *error) {
  cai_input_checkpoint_restore *restore;
  lonejson_config json_config = lonejson_default_config();
  lonejson_mapped_array_stream_handler handler;
  lonejson_error local;
  size_t i;
  int rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input checkpoint parser output is required");
  *out = NULL;
  restore = cai_alloc(NULL, sizeof(*restore));
  if (restore == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate input checkpoint parser");
  memset(restore, 0, sizeof(*restore));
  rc = cai_input_queue_open(config, &restore->queue, error);
  json_config.clear_destination_by_default = 0;
  json_config.max_dynamic_string_bytes =
      restore->queue != NULL ? restore->queue->config.max_text_bytes : 66048U;
  lonejson_error_init(&local);
  if (rc == CAI_OK &&
      (restore->json = lonejson_new(&json_config, &local)) == NULL)
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "create input checkpoint JSON parser");
  for (i = 0U; rc == CAI_OK && i < 2U; ++i) {
    memset(&handler, 0, sizeof(handler));
    restore->lanes[i].restore = restore;
    restore->lanes[i].receipt = i != 0U;
    handler.item_map = &input_checkpoint_item_parse_map;
    handler.item_dst = &restore->items[i];
    handler.item = input_checkpoint_item_parsed;
    handler.user = &restore->lanes[i];
    {
      lonejson_mapped_array_stream_handler positions;
      memset(&positions, 0, sizeof(positions));
      positions.item_map = &input_checkpoint_position_map;
      positions.item_dst = &restore->lanes[i].position;
      positions.item = input_checkpoint_position_parsed;
      positions.user = &restore->lanes[i];
      if (lonejson_mapped_array_stream_set_handler(
              &restore->items[i].events_parse, &positions, &local) !=
          LONEJSON_STATUS_OK)
        rc = cai_set_error(error, CAI_ERR_NOMEM,
                           "configure input provenance checkpoint");
    }
    if (lonejson_mapped_array_stream_set_handler(
            i == 0U ? &restore->document.pending : &restore->document.receipts,
            &handler, &local) != LONEJSON_STATUS_OK)
      rc = cai_set_error(error, CAI_ERR_NOMEM,
                         "configure input checkpoint arrays");
  }
  if (rc == CAI_OK &&
      lonejson_curl_parse_init(&restore->parser, restore->json,
                               &input_checkpoint_parse_map,
                               &restore->document) != LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "initialize input checkpoint parser");
  if (rc != CAI_OK)
    cai_input_checkpoint_close(restore);
  else
    *out = restore;
  return rc;
}
int cai_input_checkpoint_feed(cai_input_checkpoint_restore *restore,
                              const void *bytes, size_t count,
                              cai_error *error) {
  char *borrowed;
  if (restore == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input checkpoint parser is required");
  memcpy(&borrowed, &bytes, sizeof(borrowed));
  if (restore->parser.write_callback(&restore->parser, borrowed, 1U, count) !=
      count)
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "invalid input checkpoint",
                                restore->parser.error.message);
  return CAI_OK;
}
int cai_input_checkpoint_finish(cai_input_checkpoint_restore *restore,
                                cai_input_queue **out,
                                unsigned long long *through, cai_error *error) {
  input_checkpoint_header *header;
  input_node *node;
  size_t lane;
  if (restore == NULL || out == NULL || through == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "input checkpoint outputs are required");
  *out = NULL;
  if (restore->parser.finish(&restore->parser) != LONEJSON_STATUS_OK)
    return cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "incomplete input checkpoint",
                                restore->parser.error.message);
  header = &restore->document.header;
  if (header->pending_count != restore->queue->count ||
      header->receipt_count != restore->receipts ||
      restore->queue->origin_count > restore->queue->config.max_entries)
    return cai_set_error(error, CAI_ERR_PROTOCOL,
                         "input checkpoint count mismatch");
  for (lane = 0U; lane < 2U; ++lane)
    for (node = lane == 0U ? restore->queue->head : restore->queue->receipts;
         node != NULL; node = node->next) {
      size_t i;
      for (i = 0U; i < node->input_event_count; ++i)
        if (node->input_events[i] > header->through)
          return cai_set_error(error, CAI_ERR_PROTOCOL,
                               "input provenance exceeds checkpoint boundary");
      if (node->value.id > header->serial ||
          node->value.revision > header->revision ||
          node->value.client_id > header->client_serial ||
          node->value.edit_owner > header->client_serial ||
          node->value.edit_token > header->token ||
          (node->value.edit_held
               ? node->value.edit_owner == 0U || node->value.edit_token == 0U
               : node->value.edit_owner != 0U || node->value.edit_token != 0U))
        return cai_set_error(error, CAI_ERR_PROTOCOL,
                             "invalid input checkpoint identity");
    }
  restore->queue->revision = header->revision;
  restore->queue->serial = header->serial;
  restore->queue->client_serial = header->client_serial;
  restore->queue->token = header->token;
  restore->queue->stopped = header->stopped;
  restore->queue->auto_approval = header->auto_approval;
  *through = header->through;
  *out = restore->queue;
  restore->queue = NULL;
  return CAI_OK;
}
#undef INPUT_CHECKPOINT_HEADER_FIELDS
