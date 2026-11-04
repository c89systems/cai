#include "input_wire.h"
#include "pouch_stream.h"
#include <stdlib.h>
#include <string.h>

#define FIELD_LIMIT 27U
typedef struct field {
  uint64_t number;
  const char *text;
  size_t length;
} field;
struct cai_cli_input_wire_decoder {
  cai_cli_input_wire_message message;
  field fields[FIELD_LIMIT];
  const char *format;
  size_t index, offset, length;
  char *string;
  int phase;
  char text[CAI_CLI_INPUT_TEXT_MAX + 1U], session[129], origin[128],
      reason[192];
};
static const char *format(cai_protocol_kind kind) {
  switch (kind) {
  case CAI_PROTOCOL_SYNCHRONIZE:
    return "us";
  case CAI_PROTOCOL_REQUEST:
    return "uuuuuubut";
  case CAI_PROTOCOL_QUEUE_BEGIN:
    return "uuuubbbs";
  case CAI_PROTOCOL_QUEUE_ITEM:
    return "uuuuuuuubbbbbbst";
  case CAI_PROTOCOL_QUEUE_END:
    return "uu";
  case CAI_PROTOCOL_QUEUE_CHANGE:
    return "uuuuuuuuubbbuuuuuuubbbbbbst";
  case CAI_PROTOCOL_RESULT:
    return "uuuuuuuus";
  default:
    return NULL;
  }
}
static int invalid(cai_error *error) {
  return cai_cli_error(error, CAI_ERR_PROTOCOL,
                       "invalid workspace input payload");
}
static size_t string_capacity(cai_protocol_kind kind, size_t index) {
  if (kind == CAI_PROTOCOL_SYNCHRONIZE || kind == CAI_PROTOCOL_QUEUE_BEGIN)
    return 129U;
  if (kind == CAI_PROTOCOL_RESULT)
    return 192U;
  if ((kind == CAI_PROTOCOL_QUEUE_ITEM && index == 14U) ||
      (kind == CAI_PROTOCOL_QUEUE_CHANGE && index == 25U))
    return 128U;
  return CAI_CLI_INPUT_TEXT_MAX + 1U;
}
static void entry_pack(field *f, const cai_input_entry *e) {
  f[0].number = e->id;
  f[1].number = e->revision;
  f[2].number = e->client_id;
  f[3].number = e->request_id;
  f[4].number = e->edit_owner;
  f[5].number = e->edit_token;
  f[6].number = e->intent;
  f[7].number = e->held;
  f[8].number = e->edit_held;
  f[9].number = e->ready;
  f[10].number = e->stopped;
  f[11].number = e->action;
  f[12].number = e->independent;
  f[13].text = e->origin;
  f[14].text = e->text;
}
static int entry_unpack(const field *f, cai_input_entry *e, cai_error *error) {
  if (f[6].number > CAI_INPUT_STEERING || f[13].length >= sizeof(e->origin))
    return invalid(error);
  e->id = f[0].number;
  e->revision = f[1].number;
  e->client_id = f[2].number;
  e->request_id = f[3].number;
  e->edit_owner = f[4].number;
  e->edit_token = f[5].number;
  e->intent = (cai_input_intent)f[6].number;
  e->held = (int)f[7].number;
  e->edit_held = (int)f[8].number;
  e->ready = (int)f[9].number;
  e->stopped = (int)f[10].number;
  e->action = (int)f[11].number;
  e->independent = (int)f[12].number;
  if (f[13].length != 0U)
    memcpy(e->origin, f[13].text, f[13].length);
  e->origin[f[13].length] = '\0';
  e->text = f[14].text;
  return CAI_OK;
}
static int pack(const cai_cli_input_wire_message *m, field *f,
                cai_error *error) {
  const cai_input_request *r = &m->request;
  const cai_input_observation *o = &m->observation;
  const cai_input_state *s = &o->state;
  switch (m->header.kind) {
  case CAI_PROTOCOL_SYNCHRONIZE:
    f[0].number = m->producer;
    f[1].text = m->session;
    break;
  case CAI_PROTOCOL_REQUEST:
    if (r->operation < CAI_INPUT_OFFER || r->operation > CAI_INPUT_RESOLVE ||
        r->intent < CAI_INPUT_QUEUED || r->intent > CAI_INPUT_STEERING ||
        r->origin[0] != '\0')
      return invalid(error);
    f[0].number = r->operation;
    f[1].number = r->item_id;
    f[2].number = r->item_revision;
    f[3].number = r->queue_revision;
    f[4].number = r->edit_token;
    f[5].number = r->intent;
    f[6].number = r->enabled;
    f[7].number = r->target_request_id;
    f[8].text = r->text;
    break;
  case CAI_PROTOCOL_QUEUE_BEGIN:
    f[0].number = o->producer;
    f[1].number = o->last_request;
    f[2].number = s->revision;
    f[3].number = s->count;
    f[4].number = s->stopped;
    f[5].number = s->auto_approval;
    f[6].number = s->uncertain;
    f[7].text = m->session;
    break;
  case CAI_PROTOCOL_QUEUE_ITEM:
    f[0].number = o->index;
    entry_pack(f + 1U, &o->entry);
    break;
  case CAI_PROTOCOL_QUEUE_END:
    f[0].number = s->count;
    f[1].number = s->revision;
    break;
  case CAI_PROTOCOL_QUEUE_CHANGE:
    f[0].number = o->change.type;
    f[1].number = o->change.index;
    f[2].number = o->change.previous_view_revision;
    f[3].number = o->change.client_id;
    f[4].number = o->change.request_id;
    f[5].number = o->producer;
    f[6].number = o->last_request;
    f[7].number = s->revision;
    f[8].number = s->count;
    f[9].number = s->stopped;
    f[10].number = s->auto_approval;
    f[11].number = s->uncertain;
    entry_pack(f + 12U, &o->change.entry);
    break;
  case CAI_PROTOCOL_RESULT:
    f[0].number = m->category;
    f[1].number = o->producer;
    f[2].number = o->last_request;
    f[3].number = o->result.outcome;
    f[4].number = o->result.code;
    f[5].number = o->result.item_id;
    f[6].number = o->result.item_revision;
    f[7].number = o->result.edit_token;
    f[8].text = o->result.reason;
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
static int unpack(cai_cli_input_wire_decoder *d, cai_error *error) {
  cai_cli_input_wire_message *m = &d->message;
  cai_input_observation *o = &m->observation;
  cai_input_request *r = &m->request;
  field *f = d->fields;
  o->conversation = m->header.conversation;
  o->state.view_revision = m->header.sequence;
  switch (m->header.kind) {
  case CAI_PROTOCOL_SYNCHRONIZE:
    m->producer = f[0].number;
    m->session = f[1].text;
    break;
  case CAI_PROTOCOL_REQUEST:
    if (f[0].number > CAI_INPUT_RESOLVE || f[5].number > CAI_INPUT_STEERING)
      return invalid(error);
    r->request_id = m->header.request;
    r->operation = (cai_input_operation)f[0].number;
    r->item_id = f[1].number;
    r->item_revision = f[2].number;
    r->queue_revision = f[3].number;
    r->edit_token = f[4].number;
    r->intent = (cai_input_intent)f[5].number;
    r->enabled = (int)f[6].number;
    r->target_request_id = f[7].number;
    r->text = f[8].text;
    break;
  case CAI_PROTOCOL_QUEUE_BEGIN:
    if (f[0].number == 0U || f[3].number > 128U || f[7].length == 0U)
      return invalid(error);
    o->type = CAI_INPUT_OBSERVATION_SNAPSHOT_BEGIN;
    o->producer = f[0].number;
    o->last_request = f[1].number;
    o->state.revision = f[2].number;
    o->state.count = (size_t)f[3].number;
    o->state.stopped = (int)f[4].number;
    o->state.auto_approval = (int)f[5].number;
    o->state.uncertain = (int)f[6].number;
    m->session = f[7].text;
    break;
  case CAI_PROTOCOL_QUEUE_ITEM:
    if (f[0].number >= 128U || f[1].number == 0U || f[15].text == NULL)
      return invalid(error);
    o->type = CAI_INPUT_OBSERVATION_SNAPSHOT_ITEM;
    o->index = (size_t)f[0].number;
    return entry_unpack(f + 1U, &o->entry, error);
  case CAI_PROTOCOL_QUEUE_END:
    if (f[0].number > 128U)
      return invalid(error);
    o->type = CAI_INPUT_OBSERVATION_SNAPSHOT_END;
    o->index = o->state.count = (size_t)f[0].number;
    o->state.revision = f[1].number;
    break;
  case CAI_PROTOCOL_QUEUE_CHANGE:
    if (f[0].number > CAI_INPUT_STATE || f[1].number >= 128U ||
        f[8].number > 128U)
      return invalid(error);
    o->type = CAI_INPUT_OBSERVATION_CHANGE;
    o->change.type = (cai_input_change_type)f[0].number;
    o->change.index = (size_t)f[1].number;
    o->change.previous_view_revision = f[2].number;
    o->change.client_id = f[3].number;
    o->change.request_id = f[4].number;
    o->producer = f[5].number;
    o->last_request = f[6].number;
    o->state.revision = f[7].number;
    o->state.count = (size_t)f[8].number;
    o->state.stopped = (int)f[9].number;
    o->state.auto_approval = (int)f[10].number;
    o->state.uncertain = (int)f[11].number;
    o->change.state = o->state;
    return entry_unpack(f + 12U, &o->change.entry, error);
  case CAI_PROTOCOL_RESULT:
    if (f[0].number > CAI_CLI_INPUT_RESET || f[3].number > CAI_INPUT_UNKNOWN ||
        f[4].number > CAI_ERR_LIMIT)
      return invalid(error);
    m->category = (cai_cli_input_result_kind)f[0].number;
    o->type = m->category == CAI_CLI_INPUT_RECEIPT
                  ? CAI_INPUT_OBSERVATION_RESULT
                  : CAI_INPUT_OBSERVATION_DISCONNECTED;
    o->producer = f[1].number;
    o->last_request = f[2].number;
    o->result.request_id = m->header.request;
    o->result.outcome = (cai_input_outcome)f[3].number;
    o->result.code = (int)f[4].number;
    o->result.item_id = f[5].number;
    o->result.item_revision = f[6].number;
    o->result.edit_token = f[7].number;
    memcpy(o->result.reason, f[8].text, f[8].length + 1U);
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
static char *string_target(cai_cli_input_wire_decoder *d, size_t *capacity) {
  cai_protocol_kind kind = d->message.header.kind;
  *capacity = string_capacity(kind, d->index);
  if (kind == CAI_PROTOCOL_SYNCHRONIZE || kind == CAI_PROTOCOL_QUEUE_BEGIN) {
    return d->session;
  }
  if (kind == CAI_PROTOCOL_RESULT) {
    return d->reason;
  }
  if ((kind == CAI_PROTOCOL_QUEUE_ITEM && d->index == 14U) ||
      (kind == CAI_PROTOCOL_QUEUE_CHANGE && d->index == 25U)) {
    return d->origin;
  }
  return d->text;
}
int cai_cli_input_wire_decoder_open(cai_cli_input_wire_decoder **out,
                                    cai_error *error) {
  if (out == NULL)
    return invalid(error);
  *out = calloc(1U, sizeof(**out));
  return *out != NULL ? CAI_OK
                      : cai_cli_error(error, CAI_ERR_NOMEM,
                                      "allocate input wire parser");
}
int cai_cli_input_wire_decode(cai_cli_input_wire_decoder *d,
                              const cai_protocol_event *event,
                              const cai_cli_input_wire_message **out,
                              cai_error *error) {
  const cai_cbor_token *t;
  size_t count;
  char expected;
  if (out != NULL)
    *out = NULL;
  if (d == NULL || event == NULL || out == NULL || event->header == NULL)
    return invalid(error);
  if (event->type == CAI_PROTOCOL_HEADER) {
    const char *f = format(event->header->kind);
    if (f == NULL || event->header->stream != 0U ||
        (event->header->kind == CAI_PROTOCOL_RESULT &&
         event->header->sequence != 0U) ||
        ((event->header->kind == CAI_PROTOCOL_REQUEST ||
          event->header->kind == CAI_PROTOCOL_SYNCHRONIZE) &&
         (event->header->request == 0U || event->header->sequence != 0U)))
      return invalid(error);
    memset(&d->message, 0, sizeof(d->message));
    memset(d->fields, 0, sizeof(d->fields));
    d->message.header = *event->header;
    d->format = f;
    d->index = d->offset = d->length = 0U;
    d->phase = 0;
    return CAI_OK;
  }
  if (d->format == NULL)
    return invalid(error);
  count = strlen(d->format);
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    int rc;
    if (d->phase != 4)
      return invalid(error);
    rc = unpack(d, error);
    if (rc == CAI_OK)
      *out = &d->message;
    d->format = NULL;
    return rc;
  }
  t = event->token;
  if (event->type != CAI_PROTOCOL_PAYLOAD || t == NULL || t->indefinite)
    return invalid(error);
  if (d->phase == 0) {
    if (t->type != CAI_CBOR_ARRAY || t->argument != count)
      return invalid(error);
    d->phase = 1;
    return CAI_OK;
  }
  if (d->phase == 2) {
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > d->length - d->offset ||
          (t->length != 0U &&
           (t->data == NULL || memchr(t->data, '\0', t->length) != NULL)))
        return invalid(error);
      if (t->length != 0U)
        memcpy(d->string + d->offset, t->data, t->length);
      d->offset += t->length;
    } else if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_BYTES &&
               d->offset == d->length) {
      d->string[d->length] = '\0';
      d->fields[d->index].text = d->string;
      d->fields[d->index].length = d->length;
      ++d->index;
      d->phase = 1;
    } else
      return invalid(error);
    return CAI_OK;
  }
  if (d->phase != 1)
    return invalid(error);
  if (d->index == count) {
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    d->phase = 4;
    return CAI_OK;
  }
  expected = d->format[d->index];
  if (expected == 'u' || expected == 'b') {
    if (t->type != (expected == 'u' ? CAI_CBOR_UINT : CAI_CBOR_BOOL) ||
        (expected == 'b' && t->argument > 1U))
      return invalid(error);
    d->fields[d->index++].number = t->argument;
  } else if (t->type == CAI_CBOR_NULL && expected == 't')
    ++d->index;
  else {
    size_t capacity;
    d->string = string_target(d, &capacity);
    if (t->type != CAI_CBOR_BYTES || t->argument >= capacity)
      return invalid(error);
    d->offset = 0U;
    d->length = (size_t)t->argument;
    d->phase = 2;
  }
  return CAI_OK;
}
void cai_cli_input_wire_decoder_close(cai_cli_input_wire_decoder *decoder) {
  free(decoder);
}
static uint32_t integer_size(uint64_t value) {
  return value < 24U             ? 1U
         : value <= 255U         ? 2U
         : value <= 65535U       ? 3U
         : value <= 4294967295UL ? 5U
                                 : 9U;
}
static int token(const cai_cli_input_wire_output *out, cai_cbor_type type,
                 uint64_t argument, const void *bytes, size_t length,
                 cai_error *error) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = argument;
  t.data = bytes;
  t.length = length;
  return out->write(out->context, &t, error);
}
int cai_cli_input_wire_write(const cai_cli_input_wire_output *out,
                             const cai_cli_input_wire_message *message,
                             cai_error *error) {
  field fields[FIELD_LIMIT];
  const char *f;
  size_t i, count;
  uint32_t length;
  int rc;
  if (out == NULL || message == NULL || out->begin == NULL ||
      out->write == NULL || out->end == NULL)
    return invalid(error);
  f = format(message->header.kind);
  if (f == NULL)
    return invalid(error);
  count = strlen(f);
  memset(fields, 0, sizeof(fields));
  rc = pack(message, fields, error);
  if (rc != CAI_OK)
    return rc;
  length = integer_size(count);
  for (i = 0U; i < count; ++i) {
    if (f[i] == 'u')
      length += integer_size(fields[i].number);
    else if (f[i] == 'b') {
      if (fields[i].number > 1U)
        return invalid(error);
      ++length;
    } else if (f[i] == 't' && fields[i].text == NULL)
      ++length;
    else {
      size_t capacity = string_capacity(message->header.kind, i);
      if (fields[i].text != NULL)
        while (fields[i].length < capacity &&
               fields[i].text[fields[i].length] != '\0')
          ++fields[i].length;
      if (fields[i].length >= capacity)
        return invalid(error);
      length += integer_size(fields[i].length) + (uint32_t)fields[i].length;
    }
  }
  if (length > CAI_WIRE_MAX_RECORD)
    return invalid(error);
  rc = out->begin(out->context, &message->header, length, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_ARRAY, count, NULL, 0U, error);
  for (i = 0U; rc == CAI_OK && i < count; ++i) {
    if (f[i] == 'u' || f[i] == 'b')
      rc = token(out, f[i] == 'u' ? CAI_CBOR_UINT : CAI_CBOR_BOOL,
                 fields[i].number, NULL, 0U, error);
    else if (f[i] == 't' && fields[i].text == NULL)
      rc = token(out, CAI_CBOR_NULL, 0U, NULL, 0U, error);
    else {
      size_t at = 0U;
      rc = token(out, CAI_CBOR_BYTES, fields[i].length, NULL, 0U, error);
      while (rc == CAI_OK && at < fields[i].length) {
        size_t n = fields[i].length - at;
        if (n > CAI_CBOR_MAX_CHUNK_BYTES)
          n = CAI_CBOR_MAX_CHUNK_BYTES;
        rc = token(out, CAI_CBOR_DATA, 0U, fields[i].text + at, n, error);
        at += n;
      }
      if (rc == CAI_OK)
        rc = token(out, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
    }
  }
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = out->end(out->context, error);
  return rc;
}
