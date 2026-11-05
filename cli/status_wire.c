#define _POSIX_C_SOURCE 200809L
#include "status_wire.h"
#include "error.h"
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The descriptor table is the single schema for copy, comparison, size,
 * encoding and decoding. Only bounded presentation metadata is retained. */
typedef struct field {
  size_t offset;
  char type;
} field;
#define FIELD(member, type) {offsetof(cai_app_view, member), type}
static const field fields[] = {
    FIELD(running, 'b'),
    FIELD(runtime_ready, 'b'),
    FIELD(outcome, 'n'),
    FIELD(exit_code, 'n'),
    FIELD(runtime_state, 'n'),
    FIELD(session_id, 's'),
    FIELD(provider, 's'),
    FIELD(last_response_id, 's'),
    FIELD(provider_conversation_id, 's'),
    FIELD(log_path, 's'),
    FIELD(effective.present, 'p'),
    FIELD(effective.model, 's'),
    FIELD(effective.reasoning_effort, 's'),
    FIELD(effective.reasoning_summary, 's'),
    FIELD(effective.service_tier, 's'),
    FIELD(pending.present, 'p'),
    FIELD(pending.model, 's'),
    FIELD(pending.reasoning_effort, 's'),
    FIELD(pending.reasoning_summary, 's'),
    FIELD(pending.service_tier, 's'),
    FIELD(metrics.context_window_tokens, 'i'),
    FIELD(metrics.context_used_tokens, 'i'),
    FIELD(metrics.has_context_usage, 'b'),
    FIELD(metrics.turn_active, 'b'),
    FIELD(metrics.turn_elapsed_ms, 'u'),
    FIELD(metrics.has_last_turn, 'b'),
    FIELD(metrics.last_turn_duration_ms, 'u'),
    FIELD(metrics.last_turn_finished_unix_seconds, 'i'),
    FIELD(metrics.session_usage.usage.input_tokens, 'l'),
    FIELD(metrics.session_usage.usage.input_cached_tokens, 'l'),
    FIELD(metrics.session_usage.usage.output_tokens, 'l'),
    FIELD(metrics.session_usage.usage.output_reasoning_tokens, 'l'),
    FIELD(metrics.session_usage.usage.total_tokens, 'l'),
    FIELD(metrics.session_usage.estimated_spend_usd, 'f'),
    FIELD(metrics.session_usage.estimated_spend_eur, 'f'),
    FIELD(metrics.session_usage.limit_exceeded, 'b'),
    FIELD(goal.has_goal, 'b'),
    FIELD(goal.status, 's'),
    FIELD(goal.has_token_budget, 'b'),
    FIELD(goal.token_budget, 'l'),
    FIELD(goal.tokens_used, 'l'),
    FIELD(goal.remaining_tokens, 'l'),
    FIELD(goal.elapsed_seconds, 'i'),
    FIELD(goal.created_at, 'i'),
    FIELD(goal.updated_at, 'i'),
    FIELD(quota.has_short_window, 'b'),
    FIELD(quota.short_window_seconds, 'l'),
    FIELD(quota.short_window_remaining_percent, 'f'),
    FIELD(quota.has_weekly, 'b'),
    FIELD(quota.weekly_remaining_percent, 'f'),
    FIELD(quota.has_credit_balance, 'b'),
    FIELD(quota.credit_balance, 'f'),
    FIELD(quota.credits_unlimited, 'b'),
    FIELD(yolo, 'b'),
    FIELD(foreground, 'b'),
    FIELD(yield_time_ms, 'l')};
