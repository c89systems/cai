#include "cai_internal.h"
#include <cai/handover.h>

#include <stdint.h>
#include <string.h>

#define HANDOVER_MAX_DEPTH 128U
#define HANDOVER_KEY_BYTES 64U
#define HANDOVER_FIELD_BYTES 160U

typedef enum handover_container_kind {
  HANDOVER_OBJECT = 1,
  HANDOVER_ARRAY = 2
} handover_container_kind;

typedef struct handover_container {
  int kind;
  int root_array;
  int content_object;
  int summary_object;
  int output_part_object;
  char owner_key[HANDOVER_KEY_BYTES];
  char key[HANDOVER_KEY_BYTES];
  size_t key_length;
  int key_truncated;
  char content_type[HANDOVER_FIELD_BYTES];
  size_t content_type_length;
  int content_type_truncated;
  int content_written;
} handover_container;

typedef struct handover_exporter {
  cai_sink *sink;
  cai_error *error;
  int rc;
  const cai_handover_config *config;
  handover_container stack[HANDOVER_MAX_DEPTH];
  size_t depth;
  size_t item_depth;
  int item_active;
  char item_type[HANDOVER_FIELD_BYTES];
  size_t item_type_length;
  int item_type_truncated;
  char item_role[HANDOVER_FIELD_BYTES];
  size_t item_role_length;
  int item_role_truncated;
  char item_name[HANDOVER_FIELD_BYTES];
  size_t item_name_length;
  int item_name_truncated;
  char *capture;
  size_t *capture_length;
  int *capture_truncated;
  int stream_kind;
  int item_heading_written;
  int item_activity_written;
  int item_content_written;
  int code_at_line_start;
  size_t record_item_index;
  size_t target_item_index;
  int target_item_found;
  int metadata_only;
  int preserve_item_metadata;
} handover_exporter;

static int handover_write(handover_exporter *exporter, const void *bytes,
                          size_t count) {
  if (exporter->rc != CAI_OK || count == 0U) {
    return exporter->rc;
  }
  if (exporter->config->cancelled != NULL &&
      exporter->config->cancelled(exporter->config->cancel_context)) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_CANCELLED,
                                 "handover export cancelled");
    return exporter->rc;
  }
  if (exporter->metadata_only) {
    return CAI_OK;
  }
  while (count != 0U && exporter->rc == CAI_OK) {
    size_t take = count > 4096U ? 4096U : count;
    exporter->rc = cai_sink_write(exporter->sink, bytes, take, exporter->error);
    bytes = (const char *)bytes + take;
    count -= take;
  }
  return exporter->rc;
}

static int handover_literal(handover_exporter *exporter, const char *text) {
  return handover_write(exporter, text, strlen(text));
}

static int handover_write_text(handover_exporter *exporter, const char *data,
                               size_t length) {
  size_t start;
  size_t i;

  start = 0U;
  for (i = 0U; i < length; i++) {
    unsigned char ch;

    ch = (unsigned char)data[i];
    if ((ch < 0x20U && ch != '\n' && ch != '\t') || ch == 0x7fU) {
      if (i > start) {
        (void)handover_write(exporter, data + start, i - start);
      }
      start = i + 1U;
    }
  }
  if (start < length) {
    (void)handover_write(exporter, data + start, length - start);
  }
  return exporter->rc;
}

static int handover_write_code(handover_exporter *exporter, const char *data,
                               size_t length) {
  size_t start;
  size_t i;

  start = 0U;
  for (i = 0U; i < length && exporter->rc == CAI_OK; i++) {
    unsigned char ch;

    ch = (unsigned char)data[i];
    if (exporter->code_at_line_start) {
      (void)handover_literal(exporter, "    ");
      exporter->code_at_line_start = 0;
    }
    if ((ch < 0x20U && ch != '\n' && ch != '\t') || ch == 0x7fU) {
      if (i > start) {
        (void)handover_write(exporter, data + start, i - start);
      }
      start = i + 1U;
    }
    if (ch == '\n') {
      if (i + 1U > start) {
        (void)handover_write(exporter, data + start, i + 1U - start);
      }
      start = i + 1U;
      exporter->code_at_line_start = 1;
    }
  }
  if (start < length && exporter->rc == CAI_OK) {
    (void)handover_write(exporter, data + start, length - start);
  }
  return exporter->rc;
}

static int handover_write_code_block(handover_exporter *exporter,
                                     const char *data) {
  if (data == NULL || data[0] == '\0') {
    return handover_literal(exporter, "    *[not available]*\n\n");
  }
  exporter->code_at_line_start = 1;
  (void)handover_write_code(exporter, data, strlen(data));
  if (!exporter->code_at_line_start) {
    (void)handover_literal(exporter, "\n");
  }
  (void)handover_literal(exporter, "\n");
  exporter->code_at_line_start = 1;
  return exporter->rc;
}

