#include "catalog_wire.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
struct cai_cli_catalog_decoder {
  cai_app_catalog_event row;
  char text[192];
  size_t part, offset, length;
};
static int invalid(cai_error *error) {
  return cai_cli_error(error, CAI_ERR_PROTOCOL, "invalid catalog row");
}
static int valid(const cai_app_catalog_event *e) {
  size_t n = e->text != NULL ? strlen(e->text) : 0U;
  if (e->kind != CAI_APP_CATALOG_SKILLS || e->revision == 0U ||
      e->conversation == 0U || e->phase < CAI_APP_CATALOG_BEGIN ||
      e->phase > CAI_APP_CATALOG_ERROR)
    return 0;
  if (e->phase == CAI_APP_CATALOG_ITEM)
    return n != 0U && n <= CAI_SKILL_NAME_MAX && e->code == CAI_OK;
  if (e->phase == CAI_APP_CATALOG_ERROR)
    return n < 192U && e->code > CAI_OK && e->code <= CAI_ERR_LIMIT;
  return n == 0U && e->code == CAI_OK &&
         (e->phase != CAI_APP_CATALOG_BEGIN || e->index == 0U);
}
static uint32_t integer_size(uint64_t n) {
  return n < 24U           ? 1U
         : n <= 255U       ? 2U
         : n <= 65535U     ? 3U
         : n <= UINT32_MAX ? 5U
                           : 9U;
}
static int token(const cai_cli_input_wire_output *out, cai_cbor_type type,
                 uint64_t n, const void *data, size_t length,
                 cai_error *error) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = n;
  t.data = data;
  t.length = length;
  return out->write(out->context, &t, error);
}
int cai_cli_catalog_write(const cai_cli_input_wire_output *out,
                          const cai_app_catalog_event *e, cai_error *error) {
  cai_protocol_header h;
  const char *text = e->text != NULL ? e->text : "";
  size_t n = strlen(text);
  uint32_t size;
  int rc;
  if (!valid(e))
    return invalid(error);
  memset(&h, 0, sizeof(h));
  h.kind = CAI_PROTOCOL_CATALOG;
  h.conversation = e->conversation;
  h.stream = e->revision;
  h.sequence = e->index;
  size = 1U + integer_size((uint64_t)e->kind) +
         integer_size((uint64_t)e->phase) + integer_size((uint64_t)e->code) +
         integer_size(n) + (uint32_t)n;
  rc = out->begin(out->context, &h, size, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_ARRAY, 4U, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_UINT, e->kind, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_UINT, e->phase, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_UINT, (uint64_t)e->code, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_BYTES, n, NULL, 0U, error);
  if (rc == CAI_OK && n != 0U)
    rc = token(out, CAI_CBOR_DATA, 0U, text, n, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(out, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  return rc == CAI_OK ? out->end(out->context, error) : rc;
}
int cai_cli_catalog_decoder_open(cai_cli_catalog_decoder **out,
                                 cai_error *error) {
  *out = calloc(1U, sizeof(**out));
  return *out != NULL ? CAI_OK
                      : cai_cli_error(error, CAI_ERR_NOMEM,
                                      "allocate catalog row decoder");
}
int cai_cli_catalog_decode(cai_cli_catalog_decoder *d,
                           const cai_protocol_event *event,
                           const cai_app_catalog_event **out,
                           cai_error *error) {
  const cai_cbor_token *t = event->token;
  *out = NULL;
  if (event->type == CAI_PROTOCOL_HEADER) {
    const cai_protocol_header *h = event->header;
    if (h->kind != CAI_PROTOCOL_CATALOG || h->request != 0U ||
        h->stream == 0U || h->conversation == 0U)
      return invalid(error);
    memset(d, 0, sizeof(*d));
    d->row.revision = h->stream;
    d->row.conversation = h->conversation;
    d->row.index = h->sequence;
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    d->row.text = d->text;
    if (d->part != 8U || !valid(&d->row))
      return invalid(error);
    *out = &d->row;
    return CAI_OK;
  }
  if (t->indefinite)
    return invalid(error);
  if (d->part == 0U) {
    if (t->type != CAI_CBOR_ARRAY || t->argument != 4U)
      return invalid(error);
  } else if (d->part <= 3U) {
    if (t->type != CAI_CBOR_UINT)
      return invalid(error);
    if (d->part == 1U) {
      if (t->argument != CAI_APP_CATALOG_SKILLS)
        return invalid(error);
      d->row.kind = (cai_app_catalog_kind)t->argument;
    } else if (d->part == 2U) {
      if (t->argument > CAI_APP_CATALOG_ERROR)
        return invalid(error);
      d->row.phase = (cai_app_catalog_phase)t->argument;
    } else {
      if (t->argument > CAI_ERR_LIMIT)
        return invalid(error);
      d->row.code = (int)t->argument;
    }
  } else if (d->part == 4U) {
    if (t->type != CAI_CBOR_BYTES || t->argument >= sizeof(d->text))
      return invalid(error);
    d->length = (size_t)t->argument;
  } else if (d->part == 5U) {
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > d->length - d->offset ||
          (t->length != 0U && memchr(t->data, '\0', t->length) != NULL))
        return invalid(error);
      memcpy(d->text + d->offset, t->data, t->length);
      d->offset += t->length;
      return CAI_OK;
    }
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_BYTES ||
        d->offset != d->length)
      return invalid(error);
  } else if (d->part == 6U) {
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    d->part = 8U;
    return CAI_OK;
  } else
    return invalid(error);
  ++d->part;
  return CAI_OK;
}
void cai_cli_catalog_decoder_close(cai_cli_catalog_decoder *d) { free(d); }
