#include "transcript_wire.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
typedef struct field {
  uint64_t value;
  int64_t integer;
  const void *bytes;
  size_t length;
} field;
struct cai_cli_transcript_decoder {
  cai_cli_transcript_message message;
  field fields[5];
  const char *format;
  size_t index, offset;
  int phase;
  char strings[5][193];
  unsigned char data[CAI_CLI_TRANSCRIPT_CHUNK];
};
static int invalid(cai_error *error) {
  return cai_cli_error(error, CAI_ERR_PROTOCOL, "invalid transcript payload");
}
static const char *shape(cai_protocol_kind kind) {
  switch (kind) {
  case CAI_PROTOCOL_SUBSCRIBE:
    return "ssuui";
  case CAI_PROTOCOL_HISTORY_GET:
    return "ssuu";
  case CAI_PROTOCOL_UNSUBSCRIBE:
    return "";
  case CAI_PROTOCOL_REPLAY_BEGIN:
    return "ssuuu";
  case CAI_PROTOCOL_STREAM_BEGIN:
    return "uisb";
  case CAI_PROTOCOL_STREAM_CHUNK:
  case CAI_PROTOCOL_SHELL_OUTPUT:
    return "ud";
  case CAI_PROTOCOL_SHELL_OUTPUT_END:
    return "u";
  case CAI_PROTOCOL_STREAM_END:
    return "uu";
  case CAI_PROTOCOL_REPLAY_END:
    return "u";
  case CAI_PROTOCOL_SUBSCRIPTION_ERROR:
    return "uus";
  default:
    return NULL;
  }
}
static size_t capacity(cai_protocol_kind kind, size_t index) {
  if (kind == CAI_PROTOCOL_SUBSCRIPTION_ERROR)
    return 192U;
  if ((kind == CAI_PROTOCOL_SUBSCRIBE || kind == CAI_PROTOCOL_HISTORY_GET ||
       kind == CAI_PROTOCOL_REPLAY_BEGIN) &&
      index == 1U)
    return 21U;
  return 129U;
}
static int valid_header(const cai_protocol_header *h) {
  int record = h->kind == CAI_PROTOCOL_STREAM_BEGIN ||
               h->kind == CAI_PROTOCOL_STREAM_CHUNK ||
               h->kind == CAI_PROTOCOL_STREAM_END ||
               h->kind == CAI_PROTOCOL_SHELL_OUTPUT ||
               h->kind == CAI_PROTOCOL_SHELL_OUTPUT_END;
  if (shape(h->kind) == NULL || h->request == 0U ||
      (record ? h->sequence == 0U : h->sequence != 0U))
    return 0;
  if (h->kind == CAI_PROTOCOL_SUBSCRIBE || h->kind == CAI_PROTOCOL_HISTORY_GET)
    return h->stream == 0U;
  if (h->stream != h->request)
    return 0;
  return h->kind == CAI_PROTOCOL_UNSUBSCRIBE ||
         h->kind == CAI_PROTOCOL_SUBSCRIPTION_ERROR || h->conversation != 0U;
}
static int pack(const cai_cli_transcript_message *m, field *f,
                cai_error *error) {
  const cai_transcript_event *e = &m->event;
  switch (m->header.kind) {
  case CAI_PROTOCOL_SUBSCRIBE:
    if (m->replay_turns < -1 || (e->position != 0U && m->replay_turns != -1))
      return invalid(error);
    f[4].integer = m->replay_turns;
    /* fall through */
  case CAI_PROTOCOL_HISTORY_GET:
    f[0].bytes = e->session;
    f[1].bytes = e->collection;
    f[2].value = e->position;
    f[3].value = e->sequence;
    break;
  case CAI_PROTOCOL_UNSUBSCRIBE:
    break;
  case CAI_PROTOCOL_REPLAY_BEGIN:
    f[0].bytes = e->session;
    f[1].bytes = e->collection;
    f[2].value = e->watermark;
    f[3].value = e->visibility;
    f[4].value = e->omitted;
    break;
  case CAI_PROTOCOL_STREAM_BEGIN:
    f[0].value = e->timestamp_ns;
    f[1].integer = e->utc_offset_seconds;
    f[2].bytes = e->type;
    f[3].value = e->has_data;
    break;
  case CAI_PROTOCOL_STREAM_CHUNK:
  case CAI_PROTOCOL_SHELL_OUTPUT:
    if (m->header.kind == CAI_PROTOCOL_SHELL_OUTPUT && e->length == 0U)
      return invalid(error);
    f[0].value = e->offset;
    f[1].bytes = e->data;
    f[1].length = e->length;
    break;
  case CAI_PROTOCOL_SHELL_OUTPUT_END:
    f[0].value = e->offset;
    break;
  case CAI_PROTOCOL_STREAM_END:
    f[0].value = e->position;
    f[1].value = e->offset;
    break;
  case CAI_PROTOCOL_REPLAY_END:
    f[0].value = e->watermark;
    break;
  case CAI_PROTOCOL_SUBSCRIPTION_ERROR:
    if (e->code <= CAI_OK || e->code > CAI_ERR_LIMIT ||
        e->failure < CAI_TRANSCRIPT_FAILED ||
        e->failure > CAI_TRANSCRIPT_CONTEXT_CLOSED)
      return invalid(error);
    f[0].value = e->failure;
    f[1].value = (uint64_t)e->code;
    f[2].bytes = e->reason;
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
static int unpack(cai_cli_transcript_decoder *d, cai_error *error) {
  cai_transcript_event *e = &d->message.event;
  const field *f = d->fields;
  e->stream = d->message.header.stream;
  e->conversation = d->message.header.conversation;
  e->sequence = d->message.header.sequence;
  switch (d->message.header.kind) {
  case CAI_PROTOCOL_SUBSCRIBE:
    d->message.replay_turns = f[4].integer;
    if (d->message.replay_turns < -1 ||
        (f[2].value != 0U && d->message.replay_turns != -1))
      return invalid(error);
    /* fall through */
  case CAI_PROTOCOL_HISTORY_GET:
    e->session = f[0].bytes;
    e->collection = f[1].bytes;
    e->position = f[2].value;
    e->sequence = f[3].value;
    break;
  case CAI_PROTOCOL_UNSUBSCRIBE:
    break;
  case CAI_PROTOCOL_REPLAY_BEGIN:
    e->kind = CAI_TRANSCRIPT_REPLAY_BEGIN;
    e->session = f[0].bytes;
    e->collection = f[1].bytes;
    e->watermark = f[2].value;
    e->visibility = f[3].value;
    e->omitted = f[4].value;
    break;
  case CAI_PROTOCOL_STREAM_BEGIN:
    e->kind = CAI_TRANSCRIPT_EVENT_BEGIN;
    e->timestamp_ns = f[0].value;
    e->utc_offset_seconds = f[1].integer;
    e->type = f[2].bytes;
    e->has_data = (int)f[3].value;
    break;
  case CAI_PROTOCOL_STREAM_CHUNK:
    e->kind = CAI_TRANSCRIPT_DATA;
    e->offset = f[0].value;
    e->data = f[1].bytes;
    e->length = f[1].length;
    break;
  case CAI_PROTOCOL_SHELL_OUTPUT:
    if (f[1].length == 0U)
      return invalid(error);
    e->kind = CAI_TRANSCRIPT_SHELL_OUTPUT;
    e->offset = f[0].value;
    e->data = f[1].bytes;
    e->length = f[1].length;
    break;
  case CAI_PROTOCOL_SHELL_OUTPUT_END:
    e->kind = CAI_TRANSCRIPT_SHELL_OUTPUT_END;
    e->offset = f[0].value;
    break;
  case CAI_PROTOCOL_STREAM_END:
    e->kind = CAI_TRANSCRIPT_EVENT_END;
    e->position = f[0].value;
    e->offset = f[1].value;
    break;
  case CAI_PROTOCOL_REPLAY_END:
    e->kind = CAI_TRANSCRIPT_REPLAY_END;
    e->watermark = f[0].value;
    break;
  case CAI_PROTOCOL_SUBSCRIPTION_ERROR:
    if (f[0].value > CAI_TRANSCRIPT_CONTEXT_CLOSED || f[1].value == 0U ||
        f[1].value > CAI_ERR_LIMIT)
      return invalid(error);
    e->kind = CAI_TRANSCRIPT_ERROR;
    e->failure = (cai_transcript_failure)f[0].value;
    e->code = (int)f[1].value;
    e->reason = f[2].bytes;
    break;
  default:
    return invalid(error);
  }
  return CAI_OK;
}
int cai_cli_transcript_decoder_open(cai_cli_transcript_decoder **out,
                                    cai_error *error) {
  if (out == NULL)
    return invalid(error);
  *out = calloc(1U, sizeof(**out));
  return *out != NULL ? CAI_OK
                      : cai_cli_error(error, CAI_ERR_NOMEM,
                                      "allocate transcript decoder");
}
int cai_cli_transcript_decode(cai_cli_transcript_decoder *d,
                              const cai_protocol_event *event,
                              const cai_cli_transcript_message **out,
                              cai_error *error) {
  const cai_cbor_token *t;
  field *f;
  char kind;
  size_t n;
  if (out != NULL)
    *out = NULL;
  if (d == NULL || event == NULL || event->header == NULL || out == NULL)
    return invalid(error);
  if (event->type == CAI_PROTOCOL_HEADER) {
    const cai_protocol_header *h = event->header;
    const char *format = shape(h->kind);
    if (!valid_header(h))
      return invalid(error);
    memset(&d->message, 0, sizeof(d->message));
    memset(d->fields, 0, sizeof(d->fields));
    d->message.header = *h;
    d->format = format;
    d->index = d->offset = 0U;
    d->phase = 0;
    return CAI_OK;
  }
  if (d->format == NULL)
    return invalid(error);
  n = strlen(d->format);
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    int rc;
    if (d->phase != 3)
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
    if (t->type != CAI_CBOR_ARRAY || t->argument != n)
      return invalid(error);
    d->phase = 1;
    return CAI_OK;
  }
  if (d->phase == 2) {
    f = &d->fields[d->index];
    if (t->type == CAI_CBOR_DATA) {
      unsigned char *target = d->format[d->index] == 'd'
                                  ? d->data
                                  : (unsigned char *)d->strings[d->index];
      if (t->length > f->length - d->offset ||
          (t->length != 0U &&
           (t->data == NULL || (d->format[d->index] == 's' &&
                                memchr(t->data, '\0', t->length) != NULL))))
        return invalid(error);
      if (t->length != 0U)
        memcpy(target + d->offset, t->data, t->length);
      d->offset += t->length;
    } else if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_BYTES &&
               d->offset == f->length) {
      if (d->format[d->index] == 's')
        d->strings[d->index][f->length] = '\0';
      ++d->index;
      d->phase = 1;
    } else
      return invalid(error);
    return CAI_OK;
  }
  if (d->phase != 1)
    return invalid(error);
  if (d->index == n) {
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    d->phase = 3;
    return CAI_OK;
  }
  kind = d->format[d->index];
  f = &d->fields[d->index];
  if (kind == 's' || kind == 'd') {
    size_t cap = kind == 'd' ? sizeof(d->data) + 1U
                             : capacity(d->message.header.kind, d->index);
    if (t->type != CAI_CBOR_BYTES || t->argument >= cap)
      return invalid(error);
    f->length = (size_t)t->argument;
    f->bytes = kind == 'd' ? (const void *)d->data : d->strings[d->index];
    d->offset = 0U;
    d->phase = 2;
  } else {
    if ((kind == 'u' && t->type != CAI_CBOR_UINT) ||
        (kind == 'b' && (t->type != CAI_CBOR_BOOL || t->argument > 1U)) ||
        (kind == 'i' &&
         ((t->type != CAI_CBOR_UINT && t->type != CAI_CBOR_NEGATIVE) ||
          t->argument > INT64_MAX)))
      return invalid(error);
    f->value = t->argument;
    f->integer = t->type == CAI_CBOR_NEGATIVE ? -1 - (int64_t)t->argument
                                              : (int64_t)t->argument;
    ++d->index;
  }
  return CAI_OK;
}
void cai_cli_transcript_decoder_close(cai_cli_transcript_decoder *d) {
  free(d);
}
static uint32_t size_of(uint64_t x) {
  return x < 24U             ? 1U
         : x <= 255U         ? 2U
         : x <= 65535U       ? 3U
         : x <= 4294967295UL ? 5U
                             : 9U;
}
static int token(const cai_cli_input_wire_output *out, cai_cbor_type kind,
                 uint64_t value, const void *data, size_t length,
                 cai_error *error) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = kind;
  t.argument = value;
  t.data = data;
  t.length = length;
  return out->write(out->context, &t, error);
}
int cai_cli_transcript_write(const cai_cli_input_wire_output *out,
                             const cai_cli_transcript_message *m,
                             cai_error *error) {
  field f[5];
  const char *format;
  size_t i, n;
  uint32_t length;
  int rc;
  if (out == NULL || m == NULL || out->begin == NULL || out->write == NULL ||
      out->end == NULL || !valid_header(&m->header))
    return invalid(error);
  format = shape(m->header.kind);
  memset(f, 0, sizeof(f));
  n = strlen(format);
  rc = pack(m, f, error);
  if (rc != CAI_OK)
    return rc;
  length = size_of(n);
  for (i = 0U; i < n; ++i) {
    if (format[i] == 's') {
      size_t cap = capacity(m->header.kind, i);
      const char *s = f[i].bytes;
      if (s != NULL)
        while (f[i].length < cap && s[f[i].length] != '\0')
          ++f[i].length;
      if (f[i].length >= cap)
        return invalid(error);
    }
    if (format[i] == 's' || format[i] == 'd') {
      if (f[i].length > CAI_CLI_TRANSCRIPT_CHUNK ||
          (f[i].length != 0U && f[i].bytes == NULL))
        return invalid(error);
      length += size_of(f[i].length) + (uint32_t)f[i].length;
    } else {
      if (format[i] == 'i')
        f[i].value = f[i].integer < 0 ? (uint64_t)(-(f[i].integer + 1))
                                      : (uint64_t)f[i].integer;
      if (format[i] == 'b' && f[i].value > 1U)
        return invalid(error);
      length += format[i] == 'b' ? 1U : size_of(f[i].value);
    }
  }
  rc = out->begin(out->context, &m->header, length, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_ARRAY, n, NULL, 0U, error);
  for (i = 0U; i < n && rc == CAI_OK; ++i) {
    char kind = format[i];
    if (kind == 's' || kind == 'd') {
      rc = token(out, CAI_CBOR_BYTES, f[i].length, NULL, 0U, error);
      if (rc == CAI_OK && f[i].length != 0U)
        rc = token(out, CAI_CBOR_DATA, 0U, f[i].bytes, f[i].length, error);
      if (rc == CAI_OK)
        rc = token(out, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
    } else
      rc = token(out,
                 kind == 'b'                       ? CAI_CBOR_BOOL
                 : kind == 'i' && f[i].integer < 0 ? CAI_CBOR_NEGATIVE
                                                   : CAI_CBOR_UINT,
                 f[i].value, NULL, 0U, error);
  }
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  return rc == CAI_OK ? out->end(out->context, error) : rc;
}
