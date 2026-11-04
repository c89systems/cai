#include "bootstrap.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
struct cai_cli_bootstrap_decoder {
  cai_cli_bootstrap_message value;
  size_t index, offset, length;
  int phase;
};
static int invalid(cai_error *error) {
  return cai_cli_error(error, CAI_ERR_PROTOCOL,
                       "invalid workspace bootstrap metadata");
}
static int valid(const cai_cli_bootstrap_message *v) {
  return (v->ready == 0 || v->ready == 1) &&
         (v->persist == 0 || v->persist == 1) &&
         (v->clipped == 0 || v->clipped == 1) && v->exit_code >= 0 &&
         v->exit_code <= 255 && v->code >= CAI_OK && v->code <= CAI_ERR_LIMIT &&
         (v->log_path[0] == '\0' || v->log_path[0] == '/') &&
         (!v->ready ||
          (v->code == CAI_OK && v->exit_code == 0 && v->session[0] != '\0'));
}
static void *field(cai_cli_bootstrap_message *v, size_t index,
                   size_t *capacity) {
  switch (index) {
  case 3U:
    *capacity = sizeof(v->instance);
    return v->instance;
  case 4U:
    *capacity = sizeof(v->session) - 1U;
    return v->session;
  case 5U:
    *capacity = sizeof(v->log_path) - 1U;
    return v->log_path;
  default:
    *capacity = sizeof(v->message) - 1U;
    return v->message;
  }
}
int cai_cli_bootstrap_decoder_open(cai_cli_bootstrap_decoder **out,
                                   cai_error *error) {
  if (out == NULL)
    return invalid(error);
  *out = calloc(1U, sizeof(**out));
  return *out != NULL ? CAI_OK
                      : cai_cli_error(error, CAI_ERR_NOMEM,
                                      "allocate workspace bootstrap decoder");
}
int cai_cli_bootstrap_decode(cai_cli_bootstrap_decoder *d,
                             const cai_protocol_event *event,
                             const cai_cli_bootstrap_message **out,
                             cai_error *error) {
  const cai_cbor_token *t;
  size_t capacity;
  void *target;
  if (out != NULL)
    *out = NULL;
  if (d == NULL || event == NULL || event->header == NULL || out == NULL)
    return invalid(error);
  if (event->type == CAI_PROTOCOL_HEADER) {
    const cai_protocol_header *h = event->header;
    if (h->kind != CAI_PROTOCOL_STATUS || h->request || h->conversation ||
        h->generation || h->stream || h->attachment ||
        (h->sequence != 1U && h->sequence != 2U))
      return invalid(error);
    memset(d, 0, sizeof(*d));
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (d->phase != 3 || !valid(&d->value) ||
        event->header->sequence != (d->value.ready ? 1U : 2U))
      return invalid(error);
    *out = &d->value;
    d->phase = 4;
    return CAI_OK;
  }
  t = event->token;
  if (event->type != CAI_PROTOCOL_PAYLOAD || t == NULL || t->indefinite)
    return invalid(error);
  if (d->phase == 0) {
    if (t->type != CAI_CBOR_ARRAY || t->argument != 9U)
      return invalid(error);
    d->phase = 1;
  } else if (d->phase == 2) {
    target = field(&d->value, d->index, &capacity);
    if (t->type == CAI_CBOR_DATA) {
      if (t->length > d->length - d->offset ||
          (t->length &&
           (t->data == NULL ||
            (d->index != 3U && memchr(t->data, '\0', t->length) != NULL))))
        return invalid(error);
      if (t->length)
        memcpy((unsigned char *)target + d->offset, t->data, t->length);
      d->offset += t->length;
    } else if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_BYTES &&
               d->offset == d->length) {
      if (d->index != 3U)
        ((char *)target)[d->length] = '\0';
      d->index++;
      d->phase = 1;
    } else
      return invalid(error);
  } else if (d->phase != 1)
    return invalid(error);
  else if (d->index == 9U) {
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(error);
    d->phase = 3;
  } else if (d->index >= 3U && d->index <= 6U) {
    (void)field(&d->value, d->index, &capacity);
    if (t->type != CAI_CBOR_BYTES || t->argument > capacity ||
        (d->index == 3U && t->argument != capacity))
      return invalid(error);
    d->offset = 0U;
    d->length = (size_t)t->argument;
    d->phase = 2;
  } else {
    int boolean = d->index == 0U || d->index >= 7U;
    int n;
    if ((boolean && (t->type != CAI_CBOR_BOOL || t->argument > 1U)) ||
        (!boolean && (t->type != CAI_CBOR_UINT || t->argument > 255U)))
      return invalid(error);
    n = (int)t->argument;
    switch (d->index) {
    case 0U:
      d->value.ready = n;
      break;
    case 1U:
      d->value.exit_code = n;
      break;
    case 2U:
      d->value.code = n;
      break;
    case 7U:
      d->value.clipped = n;
      break;
    default:
      d->value.persist = n;
      break;
    }
    d->index++;
  }
  return CAI_OK;
}
void cai_cli_bootstrap_decoder_close(cai_cli_bootstrap_decoder *d) { free(d); }
static uint32_t head_size(size_t n) {
  return n < 24U ? 1U : n <= 255U ? 2U : n <= 65535U ? 3U : 5U;
}
static int token(cai_wire_peer *peer, cai_cbor_type type, uint64_t argument,
                 const void *data, size_t length, cai_error *error) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = argument;
  t.data = data;
  t.length = length;
  return cai_wire_peer_write(peer, &t, error);
}
int cai_cli_bootstrap_write(cai_wire_peer *peer,
                            const cai_cli_bootstrap_message *v,
                            cai_error *error) {
  const void *strings[4];
  size_t lengths[4], i;
  uint32_t size;
  cai_protocol_header header;
  int rc;
  if (v == NULL || memchr(v->session, '\0', sizeof(v->session)) == NULL ||
      memchr(v->log_path, '\0', sizeof(v->log_path)) == NULL ||
      memchr(v->message, '\0', sizeof(v->message)) == NULL || !valid(v))
    return invalid(error);
  strings[0] = v->instance;
  strings[1] = v->session;
  strings[2] = v->log_path;
  strings[3] = v->message;
  lengths[0] = sizeof(v->instance);
  lengths[1] = strlen(v->session);
  lengths[2] = strlen(v->log_path);
  lengths[3] = strlen(v->message);
  size = 4U + head_size((size_t)v->exit_code) + head_size((size_t)v->code);
  for (i = 0U; i < 4U; i++)
    size += head_size(lengths[i]) + (uint32_t)lengths[i];
  memset(&header, 0, sizeof(header));
  header.kind = CAI_PROTOCOL_STATUS;
  header.sequence = v->ready ? 1U : 2U;
  rc = cai_protocol_peer_begin(peer, &header, size, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_ARRAY, 9U, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_BOOL, (uint64_t)v->ready, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_UINT, (uint64_t)v->exit_code, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_UINT, (uint64_t)v->code, NULL, 0U, error);
  for (i = 0U; i < 4U && rc == CAI_OK; i++) {
    rc = token(peer, CAI_CBOR_BYTES, lengths[i], NULL, 0U, error);
    if (rc == CAI_OK && lengths[i])
      rc = token(peer, CAI_CBOR_DATA, 0U, strings[i], lengths[i], error);
    if (rc == CAI_OK)
      rc = token(peer, CAI_CBOR_END, CAI_CBOR_BYTES, NULL, 0U, error);
  }
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_BOOL, (uint64_t)v->clipped, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_BOOL, (uint64_t)v->persist, NULL, 0U, error);
  if (rc == CAI_OK)
    rc = token(peer, CAI_CBOR_END, CAI_CBOR_ARRAY, NULL, 0U, error);
  return rc == CAI_OK ? cai_protocol_peer_end(peer, error) : rc;
}
int cai_cli_bootstrap_status(cai_cli_bootstrap_message *v, int ready,
                             int exit_code, const cai_app_status *status,
                             const cai_error *failure, cai_error *error) {
  const char *session = status && status->session_id ? status->session_id : "";
  const char *path = status && status->log_path ? status->log_path : "";
  const char *message;
  size_t length = 0U;
  /* status.error may describe a recovered diagnostic. Only a failed owner
   * outcome makes it the final failure when no process error was supplied. */
  if (!ready && failure == NULL && status != NULL &&
      status->outcome == CAI_APP_FAILED)
    failure = &status->error;
  message = failure && failure->message ? failure->message : "";
  if (v == NULL || strlen(session) >= sizeof(v->session) ||
      strlen(path) >= sizeof(v->log_path))
    return invalid(error);
  memset(v, 0, sizeof(*v));
  v->ready = ready;
  v->exit_code = exit_code;
  v->code = failure ? failure->code : CAI_OK;
  if (status) {
    memcpy(v->instance, status->server_instance, sizeof(v->instance));
    v->persist = status->has_server && status->server.persist;
  }
  strcpy(v->session, session);
  strcpy(v->log_path, path);
  while (length < sizeof(v->message) - 1U && message[length])
    length++;
  memcpy(v->message, message, length);
  v->clipped = message[length] != '\0';
  return valid(v) ? CAI_OK : invalid(error);
}