#undef FIELD
static const size_t groups[] = {5U, 5U, 5U, 5U, 8U, 8U, 9U, 8U, 3U};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
struct cai_cli_status_decoder {
  cai_app_view candidate;
  size_t group, index, in_group, offset, length, allocated;
  int phase;
};
static void *at(cai_app_view *v, size_t index) {
  return (unsigned char *)v + fields[index].offset;
}
static const void *cat(const cai_app_view *v, size_t index) {
  return (const unsigned char *)v + fields[index].offset;
}
static int invalid(cai_error *e) {
  return cai_cli_error(e, CAI_ERR_PROTOCOL, "invalid workspace status view");
}
void cai_cli_status_cleanup(cai_app_view *v) {
  size_t i;
  if (v == NULL)
    return;
  for (i = 0U; i < COUNT(fields); ++i)
    if (fields[i].type == 's') {
      void *owned;
      memcpy(&owned, cat(v, i), sizeof(owned));
      free(owned);
    }
  memset(v, 0, sizeof(*v));
}
int cai_cli_status_copy(cai_app_view *out, const cai_app_view *v,
                        cai_error *e) {
  cai_app_view copy = *v;
  size_t i;
  /* Clear every borrowed pointer before allocation so failure cleanup never
   * frees a source string. Commit the replacement only after all copies. */
  for (i = 0U; i < COUNT(fields); ++i)
    if (fields[i].type == 's') {
      const char *s = NULL;
      memcpy(at(&copy, i), &s, sizeof(s));
    }
  for (i = 0U; i < COUNT(fields); ++i)
    if (fields[i].type == 's') {
      const char *s, *owned;
      memcpy(&s, cat(v, i), sizeof(s));
      if (s == NULL)
        continue;
      owned = strdup(s);
      if (owned == NULL) {
        cai_cli_status_cleanup(&copy);
        return cai_cli_error(e, CAI_ERR_NOMEM, "copy workspace status view");
      }
      memcpy(at(&copy, i), &owned, sizeof(owned));
    }
  cai_cli_status_cleanup(out);
  *out = copy;
  return CAI_OK;
}
static int field_token(const cai_app_view *v, size_t i, cai_cbor_token *t,
                       cai_error *e) {
  const void *p = cat(v, i);
  long long signed_value;
  unsigned long long unsigned_value;
  unsigned int mask;
  int n;
  double value;
  const char *s;
  memset(t, 0, sizeof(*t));
  t->type = CAI_CBOR_UINT;
  switch (fields[i].type) {
  case 's':
    memcpy(&s, p, sizeof(s));
    t->type = s == NULL ? CAI_CBOR_NULL : CAI_CBOR_BYTES;
    t->data = s;
    t->argument = s == NULL ? 0U : strnlen(s, CAI_WIRE_MAX_RECORD);
    if (t->argument >= CAI_WIRE_MAX_RECORD || (i == 5U && t->argument > 128U) ||
        (i == 9U && t->argument > 4095U))
      return CAI_ERR_LIMIT;
    break;
  case 'f':
    memcpy(&value, p, sizeof(value));
    return cai_cbor_token_set_float64(t, value, e);
  case 'b':
  case 'n':
    memcpy(&n, p, sizeof(n));
    if (fields[i].type == 'n' && n < 0)
      return CAI_ERR_INVALID;
    t->type = fields[i].type == 'b' ? CAI_CBOR_BOOL : CAI_CBOR_UINT;
    t->argument = fields[i].type == 'b' ? (uint64_t)(n != 0) : (uint64_t)n;
    break;
  case 'p':
    memcpy(&mask, p, sizeof(mask));
    if (mask > 15U)
      return CAI_ERR_INVALID;
    t->argument = mask;
    break;
  case 'u':
    memcpy(&unsigned_value, p, sizeof(unsigned_value));
    t->argument = unsigned_value;
    break;
  default:
    memcpy(&signed_value, p, sizeof(signed_value));
    if (fields[i].type == 'l' && signed_value < 0)
      return CAI_ERR_INVALID;
    if (signed_value < 0) {
      t->type = CAI_CBOR_NEGATIVE;
      t->argument = (uint64_t)(-(signed_value + 1));
    } else
      t->argument = (uint64_t)signed_value;
    break;
  }
  return CAI_OK;
}
int cai_cli_status_equal(const cai_app_view *a, const cai_app_view *b) {
  size_t i;
  if (a->available != b->available || a->conversation != b->conversation)
    return 0;
  if (!a->available)
    return a->code == b->code && strcmp(a->reason, b->reason) == 0;
  for (i = 0U; i < COUNT(fields); ++i) {
    cai_cbor_token x, y;
    if ((fields[i].offset == offsetof(cai_app_view, metrics.turn_elapsed_ms) &&
         a->metrics.turn_active && b->metrics.turn_active) ||
        (fields[i].offset == offsetof(cai_app_view, goal.elapsed_seconds) &&
         a->goal.has_goal && b->goal.has_goal && a->goal.status != NULL &&
         b->goal.status != NULL && strcmp(a->goal.status, "active") == 0 &&
         strcmp(b->goal.status, "active") == 0))
      continue;
    if (field_token(a, i, &x, NULL) != CAI_OK ||
        field_token(b, i, &y, NULL) != CAI_OK || x.type != y.type ||
        x.argument != y.argument ||
        (x.type == CAI_CBOR_BYTES &&
         memcmp(x.data, y.data, (size_t)x.argument) != 0))
      return 0;
  }
  return 1;
}
static uint32_t scalar_size(uint64_t n) {
  return n < 24U           ? 1U
         : n <= 255U       ? 2U
         : n <= 65535U     ? 3U
         : n <= UINT32_MAX ? 5U
                           : 9U;
}
static int length_of(const cai_app_view *v, uint32_t *out) {
  uint64_t size = 3U + COUNT(groups); /* outer, availability, body, groups */
  size_t i;
  if (v->outcome > CAI_APP_FAILED || v->runtime_state > CAI_AGENT_CANCELLED)
    return CAI_ERR_INVALID;
  for (i = 0U; i < COUNT(fields); ++i) {
    cai_cbor_token t;
    int rc = field_token(v, i, &t, NULL);
    if (rc != CAI_OK)
      return rc;
    size += t.type == CAI_CBOR_FLOAT64 ? 9U
            : t.type == CAI_CBOR_BOOL || t.type == CAI_CBOR_NULL
                ? 1U
                : scalar_size(t.argument);
    if (t.type == CAI_CBOR_BYTES)
      size += t.argument;
  }
  /* The socket stamps attachment/generation; the cache assigns a revision
   * after validating its source. Reserve all three maximum widths. */
  if (size + 9U + scalar_size(CAI_PROTOCOL_STATUS) + 20U +
          scalar_size(v->conversation) + 9U >
      CAI_WIRE_MAX_RECORD)
    return CAI_ERR_LIMIT;
  *out = (uint32_t)size;
  return CAI_OK;
}
int cai_cli_status_representable(const cai_app_view *v) {
  uint32_t length;
  return length_of(v, &length);
}
static int emit(const cai_cli_input_wire_output *o, cai_cbor_type type,
                uint64_t value, cai_error *e) {
  cai_cbor_token t;
  memset(&t, 0, sizeof(t));
  t.type = type;
  t.argument = value;
  return o->write(o->context, &t, e);
}
static int string(const cai_cli_input_wire_output *o, const cai_cbor_token *t,
                  cai_error *e) {
  size_t offset = 0U, length = (size_t)t->argument;
  int rc = emit(o, CAI_CBOR_BYTES, t->argument, e);
  while (rc == CAI_OK && offset < length) {
    cai_cbor_token chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.type = CAI_CBOR_DATA;
    chunk.data = (const unsigned char *)t->data + offset;
    chunk.length = length - offset;
    if (chunk.length > CAI_WIRE_CHUNK_SIZE)
      chunk.length = CAI_WIRE_CHUNK_SIZE;
    rc = o->write(o->context, &chunk, e);
    offset += chunk.length;
  }
  return rc == CAI_OK ? emit(o, CAI_CBOR_END, CAI_CBOR_BYTES, e) : rc;
}
int cai_cli_status_write(const cai_cli_input_wire_output *o,
                         const cai_app_view *v, cai_error *e) {
  cai_protocol_header h;
  uint32_t length;
  const char *reason = "Status metadata exceeds the wire profile";
  size_t g, i = 0U, j;
  int available, code, rc;
  if (o == NULL || v == NULL || o->begin == NULL || o->write == NULL ||
      o->end == NULL || v->revision == 0U)
    return invalid(e);
  memset(&h, 0, sizeof(h));
  h.kind = CAI_PROTOCOL_STATUS;
  h.sequence = v->revision;
  h.conversation = v->conversation;
  code = v->available ? length_of(v, &length) : v->code;
  available = v->available && code == CAI_OK;
  if (!available) {
    if (!v->available) {
      if (code <= CAI_OK || code > CAI_ERR_LIMIT ||
          strnlen(v->reason, sizeof(v->reason)) == sizeof(v->reason))
        return invalid(e);
      reason = v->reason;
    }
    length = 3U + scalar_size((uint64_t)code) + scalar_size(strlen(reason)) +
             (uint32_t)strlen(reason);
  }
  rc = o->begin(o->context, &h, length, e);
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_ARRAY, 2U, e);
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_BOOL, (uint64_t)available, e);
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_ARRAY, available ? COUNT(groups) : 2U, e);
  if (!available) {
    cai_cbor_token t;
    if (rc == CAI_OK)
      rc = emit(o, CAI_CBOR_UINT, (uint64_t)code, e);
    memset(&t, 0, sizeof(t));
    t.data = reason;
    t.argument = strlen(reason);
    if (rc == CAI_OK)
      rc = string(o, &t, e);
  } else
    for (g = 0U; rc == CAI_OK && g < COUNT(groups); ++g) {
      rc = emit(o, CAI_CBOR_ARRAY, groups[g], e);
      for (j = 0U; rc == CAI_OK && j < groups[g]; ++j, ++i) {
        cai_cbor_token t;
        rc = field_token(v, i, &t, e);
        if (rc == CAI_OK)
          rc = t.type == CAI_CBOR_BYTES ? string(o, &t, e)
                                        : o->write(o->context, &t, e);
      }
      if (rc == CAI_OK)
        rc = emit(o, CAI_CBOR_END, CAI_CBOR_ARRAY, e);
    }
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_END, CAI_CBOR_ARRAY, e);
  if (rc == CAI_OK)
    rc = emit(o, CAI_CBOR_END, CAI_CBOR_ARRAY, e);
  return rc == CAI_OK ? o->end(o->context, e) : rc;
}
int cai_cli_status_decoder_open(cai_cli_status_decoder **out, cai_error *e) {
  if (out == NULL)
    return invalid(e);
  *out = calloc(1U, sizeof(**out));
  return *out != NULL
             ? CAI_OK
             : cai_cli_error(e, CAI_ERR_NOMEM, "allocate status decoder");
}
static int scalar(cai_cli_status_decoder *d, const cai_cbor_token *t,
                  cai_error *e) {
  void *p = at(&d->candidate, d->index);
  char type = fields[d->index].type;
  long long n;
  unsigned long long u;
  unsigned int mask;
  int value;
  double f;
  if (type == 'f') {
    if (cai_cbor_token_get_float64(t, &f, e) != CAI_OK)
      return invalid(e);
    memcpy(p, &f, sizeof(f));
  } else if (type == 'b') {
    if (t->type != CAI_CBOR_BOOL || t->argument > 1U)
      return invalid(e);
    value = (int)t->argument;
    memcpy(p, &value, sizeof(value));
  } else {
    if ((t->type != CAI_CBOR_UINT &&
         (type != 'i' || t->type != CAI_CBOR_NEGATIVE)) ||
        (type == 'n' && t->argument > INT_MAX) ||
        (type == 'p' && t->argument > 15U) ||
        ((type == 'i' || type == 'l') && t->argument > LLONG_MAX))
      return invalid(e);
    if (type == 'n') {
      value = (int)t->argument;
      memcpy(p, &value, sizeof(value));
    } else if (type == 'p') {
      mask = (unsigned int)t->argument;
      memcpy(p, &mask, sizeof(mask));
    } else if (type == 'u') {
      u = t->argument;
      memcpy(p, &u, sizeof(u));
    } else {
      n = t->type == CAI_CBOR_NEGATIVE ? -1LL - (long long)t->argument
                                       : (long long)t->argument;
      memcpy(p, &n, sizeof(n));
    }
  }
  ++d->index;
  ++d->in_group;
  return CAI_OK;
}
int cai_cli_status_decode(cai_cli_status_decoder *d,
                          const cai_protocol_event *event,
                          const cai_app_view **out, cai_error *e) {
  const cai_cbor_token *t;
  cai_app_view *v;
  if (out != NULL)
    *out = NULL;
  if (d == NULL || event == NULL || event->header == NULL || out == NULL)
    return invalid(e);
  v = &d->candidate;
  if (event->type == CAI_PROTOCOL_HEADER) {
    const cai_protocol_header *h = event->header;
    if (h->kind != CAI_PROTOCOL_STATUS || h->request != 0U ||
        h->generation == 0U || h->attachment == 0U || h->stream != 0U ||
        h->sequence == 0U)
      return invalid(e);
    cai_cli_status_cleanup(v);
    v->revision = h->sequence;
    v->conversation = h->conversation;
    d->group = d->index = d->in_group = d->allocated = 0U;
    d->phase = 1;
    return CAI_OK;
  }
  if (event->type == CAI_PROTOCOL_COMPLETE) {
    if (d->phase != 12 ||
        (v->available && (v->outcome > CAI_APP_FAILED ||
                          v->runtime_state > CAI_AGENT_CANCELLED)))
      return invalid(e);
    d->phase = 0;
    *out = v;
    return CAI_OK;
  }
  t = event->token;
  if (event->type != CAI_PROTOCOL_PAYLOAD || t == NULL || t->indefinite)
    return invalid(e);
  switch (d->phase) {
  case 1:
    if (t->type != CAI_CBOR_ARRAY || t->argument != 2U)
      return invalid(e);
    d->phase = 2;
    break;
  case 2:
    if (t->type != CAI_CBOR_BOOL || t->argument > 1U)
      return invalid(e);
    v->available = (int)t->argument;
    d->phase = 3;
    break;
  case 3:
    if (t->type != CAI_CBOR_ARRAY ||
        t->argument != (v->available ? COUNT(groups) : 2U))
      return invalid(e);
    d->phase = v->available ? 4 : 8;
    break;
  case 4:
    if (d->group >= COUNT(groups) || t->type != CAI_CBOR_ARRAY ||
        t->argument != groups[d->group])
      return invalid(e);
    d->in_group = 0U;
    d->phase = 5;
    break;
  case 5:
    if (d->in_group == groups[d->group]) {
      if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
        return invalid(e);
      d->phase = ++d->group == COUNT(groups) ? 10 : 4;
    } else if (fields[d->index].type != 's')
      return scalar(d, t, e);
    else if (t->type == CAI_CBOR_NULL) {
      ++d->index;
      ++d->in_group;
    } else {
      char *s;
      if (t->type != CAI_CBOR_BYTES || t->argument >= CAI_WIRE_MAX_RECORD ||
          t->argument + 1U > CAI_WIRE_MAX_RECORD - d->allocated ||
          (d->index == 5U && t->argument > 128U) ||
          (d->index == 9U && t->argument > 4095U))
        return invalid(e);
      d->length = (size_t)t->argument;
      s = malloc(d->length + 1U);
      if (s == NULL)
        return cai_cli_error(e, CAI_ERR_NOMEM, "allocate status string");
      s[d->length] = '\0';
      memcpy(at(v, d->index), &s, sizeof(s));
      d->allocated += d->length + 1U;
      d->offset = 0U;
      d->phase = 6;
    }
    break;
  case 6:
  case 9:
    if (t->type == CAI_CBOR_END && t->argument == CAI_CBOR_BYTES &&
        d->offset == d->length) {
      if (d->phase == 9)
        d->phase = 10;
      else {
        ++d->index;
        ++d->in_group;
        d->phase = 5;
      }
    } else {
      char *s = v->reason;
      if (t->type != CAI_CBOR_DATA || t->length > d->length - d->offset ||
          (t->length != 0U &&
           (t->data == NULL || memchr(t->data, 0, t->length) != NULL)))
        return invalid(e);
      if (d->phase == 6)
        memcpy(&s, at(v, d->index), sizeof(s));
      if (t->length != 0U)
        memcpy(s + d->offset, t->data, t->length);
      d->offset += t->length;
    }
    break;
  case 8:
    if (t->type != CAI_CBOR_UINT || t->argument == 0U ||
        t->argument > CAI_ERR_LIMIT)
      return invalid(e);
    v->code = (int)t->argument;
    d->phase = 7;
    break;
  case 7:
    if (t->type != CAI_CBOR_BYTES || t->argument >= sizeof(v->reason))
      return invalid(e);
    d->length = (size_t)t->argument;
    d->offset = 0U;
    v->reason[d->length] = '\0';
    d->phase = 9;
    break;
  case 10:
  case 11:
    if (t->type != CAI_CBOR_END || t->argument != CAI_CBOR_ARRAY)
      return invalid(e);
    ++d->phase;
    break;
  default:
    return invalid(e);
  }
  return CAI_OK;
}
void cai_cli_status_decoder_close(cai_cli_status_decoder *d) {
  if (d != NULL) {
    cai_cli_status_cleanup(&d->candidate);
    free(d);
  }
}
void cai_cli_status_take(cai_cli_status_decoder *d, cai_app_view *v) {
  cai_cli_status_cleanup(v);
  *v = d->candidate;
  memset(&d->candidate, 0, sizeof(d->candidate));
}
void cai_app_view_elapsed(const cai_app_view *v, uint64_t now,
                          unsigned long long *turn, long long *goal) {
  uint64_t delta = 0U, seconds;
  unsigned long long t = 0U;
  long long g = 0;
  if (v != NULL && v->available) {
    t = v->metrics.turn_elapsed_ms;
    g = v->goal.elapsed_seconds;
    if (!v->stale && now > v->received_ms)
      delta = now - v->received_ms;
    if (v->metrics.turn_active)
      t = delta > ULLONG_MAX - t ? ULLONG_MAX : t + delta;
    seconds = delta / 1000U;
    if (v->goal.has_goal && v->goal.status != NULL &&
        strcmp(v->goal.status, "active") == 0)
      g = seconds > (uint64_t)LLONG_MAX ||
                  (g > 0 && seconds > (uint64_t)(LLONG_MAX - g))
              ? LLONG_MAX
              : g + (long long)seconds;
  }
  if (turn != NULL)
    *turn = t;
  if (goal != NULL)
    *goal = g;
}
