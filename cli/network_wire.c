/** @file Streaming network records, sizing from fields without a sizing pass.
 */
#include "network_wire.h"
#include "error.h"
#include <cai/utf8.h>
#include <stdlib.h>
#include <string.h>
struct cai_cli_network_decoder {
  cai_app_network_event event;
  char *strings[6];
  size_t field, offset, length;
  int data, complete;
};
static int invalid(cai_error *e) {
  return cai_cli_error(e, CAI_ERR_PROTOCOL,
                       "invalid network capability/action record");
}
static uint32_t integer_size(uint64_t n) {
  return n < 24U           ? 1U
         : n <= 255U       ? 2U
         : n <= 65535U     ? 3U
         : n <= UINT32_MAX ? 5U
                           : 9U;
}
static int emit(const cai_cli_input_wire_output *o, cai_cbor_type type,
                uint64_t n, const void *data, size_t length, cai_error *e) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = n;
  t.data = data;
  t.length = length;
  return o->write(o->context, &t, e);
}
int cai_cli_network_write(const cai_cli_input_wire_output *o,
                          const cai_app_network_event *event, cai_error *e) {
  const char *strings[6];
  uint64_t numbers[5];
  uint32_t size = 1U;
  size_t i;
  int rc;
  if (o == NULL || o->begin == NULL || o->write == NULL || o->end == NULL ||
      event == NULL || event->header.kind != CAI_PROTOCOL_NETWORK ||
      (int)event->operation < 0 || event->operation > CAI_APP_NETWORK_NOTICE ||
      event->flags > 7U || event->code < 0 || event->code > CAI_ERR_LIMIT)
    return invalid(e);
  strings[0] = event->address;
  strings[1] = event->advertise;
  strings[2] = event->interface_name;
  strings[3] = event->socat;
  strings[4] = event->prompt;
  strings[5] = event->reason;
  numbers[0] = event->operation;
  numbers[1] = event->timeout_ms;
  numbers[2] = event->transferred;
  numbers[3] = (uint64_t)event->code;
  numbers[4] = event->flags;
  for (i = 0U; i < 5U; i++)
    size += integer_size(numbers[i]);
  for (i = 0U; i < 6U; i++) {
    size_t n = strings[i] != NULL ? strlen(strings[i]) : 0U;
    cai_utf8_state utf8 = {0};
    if (n > (i == 4U ? CAI_CLI_INPUT_TEXT_MAX : 4096U))
      return invalid(e);
    if (!cai_utf8_feed(&utf8, strings[i], n) || !cai_utf8_complete(&utf8))
      return invalid(e);
    size += integer_size(n) + (uint32_t)n;
  }
  rc = o->begin(o->context, &event->header, size, e);
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_ARRAY, 11U, NULL, 0U, e);
  for (i = 0U; rc == CAI_OK && i < 5U; i++)
    rc = emit(o, CAI_CBOR_UINT, numbers[i], NULL, 0U, e);
  for (i = 0U; rc == CAI_OK && i < 6U; i++) {
    size_t n = strings[i] != NULL ? strlen(strings[i]) : 0U;
    rc = emit(o, CAI_CBOR_TEXT, n, NULL, 0U, e);
    if (rc == CAI_OK && n)
      rc = emit(o, CAI_CBOR_DATA, 0U, strings[i], n, e);
    if (rc == CAI_OK)
      rc = emit(o, CAI_CBOR_END, CAI_CBOR_TEXT, NULL, 0U, e);
  }
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, e);
  return rc == CAI_OK ? o->end(o->context, e) : rc;
}
static void clear(cai_cli_network_decoder *d) {
  size_t i;
  for (i = 0U; i < 6U; i++)
    free(d->strings[i]);
  memset(d, 0, sizeof(*d));
}
int cai_cli_network_decoder_open(cai_cli_network_decoder **out, cai_error *e) {
  *out = calloc(1U, sizeof(**out));
  return *out != NULL ? CAI_OK
                      : cai_cli_error(e, CAI_ERR_NOMEM,
                                      "allocate network field decoder");
}
int cai_cli_network_decode(cai_cli_network_decoder *d,
                           const cai_protocol_event *event,
                           const cai_app_network_event **out, cai_error *e) {
  const cai_cbor_token *t = event->token;
  *out = NULL;
  if (event->type == CAI_PROTOCOL_HEADER) {
    clear(d);
    d->event.header = *event->header;
    return event->header->kind == CAI_PROTOCOL_NETWORK ? CAI_OK : invalid(e);
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (!d->complete)
      return invalid(e);
    d->event.address = d->strings[0];
    d->event.advertise = d->strings[1];
    d->event.interface_name = d->strings[2];
    d->event.socat = d->strings[3];
    d->event.prompt = d->strings[4];
    d->event.reason = d->strings[5];
    *out = &d->event;
    return CAI_OK;
  }
  if (t->indefinite || d->complete)
    return invalid(e);
  if (d->field == 0U) {
    if (t->type != CAI_CBOR_ARRAY || t->argument != 11U)
      return invalid(e);
    d->field++;
  } else if (d->field <= 5U) {
    if (t->type != CAI_CBOR_UINT)
      return invalid(e);
    switch (d->field++) {
    case 1U:
      if (t->argument > CAI_APP_NETWORK_NOTICE)
        return invalid(e);
      d->event.operation = (cai_app_network_operation)t->argument;
      break;
    case 2U:
      d->event.timeout_ms = t->argument;
      break;
    case 3U:
      d->event.transferred = t->argument;
      break;
    case 4U:
      if (t->argument > CAI_ERR_LIMIT)
        return invalid(e);
      d->event.code = (int)t->argument;
      break;
    default:
      if (t->argument > 7U)
        return invalid(e);
      d->event.flags = (unsigned int)t->argument;
      break;
    }
  } else if (d->field <= 11U) {
    size_t index = d->field - 6U;
    if (!d->data) {
      size_t limit = index == 4U ? CAI_CLI_INPUT_TEXT_MAX : 4096U;
      if (t->type != CAI_CBOR_TEXT || t->argument > limit)
        return invalid(e);
      d->length = (size_t)t->argument;
      d->offset = 0U;
      d->strings[index] = calloc(d->length + 1U, 1U);
      if (d->strings[index] == NULL)
        return cai_cli_error(e, CAI_ERR_NOMEM,
                             "allocate bounded network field");
      d->data = 1;
    } else if (t->type == CAI_CBOR_DATA) {
      if (t->length > d->length - d->offset ||
          (t->length && memchr(t->data, 0, t->length)))
        return invalid(e);
      memcpy(d->strings[index] + d->offset, t->data, t->length);
      d->offset += t->length;
    } else {
      if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_TEXT ||
          d->offset != d->length)
        return invalid(e);
      d->data = 0;
      d->field++;
    }
  } else {
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(e);
    d->complete = 1;
  }
  return CAI_OK;
}
void cai_cli_network_decoder_close(cai_cli_network_decoder *d) {
  if (d != NULL) {
    clear(d);
    free(d);
  }
}