static int handover_write_metadata(handover_exporter *exporter,
                                   const char *label, const char *value) {
  (void)handover_literal(exporter, "- ");
  (void)handover_literal(exporter, label);
  (void)handover_literal(exporter, ":\n");
  return handover_write_code_block(exporter, value);
}

static void handover_capture_append(char *destination, size_t *length,
                                    int *truncated, const char *data,
                                    size_t count) {
  size_t available;
  size_t copy_count;

  if (*truncated || *length >= HANDOVER_FIELD_BYTES - 1U) {
    *truncated = 1;
    return;
  }
  available = HANDOVER_FIELD_BYTES - 1U - *length;
  copy_count = count < available ? count : available;
  if (copy_count > 0U) {
    memcpy(destination + *length, data, copy_count);
    *length += copy_count;
    destination[*length] = '\0';
  }
  if (copy_count != count) {
    *truncated = 1;
  }
}

static handover_container *handover_top(handover_exporter *exporter) {
  return exporter->depth > 0U ? &exporter->stack[exporter->depth - 1U] : NULL;
}

static int handover_key_is(const handover_container *value, const char *key) {
  return value != NULL && !value->key_truncated && strcmp(value->key, key) == 0;
}

static int handover_string_is(const char *value, size_t length, int truncated,
                              const char *expected) {
  return !truncated && strlen(expected) == length &&
         memcmp(value, expected, length) == 0;
}

static int handover_item_is(const handover_exporter *exporter,
                            const char *type) {
  return handover_string_is(exporter->item_type, exporter->item_type_length,
                            exporter->item_type_truncated, type);
}

static int handover_role_is(const handover_exporter *exporter,
                            const char *role) {
  return handover_string_is(exporter->item_role, exporter->item_role_length,
                            exporter->item_role_truncated, role);
}

static int handover_begin_message(handover_exporter *exporter) {
  const char *heading;

  if (exporter->item_heading_written) {
    return CAI_OK;
  }
  if (handover_role_is(exporter, "user")) {
    heading = "## User\n\n";
  } else if (handover_role_is(exporter, "assistant")) {
    heading = "## Assistant\n\n";
  } else if (handover_role_is(exporter, "developer")) {
    heading = "## Developer\n\n";
  } else if (handover_role_is(exporter, "system")) {
    heading = "## System\n\n";
  } else {
    heading = "## Message\n\n";
  }
  exporter->item_heading_written = 1;
  return handover_literal(exporter, heading);
}

static int handover_begin_reasoning(handover_exporter *exporter) {
  if (!exporter->item_heading_written) {
    exporter->item_heading_written = 1;
    (void)handover_literal(exporter, "## Reasoning\n\n");
  }
  return exporter->rc;
}

static int handover_begin_activity(handover_exporter *exporter, int output) {
  if (!exporter->item_activity_written) {
    (void)handover_literal(exporter, "## Activity\n\n");
    exporter->item_activity_written = 1;
  }
  if (!exporter->item_heading_written) {
    if (output) {
      (void)handover_literal(exporter, "### Tool output\n\n");
    } else {
      (void)handover_literal(exporter, "### Tool call");
      if (exporter->item_name_length > 0U) {
        (void)handover_literal(exporter, ": ");
        (void)handover_write_text(exporter, exporter->item_name,
                                  exporter->item_name_length);
      }
      (void)handover_literal(exporter, "\n\n");
    }
    exporter->item_heading_written = 1;
    exporter->code_at_line_start = 1;
  }
  return exporter->rc;
}

static lonejson_status handover_object_begin(void *user,
                                             lonejson_error *error) {
  handover_exporter *exporter;
  handover_container *parent;
  handover_container *value;

  (void)error;
  exporter = (handover_exporter *)user;
  if (exporter->depth >= HANDOVER_MAX_DEPTH) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_LIMIT,
                                 "session export nesting is too deep");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  parent = handover_top(exporter);
  value = &exporter->stack[exporter->depth++];
  memset(value, 0, sizeof(*value));
  value->kind = HANDOVER_OBJECT;
  if (parent != NULL && parent->kind == HANDOVER_ARRAY && parent->root_array) {
    if (exporter->record_item_index++ == exporter->target_item_index) {
      exporter->item_active = 1;
      exporter->target_item_found = 1;
      exporter->item_depth = exporter->depth;
      if (!exporter->preserve_item_metadata) {
        exporter->item_type_length = 0U;
        exporter->item_type_truncated = 0;
        exporter->item_type[0] = '\0';
        exporter->item_role_length = 0U;
        exporter->item_role_truncated = 0;
        exporter->item_role[0] = '\0';
        exporter->item_name_length = 0U;
        exporter->item_name_truncated = 0;
        exporter->item_name[0] = '\0';
      }
      exporter->item_heading_written = 0;
      exporter->item_activity_written = 0;
      exporter->item_content_written = 0;
      exporter->code_at_line_start = 0;
    }
  } else if (parent != NULL && parent->kind == HANDOVER_ARRAY &&
             strcmp(parent->owner_key, "content") == 0) {
    value->content_object = 1;
  } else if (parent != NULL && parent->kind == HANDOVER_ARRAY &&
             strcmp(parent->owner_key, "summary") == 0) {
    value->summary_object = 1;
  } else if (parent != NULL && parent->kind == HANDOVER_ARRAY &&
             strcmp(parent->owner_key, "output") == 0) {
    value->output_part_object = 1;
  } else if (parent != NULL && parent->kind == HANDOVER_OBJECT &&
             exporter->item_active &&
             exporter->item_depth == exporter->depth - 1U &&
             handover_key_is(parent, "output")) {
    (void)handover_begin_activity(exporter, 1);
    (void)handover_literal(exporter, "*[non-text attachment omitted]*\n\n");
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status handover_object_end(void *user, lonejson_error *error) {
  handover_exporter *exporter;
  handover_container *value;

  (void)error;
  exporter = (handover_exporter *)user;
  value = handover_top(exporter);
  if (value == NULL || value->kind != HANDOVER_OBJECT) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_PROTOCOL,
                                 "invalid session export structure");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (exporter->item_active && value->content_object &&
      !value->content_written && value->content_type_length > 0U &&
      !handover_string_is(value->content_type, value->content_type_length,
                          value->content_type_truncated, "input_text") &&
      !handover_string_is(value->content_type, value->content_type_length,
                          value->content_type_truncated, "output_text") &&
      !handover_string_is(value->content_type, value->content_type_length,
                          value->content_type_truncated, "refusal")) {
    (void)handover_begin_message(exporter);
    (void)handover_literal(exporter, "*[non-text attachment omitted]*\n\n");
  } else if (exporter->item_active && value->output_part_object &&
             !value->content_written) {
    (void)handover_begin_activity(exporter, 1);
    (void)handover_literal(exporter, "*[non-text attachment omitted]*\n\n");
  }
  if (exporter->item_active && exporter->item_depth == exporter->depth) {
    if (!exporter->item_heading_written &&
        handover_item_is(exporter, "function_call")) {
      (void)handover_begin_activity(exporter, 0);
      (void)handover_literal(exporter, "\n");
    }
    exporter->item_active = 0;
  }
  exporter->depth--;
  return exporter->rc == CAI_OK ? LONEJSON_STATUS_OK
                                : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_status handover_array_begin(void *user, lonejson_error *error) {
  handover_exporter *exporter;
  handover_container *parent;
  handover_container *value;

  (void)error;
  exporter = (handover_exporter *)user;
  if (exporter->depth >= HANDOVER_MAX_DEPTH) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_LIMIT,
                                 "session export nesting is too deep");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  parent = handover_top(exporter);
  value = &exporter->stack[exporter->depth++];
  memset(value, 0, sizeof(*value));
  value->kind = HANDOVER_ARRAY;
  if (parent != NULL && parent->root_array) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_PROTOCOL,
                                 "handover history requires object items");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  value->root_array = parent == NULL;
  if (parent != NULL && parent->kind == HANDOVER_OBJECT) {
    memcpy(value->owner_key, parent->key, sizeof(value->owner_key));
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status handover_array_end(void *user, lonejson_error *error) {
  handover_exporter *exporter;
  handover_container *value;

  (void)error;
  exporter = (handover_exporter *)user;
  value = handover_top(exporter);
  if (value == NULL || value->kind != HANDOVER_ARRAY) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_PROTOCOL,
                                 "invalid session export structure");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  exporter->depth--;
  return LONEJSON_STATUS_OK;
}

static lonejson_status handover_key_begin(void *user, lonejson_error *error) {
  handover_container *value;

  (void)error;
  value = handover_top((handover_exporter *)user);
  if (value != NULL && value->kind == HANDOVER_OBJECT) {
    value->key_length = 0U;
    value->key_truncated = 0;
    value->key[0] = '\0';
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status handover_key_chunk(void *user, const char *data,
                                          size_t length,
                                          lonejson_error *error) {
  handover_container *value;

  (void)error;
  value = handover_top((handover_exporter *)user);
  if (value != NULL && value->kind == HANDOVER_OBJECT &&
      !value->key_truncated) {
    size_t available;
    size_t copy_count;

    available = sizeof(value->key) - 1U - value->key_length;
    copy_count = length < available ? length : available;
    if (copy_count > 0U) {
      memcpy(value->key + value->key_length, data, copy_count);
      value->key_length += copy_count;
      value->key[value->key_length] = '\0';
    }
    if (copy_count != length) {
      value->key_truncated = 1;
    }
  }
  return LONEJSON_STATUS_OK;
}

static lonejson_status handover_string_begin(void *user,
                                             lonejson_error *error) {
  handover_exporter *exporter;
  handover_container *value;

  (void)error;
  exporter = (handover_exporter *)user;
  value = handover_top(exporter);
  exporter->capture = NULL;
  exporter->capture_length = NULL;
  exporter->capture_truncated = NULL;
  exporter->stream_kind = 0;
  if (value != NULL && value->root_array) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_PROTOCOL,
                                 "handover history requires object items");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  if (value == NULL || value->kind != HANDOVER_OBJECT ||
      !exporter->item_active) {
    return LONEJSON_STATUS_OK;
  }
  if (exporter->item_depth == exporter->depth) {
    if (handover_key_is(value, "type")) {
      if (!exporter->preserve_item_metadata) {
        exporter->capture = exporter->item_type;
        exporter->capture_length = &exporter->item_type_length;
        exporter->capture_truncated = &exporter->item_type_truncated;
      }
    } else if (handover_key_is(value, "role")) {
      if (!exporter->preserve_item_metadata) {
        exporter->capture = exporter->item_role;
        exporter->capture_length = &exporter->item_role_length;
        exporter->capture_truncated = &exporter->item_role_truncated;
      }
    } else if (handover_key_is(value, "name")) {
      if (!exporter->preserve_item_metadata) {
        exporter->capture = exporter->item_name;
        exporter->capture_length = &exporter->item_name_length;
        exporter->capture_truncated = &exporter->item_name_truncated;
      }
    } else if (handover_key_is(value, "arguments") ||
               handover_key_is(value, "input")) {
      (void)handover_begin_activity(exporter, 0);
      exporter->stream_kind = 2;
    } else if (handover_key_is(value, "output")) {
      (void)handover_begin_activity(exporter, 1);
      exporter->stream_kind = 2;
    } else if (handover_key_is(value, "summary")) {
      (void)handover_begin_reasoning(exporter);
      exporter->stream_kind = 3;
    }
  } else if (value->content_object) {
    if (handover_key_is(value, "type")) {
      exporter->capture = value->content_type;
      exporter->capture_length = &value->content_type_length;
      exporter->capture_truncated = &value->content_type_truncated;
    } else if (handover_key_is(value, "text") ||
               handover_key_is(value, "refusal")) {
      (void)handover_begin_message(exporter);
      value->content_written = 1;
      exporter->item_content_written = 1;
      exporter->stream_kind = 1;
    }
  } else if (value->output_part_object) {
    if (handover_key_is(value, "type")) {
      exporter->capture = value->content_type;
      exporter->capture_length = &value->content_type_length;
      exporter->capture_truncated = &value->content_type_truncated;
    } else if (handover_key_is(value, "text") ||
               handover_key_is(value, "refusal")) {
      (void)handover_begin_activity(exporter, 1);
      value->content_written = 1;
      exporter->stream_kind = 2;
    }
  } else if (value->summary_object && handover_key_is(value, "text")) {
    (void)handover_begin_reasoning(exporter);
    exporter->stream_kind = 3;
  }
  return exporter->rc == CAI_OK ? LONEJSON_STATUS_OK
                                : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_status handover_string_chunk(void *user, const char *data,
                                             size_t length,
                                             lonejson_error *error) {
  handover_exporter *exporter;

  (void)error;
  exporter = (handover_exporter *)user;
  if (exporter->capture != NULL) {
    handover_capture_append(exporter->capture, exporter->capture_length,
                            exporter->capture_truncated, data, length);
  } else if (exporter->stream_kind == 1 || exporter->stream_kind == 3) {
    (void)handover_write_text(exporter, data, length);
  } else if (exporter->stream_kind == 2) {
    (void)handover_write_code(exporter, data, length);
  }
  return exporter->rc == CAI_OK ? LONEJSON_STATUS_OK
                                : LONEJSON_STATUS_CALLBACK_FAILED;
}

static lonejson_status handover_string_end(void *user, lonejson_error *error) {
  handover_exporter *exporter;

  (void)error;
  exporter = (handover_exporter *)user;
  if (exporter->stream_kind == 1 || exporter->stream_kind == 3) {
    (void)handover_literal(exporter, "\n\n");
  } else if (exporter->stream_kind == 2) {
    if (!exporter->code_at_line_start) {
      (void)handover_literal(exporter, "\n");
    }
    (void)handover_literal(exporter, "\n");
    /* The terminating blank line also begins the next code block. */
    exporter->code_at_line_start = 1;
  }
  exporter->capture = NULL;
  exporter->capture_length = NULL;
  exporter->capture_truncated = NULL;
  exporter->stream_kind = 0;
  return exporter->rc == CAI_OK ? LONEJSON_STATUS_OK
                                : LONEJSON_STATUS_CALLBACK_FAILED;
}

static void handover_parse_reset(handover_exporter *exporter) {
  memset(exporter->stack, 0, sizeof(exporter->stack));
  exporter->depth = 0U;
  exporter->item_depth = 0U;
  exporter->item_active = 0;
  exporter->record_item_index = 0U;
  exporter->target_item_found = 0;
  if (!exporter->preserve_item_metadata) {
    exporter->item_type_length = 0U;
    exporter->item_type_truncated = 0;
    exporter->item_type[0] = '\0';
    exporter->item_role_length = 0U;
    exporter->item_role_truncated = 0;
    exporter->item_role[0] = '\0';
    exporter->item_name_length = 0U;
    exporter->item_name_truncated = 0;
    exporter->item_name[0] = '\0';
  }
  exporter->capture = NULL;
  exporter->capture_length = NULL;
  exporter->capture_truncated = NULL;
  exporter->stream_kind = 0;
  exporter->item_heading_written = 0;
  exporter->item_activity_written = 0;
  exporter->item_content_written = 0;
  exporter->code_at_line_start = 0;
}

/** The index retains only the three bounded identities needed before a text
 * field, even when the provider places those identities after that field. */
typedef struct handover_item {
  char name[HANDOVER_FIELD_BYTES];
  size_t name_length;
  int name_truncated, function_call;
  unsigned int role;
  uint64_t frame_length;
} handover_item;

typedef struct handover_frame {
  lonejson_json_value item;
} handover_frame;
static const lonejson_field frame_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(handover_frame, item, "item")};
LONEJSON_MAP_DEFINE(frame_map, handover_frame, frame_fields);

/** One synchronous operation owns parser scratch and its item index. The
 * producer supplies bounded borrowed chunks, without a pull/push spool or a
 * nested storage read. Parsing is finished and released at each frame seam. */
typedef struct handover_workspace {
  handover_exporter exporter;
  lonejson *json;
  lonejson_curl_parse parser;
  lonejson_value_visitor visitor;
  handover_frame frame;
  handover_item *items;
  size_t count, capacity, at;
  uint64_t frame_length, remaining;
  unsigned int digits;
  int phase;
} handover_workspace;

static int handover_fail(handover_workspace *work, int code,
                         const char *message) {
  if (work->exporter.rc == CAI_OK)
    work->exporter.rc = cai_set_error(work->exporter.error, code, message);
  return work->exporter.rc;
}

static int handover_parser_bytes(handover_workspace *work, const char *data,
                                 size_t length) {
  char chunk[4096];
  while (length != 0U && work->exporter.rc == CAI_OK) {
    size_t take = length < sizeof(chunk) ? length : sizeof(chunk);
    memcpy(chunk, data, take);
    if (lonejson_curl_write_callback(chunk, 1U, take, &work->parser) != take)
      return handover_fail(work,
                           work->parser.error.code ==
                                   LONEJSON_STATUS_ALLOCATION_FAILED
                               ? CAI_ERR_NOMEM
                               : CAI_ERR_PROTOCOL,
                           work->parser.error.message);
    data += take;
    length -= take;
  }
  return work->exporter.rc;
}

static void handover_frame_cleanup(handover_workspace *work) {
  lonejson_curl_parse_cleanup(&work->parser);
  lonejson_cleanup(&frame_map, &work->frame);
  memset(&work->parser, 0, sizeof(work->parser));
  memset(&work->frame, 0, sizeof(work->frame));
}

static void handover_item_transfer(handover_item *item,
                                   handover_exporter *exporter, int restore) {
  static const char *const roles[] = {"", "user", "assistant", "developer",
                                      "system"};
  unsigned int i;
  /* Type and role affect only these fixed predicates. Keep their categories,
   * rather than three 160-byte strings per historical item. Unknown/truncated
   * values retain the generic-message behavior of the original formatter. */
  if (restore) {
    strcpy(exporter->item_type, item->function_call ? "function_call" : "");
    exporter->item_type_length = strlen(exporter->item_type);
    exporter->item_type_truncated = 0;
    strcpy(exporter->item_role, roles[item->role]);
    exporter->item_role_length = strlen(exporter->item_role);
    exporter->item_role_truncated = 0;
    memcpy(exporter->item_name, item->name, sizeof(item->name));
    exporter->item_name_length = item->name_length;
    exporter->item_name_truncated = item->name_truncated;
  } else {
    item->function_call = handover_item_is(exporter, "function_call");
    item->role = 0U;
    for (i = 1U; i < sizeof(roles) / sizeof(roles[0]); i++)
      if (handover_role_is(exporter, roles[i]))
        item->role = i;
    memcpy(item->name, exporter->item_name, sizeof(item->name));
    item->name_length = exporter->item_name_length;
    item->name_truncated = exporter->item_name_truncated;
  }
}

static int handover_frame_begin(handover_workspace *work) {
  lonejson_error error;
  lonejson_status status;
  handover_exporter *exporter = &work->exporter;
  if (!exporter->metadata_only) {
    if (work->at >= work->count ||
        work->frame_length != work->items[work->at].frame_length)
      return handover_fail(work, CAI_ERR_PROTOCOL,
                           "handover history changed between passes");
    handover_item_transfer(&work->items[work->at], exporter, 1);
  }
  handover_parse_reset(exporter);
  lonejson_error_init(&error);
  lonejson_json_value_init(work->json, &work->frame.item);
  status = lonejson_json_value_set_parse_visitor(
      &work->frame.item, &work->visitor, exporter, &error);
  if (status == LONEJSON_STATUS_OK)
    status = lonejson_curl_parse_init(&work->parser, work->json, &frame_map,
                                      &work->frame);
  if (status != LONEJSON_STATUS_OK)
    return handover_fail(work, CAI_ERR_NOMEM,
                         "initialize handover history parser");
  work->remaining = work->frame_length;
  work->phase = 1;
  return handover_parser_bytes(work, "{\"item\":", 8U);
}

static int handover_frame_finish(handover_workspace *work) {
  handover_exporter *exporter = &work->exporter;
  if (handover_parser_bytes(work, "}", 1U) != CAI_OK)
    return exporter->rc;
  if (lonejson_curl_parse_finish(&work->parser) != LONEJSON_STATUS_OK)
    return handover_fail(work, CAI_ERR_PROTOCOL, work->parser.error.message);
  if (exporter->depth != 0U || exporter->item_active ||
      !exporter->target_item_found || exporter->record_item_index != 1U)
    return handover_fail(work, CAI_ERR_PROTOCOL,
                         "handover history requires one object per array");
  if (exporter->metadata_only) {
    if (work->count == work->capacity) {
      size_t capacity = work->capacity != 0U ? work->capacity * 2U : 16U;
      handover_item *items;
      if (capacity < work->capacity || capacity > SIZE_MAX / sizeof(*items))
        return handover_fail(work, CAI_ERR_LIMIT,
                             "handover item index is too large");
      items = cai_realloc_mem(NULL, work->items, capacity * sizeof(*items));
      if (items == NULL)
        return handover_fail(work, CAI_ERR_NOMEM,
                             "allocate handover item index");
      work->items = items;
      work->capacity = capacity;
    }
    handover_item_transfer(&work->items[work->count], exporter, 0);
    work->items[work->count++].frame_length = work->frame_length;
  }
  work->at++;
  handover_frame_cleanup(work);
  work->phase = 2;
  return CAI_OK;
}

static int handover_history_chunk(void *context, const void *bytes,
                                  size_t length, cai_error *error) {
  handover_workspace *work = context;
  const char *data = bytes;
  (void)error;
  while (length != 0U && work->exporter.rc == CAI_OK) {
    if (work->exporter.config->cancelled != NULL &&
        work->exporter.config->cancelled(work->exporter.config->cancel_context))
      return handover_fail(work, CAI_ERR_CANCELLED,
                           "handover export cancelled");
    if (work->phase == 0) {
      unsigned char ch = (unsigned char)*data++;
      length--;
      if (work->digits == 0U && (ch == '\n' || ch == '\r'))
        continue;
      if (ch == '\n' && work->digits != 0U && work->frame_length != 0U) {
        if (handover_frame_begin(work) != CAI_OK)
          return work->exporter.rc;
      } else if (ch < '0' || ch > '9' || work->digits++ >= 20U ||
                 work->frame_length > (UINT64_MAX - (ch - '0')) / 10U)
        return handover_fail(work, CAI_ERR_PROTOCOL,
                             "invalid handover history length");
      else
        work->frame_length = work->frame_length * 10U + ch - '0';
    } else if (work->phase == 1) {
      size_t take = length > 4096U ? 4096U : length;
      if (take > work->remaining)
        take = (size_t)work->remaining;
      if (handover_parser_bytes(work, data, take) != CAI_OK)
        return work->exporter.rc;
      data += take;
      length -= take;
      work->remaining -= take;
      if (work->remaining == 0U && handover_frame_finish(work) != CAI_OK)
        return work->exporter.rc;
    } else {
      if (*data++ != '\n')
        return handover_fail(work, CAI_ERR_PROTOCOL,
                             "invalid handover history delimiter");
      length--;
      work->phase = 0;
      work->digits = 0U;
      work->frame_length = 0U;
    }
  }
  return work->exporter.rc;
}

static int handover_history(handover_workspace *work,
                            const cai_handover_source *source) {
  cai_sink sink;
  int pass, rc;
  if (source->write == NULL)
    return CAI_OK;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = handover_history_chunk;
  sink.callbacks.context = work;
  for (pass = 0; pass < 2; pass++) {
    if (work->exporter.config->cancelled != NULL &&
        work->exporter.config->cancelled(work->exporter.config->cancel_context))
      return handover_fail(work, CAI_ERR_CANCELLED,
                           "handover export cancelled");
    work->at = 0U;
    work->exporter.metadata_only = pass == 0;
    work->exporter.preserve_item_metadata = pass != 0;
    rc = source->write(source->context, &sink, work->exporter.error);
    if (work->exporter.rc != CAI_OK)
      return work->exporter.rc;
    if (rc != CAI_OK)
      return rc;
    if (work->exporter.config->cancelled != NULL &&
        work->exporter.config->cancelled(work->exporter.config->cancel_context))
      return handover_fail(work, CAI_ERR_CANCELLED,
                           "handover export cancelled");
    if (work->phase != 0 || work->digits != 0U ||
        (pass != 0 && work->at != work->count))
      return handover_fail(work, CAI_ERR_PROTOCOL,
                           "incomplete or changed handover history");
  }
  return CAI_OK;
}

typedef struct handover_text {
  handover_exporter *exporter;
  int present;
} handover_text;
static int handover_text_chunk(void *context, const void *bytes, size_t length,
                               cai_error *error) {
  handover_text *text = context;
  const char *data = bytes;
  (void)error;
  if (length != 0U)
    text->present = 1;
  while (length != 0U && text->exporter->rc == CAI_OK) {
    size_t take = length > 4096U ? 4096U : length;
    (void)handover_write_text(text->exporter, data, take);
    data += take;
    length -= take;
  }
  return text->exporter->rc;
}
static int handover_source_text(handover_exporter *exporter,
                                const cai_handover_source *source,
                                int instruction) {
  handover_text text;
  cai_sink sink;
  int rc = CAI_OK;
  if (exporter->rc != CAI_OK)
    return exporter->rc;
  memset(&sink, 0, sizeof(sink));
  text.exporter = exporter;
  text.present = 0;
  sink.callbacks.write = handover_text_chunk;
  sink.callbacks.context = &text;
  if (source->write != NULL)
    rc = source->write(source->context, &sink, exporter->error);
  if (exporter->rc == CAI_OK && rc != CAI_OK)
    exporter->rc = rc;
  if (exporter->rc == CAI_OK && instruction && !text.present)
    return handover_literal(exporter,
                            "*[no developer instructions configured]*");
  return exporter->rc;
}

static int handover_document(const cai_handover_config *config,
                             handover_workspace *work) {
  handover_exporter *exporter = &work->exporter;
  char number[64];
  (void)handover_literal(exporter, "# CAI agent handover\n\n");
  (void)handover_literal(exporter,
                         "> **Format:** `cai-agent-handover/1`\n>\n> ");
  (void)handover_literal(
      exporter,
      "This is a non-resumable handover document, not a session commit. "
      "It preserves durable session context and active developer "
      "instructions available to this runtime.\n\n");
  (void)handover_literal(exporter, "## Runtime\n\n");
  (void)handover_write_metadata(exporter, "Preset", config->preset_name);
  (void)handover_write_metadata(exporter, "Preset prompt version",
                                config->preset_prompt_version);
  (void)handover_write_metadata(exporter, "Active model", config->active_model);
  if (config->recorded_model != NULL && config->active_model != NULL &&
      strcmp(config->recorded_model, config->active_model) != 0) {
    (void)handover_write_metadata(exporter, "Recorded history model",
                                  config->recorded_model);
  }
  (void)handover_write_metadata(exporter, "Reasoning effort",
                                config->reasoning_effort);
  (void)handover_write_metadata(exporter, "Runtime state",
                                config->runtime_state);
  (void)handover_write_metadata(exporter, "Session ID", config->session_id);
  (void)handover_write_metadata(exporter, "Workspace",
                                config->workspace_directory);
  (void)handover_write_metadata(exporter, "Session scope",
                                config->session_scope);
  (void)handover_literal(exporter, "- Terminal tools: ");
  (void)handover_literal(exporter, config->terminal_enabled ? "enabled\n\n"
                                                            : "disabled\n\n");
  (void)handover_literal(exporter, "- Image generation: ");
  (void)handover_literal(exporter, config->image_generation_enabled
                                       ? "enabled\n\n"
                                       : "disabled\n\n");
  (void)snprintf(number, sizeof(number), "%lu configured",
                 (unsigned long)config->mcp_client_count);
  (void)handover_write_metadata(exporter, "MCP clients", number);

  (void)handover_literal(exporter, "## Active goal\n\n");
  if (config->goal_objective.write == NULL) {
    (void)handover_literal(exporter, "No active durable goal.\n\n");
  } else {
    (void)handover_write_metadata(exporter, "Status", config->goal_status);
    if (config->goal_has_token_budget) {
      (void)snprintf(number, sizeof(number), "%lld", config->goal_token_budget);
      (void)handover_write_metadata(exporter, "Token budget", number);
    } else {
      (void)handover_write_metadata(exporter, "Token budget", "unbounded");
    }
    (void)snprintf(number, sizeof(number), "%lld", config->goal_tokens_used);
    (void)handover_write_metadata(exporter, "Tokens used", number);
    (void)snprintf(number, sizeof(number), "%lld",
                   config->goal_elapsed_seconds);
    (void)handover_write_metadata(exporter, "Elapsed active seconds", number);
    if (config->goal_has_token_budget) {
      (void)snprintf(number, sizeof(number), "%lld",
                     config->goal_tokens_used >= config->goal_token_budget
                         ? 0LL
                         : config->goal_token_budget -
                               config->goal_tokens_used);
      (void)handover_write_metadata(exporter, "Tokens remaining", number);
    }
    (void)handover_literal(exporter, "### Objective\n\n");
    (void)handover_source_text(exporter, &config->goal_objective, 0);
    (void)handover_literal(exporter, "\n\n");
  }

  (void)handover_literal(exporter, "## Active developer instructions\n\n");
  (void)handover_source_text(exporter, &config->developer_instructions, 1);
  (void)handover_literal(exporter, "\n\n");

  (void)handover_literal(exporter, "## Handover limits\n\n");
  (void)handover_literal(
      exporter,
      "- This Markdown does not resume or authenticate a CAI session; the "
      "durable session commit remains authoritative for continuation.\n"
      "- No live terminal process, PTY state, in-flight model/tool call, or "
      "queued host input is included.\n"
      "- Non-text attachments are represented as omission markers; raw binary "
      "attachment payloads are never embedded.\n"
      "- Text already durable in the session or configured as developer "
      "instructions is intentionally preserved and is not automatically "
      "redacted.\n\n");
  return exporter->rc == CAI_OK ? handover_history(work, &config->history)
                                : exporter->rc;
}

void cai_handover_config_init(cai_handover_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}

static lonejson_status handover_primitive(void *context,
                                          lonejson_error *error) {
  handover_exporter *exporter = context;
  handover_container *value = handover_top(exporter);
  (void)error;
  if (value != NULL && value->root_array) {
    exporter->rc = cai_set_error(exporter->error, CAI_ERR_PROTOCOL,
                                 "handover history requires object items");
    return LONEJSON_STATUS_CALLBACK_FAILED;
  }
  return LONEJSON_STATUS_OK;
}
static lonejson_status handover_boolean(void *context, int value,
                                        lonejson_error *error) {
  (void)value;
  return handover_primitive(context, error);
}

int cai_handover_write(const cai_handover_config *config, cai_sink *sink,
                       cai_error *error) {
  handover_workspace *work;
  lonejson_config json_config = lonejson_default_config();
  lonejson_error local;
  int rc;
  if (config == NULL || sink == NULL || sink->callbacks.write == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "handover configuration and sink are required");
  if (config->goal_objective.write != NULL &&
      (config->goal_tokens_used < 0 || config->goal_elapsed_seconds < 0 ||
       (config->goal_has_token_budget && config->goal_token_budget < 0)))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "invalid handover goal accounting");
  work = cai_alloc(NULL, sizeof(*work));
  if (work == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "allocate handover export workspace");
  memset(work, 0, sizeof(*work));
  work->exporter.sink = sink;
  work->exporter.error = error;
  work->exporter.config = config;
  json_config.clear_destination_by_default = 0;
  json_config.json_value_max_total_bytes = 0U;
  json_config.json_value_max_string_bytes = 0U;
  lonejson_error_init(&local);
  work->json = lonejson_new(&json_config, &local);
  if (work->json == NULL)
    rc =
        cai_set_error(error, CAI_ERR_NOMEM, "allocate handover history parser");
  else {
    work->visitor = lonejson_default_value_visitor();
    work->visitor.object_begin = handover_object_begin;
    work->visitor.object_end = handover_object_end;
    work->visitor.object_key_begin = handover_key_begin;
    work->visitor.object_key_chunk = handover_key_chunk;
    work->visitor.array_begin = handover_array_begin;
    work->visitor.array_end = handover_array_end;
    work->visitor.string_begin = handover_string_begin;
    work->visitor.string_chunk = handover_string_chunk;
    work->visitor.string_end = handover_string_end;
    work->visitor.null_value = work->visitor.number_begin = handover_primitive;
    work->visitor.boolean_value = handover_boolean;
    rc = handover_document(config, work);
    handover_frame_cleanup(work);
    lonejson_free(work->json);
  }
  cai_free_mem(NULL, work->items);
  cai_free_mem(NULL, work);
  return rc;
}
