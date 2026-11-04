#include "../cli/status_wire.h"
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  cai_cli_status_decoder *decoder;
  cai_cbor_decoder *cbor;
  cai_cbor_encoder *encoder;
  cai_sink *sink;
  cai_protocol_header header;
  const cai_app_view *complete;
  size_t declared, bytes, writes;
  int reject, incomplete, fault;
  size_t arrays;
} fixture;
static int receive(void *context, const cai_cbor_token *token, cai_error *e) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_app_view *view = NULL;
  cai_cbor_token changed = *token;
  static const unsigned char nul = 0U;
  int rc;
  if (token->type == CAI_CBOR_ARRAY)
    ++f->arrays;
  if ((f->fault == 1 && token->type == CAI_CBOR_ARRAY) ||
      (f->fault == 2 && token->type == CAI_CBOR_BOOL) ||
      (f->fault == 3 && token->type == CAI_CBOR_ARRAY && f->arrays == 3U)) {
    changed.argument = 99U;
    f->fault = 0;
  } else if (f->fault == 4 && token->type == CAI_CBOR_UINT) {
    changed.argument = 99U; /* Invalid application outcome at COMPLETE. */
    f->fault = 0;
  } else if (f->fault == 5 && token->type == CAI_CBOR_BYTES) {
    changed.argument = 129U; /* Session identity cannot exceed 128 bytes. */
    f->fault = 0;
  } else if (f->fault == 6 && token->type == CAI_CBOR_DATA &&
             token->length != 0U) {
    changed.data = &nul;
    changed.length = 1U;
    f->fault = 0;
  } else if (f->fault == 7 && token->type == CAI_CBOR_FLOAT64) {
    changed.argument =
        0x7ff0000000000000ULL; /* Infinity is not a wire value. */
    f->fault = 0;
  } else if (f->fault == 8 && token->type == CAI_CBOR_NULL) {
    changed.type = CAI_CBOR_BOOL;
    f->fault = 0;
  }
  event.type = CAI_PROTOCOL_PAYLOAD;
  event.header = &f->header;
  event.token = &changed;
  rc = cai_cli_status_decode(f->decoder, &event, &view, e);
  assert(view == NULL);
  return rc;
}
static int bytes(void *context, const void *data, size_t length, cai_error *e) {
  fixture *f = context;
  const unsigned char *p = data;
  size_t i;
  ++f->writes;
  if (f->reject && f->writes == 3U)
    return CAI_ERR_CANCELLED;
  f->bytes += length;
  for (i = 0U; i < length; ++i) {
    int rc = cai_cbor_decoder_feed(f->cbor, p + i, 1U, e);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}
static int begin(void *context, const cai_protocol_header *h, uint32_t n,
                 cai_error *e) {
  fixture *f = context;
  cai_protocol_event event;
  const cai_app_view *view = NULL;
  f->declared = n;
  f->header = *h;
  f->header.attachment = 1U;
  f->header.generation = 2U;
  event.header = &f->header;
  event.type = CAI_PROTOCOL_HEADER;
  event.token = NULL;
  return cai_cli_status_decode(f->decoder, &event, &view, e);
}
static int token(void *context, const cai_cbor_token *t, cai_error *e) {
  return cai_cbor_encoder_write(((fixture *)context)->encoder, t, e);
}
static int end(void *context, cai_error *e) {
  fixture *f = context;
  cai_protocol_event event;
  int rc = cai_cbor_encoder_finish(f->encoder, e);
  if (rc == CAI_OK)
    rc = cai_cbor_decoder_finish(f->cbor, e);
  if (rc != CAI_OK)
    return rc;
  assert(f->bytes == f->declared && f->complete == NULL);
  if (f->incomplete)
    return CAI_OK;
  event.header = &f->header;
  event.type = CAI_PROTOCOL_COMPLETE;
  event.token = NULL;
  return cai_cli_status_decode(f->decoder, &event, &f->complete, e);
}
static void open_fixture(fixture *f) {
  cai_sink_callbacks sink;
  memset(f, 0, sizeof(*f));
  memset(&sink, 0, sizeof(sink));
  sink.context = f;
  sink.write = bytes;
  assert(cai_cli_status_decoder_open(&f->decoder, NULL) == CAI_OK);
  assert(cai_cbor_decoder_open(receive, f, &f->cbor, NULL) == CAI_OK);
  assert(cai_sink_from_callbacks(&sink, &f->sink, NULL) == CAI_OK);
  assert(cai_cbor_encoder_open(f->sink, &f->encoder, NULL) == CAI_OK);
}
static void close_fixture(fixture *f) {
  cai_cbor_encoder_close(f->encoder);
  cai_cbor_decoder_close(f->cbor);
  cai_cli_status_decoder_close(f->decoder);
  cai_sink_close(f->sink);
}
static int send(fixture *f, const cai_app_view *v) {
  cai_cli_input_wire_output out;
  out.context = f;
  out.begin = begin;
  out.write = token;
  out.end = end;
  return cai_cli_status_write(&out, v, NULL);
}
static void populate(cai_app_view *v) {
  memset(v, 0, sizeof(*v));
  v->available = v->running = v->runtime_ready = 1;
  v->revision = UINT64_MAX;
  v->conversation = 42U;
  v->outcome = 1;
  v->exit_code = 23;
  v->runtime_state = CAI_AGENT_DISPATCHING_TOOL;
  v->session_id = "session";
  v->provider = "custom";
  v->last_response_id = "response\033[31m**literal**";
  v->provider_conversation_id = "";
  v->log_path = "/literal/path/with spaces.log";
  v->effective.present = 15U;
  v->effective.model = "model";
  v->effective.reasoning_effort = "high";
  v->effective.reasoning_summary = "auto";
  v->effective.service_tier = "flex";
  v->pending.present = 1U;
  v->pending.model = "next-model";
  v->metrics.context_window_tokens = LLONG_MAX;
  v->metrics.context_used_tokens = -1;
  v->metrics.has_context_usage = 1;
  v->metrics.turn_active = 1;
  v->metrics.turn_elapsed_ms = ULLONG_MAX;
  v->metrics.has_last_turn = 1;
  v->metrics.last_turn_duration_ms = 4294967296ULL;
  v->metrics.last_turn_finished_unix_seconds = LLONG_MIN;
  v->metrics.session_usage.usage.input_tokens = LLONG_MAX;
  v->metrics.session_usage.usage.input_cached_tokens = 255LL;
  v->metrics.session_usage.usage.output_tokens = 24LL;
  v->metrics.session_usage.usage.output_reasoning_tokens = 23LL;
  v->metrics.session_usage.usage.total_tokens = 65536LL;
  v->metrics.session_usage.estimated_spend_usd = DBL_MAX;
  v->metrics.session_usage.estimated_spend_eur = -0.0;
  v->metrics.session_usage.limit_exceeded = 1;
  v->goal.has_goal = v->goal.has_token_budget = 1;
  v->goal.status = "active";
  v->goal.token_budget = LLONG_MAX;
  v->goal.tokens_used = 65535LL;
  v->goal.remaining_tokens = 256LL;
  v->goal.elapsed_seconds = 255LL;
  v->goal.created_at = -24LL;
  v->goal.updated_at = -65536LL;
  v->quota.has_short_window = 1;
  v->quota.short_window_seconds = 18000LL;
  v->quota.short_window_remaining_percent = 12.3456789012345;
  v->quota.has_weekly = 1;
  v->quota.weekly_remaining_percent = 99.9999;
  v->quota.has_credit_balance = v->quota.credits_unlimited = 1;
  v->quota.credit_balance = 58816.38;
  v->yolo = v->foreground = 1;
  v->yield_time_ms = 30000LL;
}
static void roundtrips(void) {
  fixture f;
  cai_app_view original, copy;
  const cai_app_view *got;
  char *large = malloc(CAI_WIRE_MAX_RECORD + 1U);
  size_t i;
  assert(large != NULL);
  memset(large, 'x', CAI_WIRE_MAX_RECORD);
  large[CAI_WIRE_MAX_RECORD] = '\0';
  populate(&original);
  for (i = 0U; i < 8U; ++i) {
    open_fixture(&f);
    if (i == 1U)
      original.effective.model = large; /* Graceful, not terminal. */
    if (i == 2U) {
      large[100000U] = '\0';
      original.effective.model = large; /* More than one DATA chunk. */
    }
    if (i == 3U) {
      original.pending.model = large; /* Combined metadata exceeds frame. */
    }
    if (i == 4U) {
      populate(&original);
      original.effective.present = 16U; /* Invalid mask -> unavailable. */
    }
    if (i == 5U) {
      populate(&original);
      original.session_id = large; /* Native identity cap. */
    }
    if (i == 6U) {
      memset(&original, 0, sizeof(original));
      original.available = 1;
      original.revision = 1U;
    }
    if (i == 7U) {
      original.available = 0;
      original.code = CAI_ERR_NOMEM;
      strcpy(original.reason, "explicitly unavailable");
    }
    assert(send(&f, &original) == CAI_OK && f.complete != NULL);
    got = f.complete;
    assert(got->revision == original.revision &&
           got->conversation == original.conversation);
    if (i == 1U || i == 3U || i == 4U || i == 5U)
      assert(!got->available &&
             got->code == (i == 4U ? CAI_ERR_INVALID : CAI_ERR_LIMIT));
    else {
      assert(cai_cli_status_equal(&original, got));
      if (i == 0U) {
        assert(got->metrics.context_used_tokens == -1 &&
               got->metrics.last_turn_finished_unix_seconds == LLONG_MIN &&
               got->metrics.turn_elapsed_ms == ULLONG_MAX &&
               got->goal.elapsed_seconds == 255LL &&
               got->metrics.session_usage.estimated_spend_usd == DBL_MAX &&
               got->quota.credit_balance == 58816.38 &&
               strcmp(got->last_response_id, original.last_response_id) == 0 &&
               got->pending.reasoning_effort == NULL &&
               got->provider_conversation_id[0] == '\0');
      }
      memset(&copy, 0, sizeof(copy));
      assert(cai_cli_status_copy(&copy, got, NULL) == CAI_OK);
      assert(cai_cli_status_equal(&copy, got));
      cai_cli_status_take(f.decoder, &copy);
      assert(cai_cli_status_equal(&copy, got) == 0); /* Source moved empty. */
      cai_cli_status_cleanup(&copy);
    }
    close_fixture(&f);
  }
  free(large);
}
static void clocks_and_changes(void) {
  cai_app_view a, b;
  unsigned long long turn;
  long long goal;
  populate(&a);
  a.received_ms = 1000U;
  a.metrics.turn_elapsed_ms = 20U;
  b = a;
  b.metrics.turn_elapsed_ms = 99U;
  b.goal.elapsed_seconds = 999LL;
  assert(cai_cli_status_equal(&a, &b));
  b.metrics.turn_active = 0;
  assert(!cai_cli_status_equal(&a, &b));
  b = a;
  b.pending.model = "different";
  assert(!cai_cli_status_equal(&a, &b));
  cai_app_view_elapsed(&a, 3500U, &turn, &goal);
  assert(turn == 2520U && goal == 257LL);
  a.stale = 1;
  cai_app_view_elapsed(&a, UINT64_MAX, &turn, &goal);
  assert(turn == 20U && goal == 255LL);
  a.stale = 0;
  cai_app_view_elapsed(&a, 999U, &turn, &goal);
  assert(turn == 20U && goal == 255LL);
  a.metrics.turn_active = 0;
  a.goal.status = "complete";
  cai_app_view_elapsed(&a, UINT64_MAX, &turn, &goal);
  assert(turn == 20U && goal == 255LL);
  a.metrics.turn_active = 1;
  a.goal.status = "active";
  a.metrics.turn_elapsed_ms = ULLONG_MAX - 1U;
  a.goal.elapsed_seconds = LLONG_MAX - 1LL;
  cai_app_view_elapsed(&a, 4000U, &turn, &goal);
  assert(turn == ULLONG_MAX && goal == LLONG_MAX);
  cai_app_view_elapsed(NULL, 4000U, &turn, &goal);
  assert(turn == 0U && goal == 0LL);
}
static void failures(void) {
  fixture f;
  cai_app_view v;
  cai_protocol_header h;
  cai_protocol_event e;
  const cai_app_view *view = NULL;
  cai_cbor_token t;
  size_t i;
  static const uint64_t nonfinite[] = {
      0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000001ULL};
  populate(&v);
  open_fixture(&f);
  f.reject = 1;
  assert(send(&f, &v) == CAI_ERR_CANCELLED && f.complete == NULL);
  close_fixture(&f);
  open_fixture(&f);
  f.incomplete = 1;
  assert(send(&f, &v) == CAI_OK && f.complete == NULL);
  close_fixture(&f); /* Partial frame never publishes; all strings reclaimed. */
  for (i = 0U; i < sizeof(nonfinite) / sizeof(nonfinite[0]); ++i) {
    open_fixture(&f);
    memcpy(&v.quota.credit_balance, &nonfinite[i], sizeof(double));
    assert(send(&f, &v) == CAI_OK && f.complete != NULL &&
           !f.complete->available && f.complete->code == CAI_ERR_INVALID);
    close_fixture(&f);
  }
  populate(&v);
  for (i = 1U; i <= 8U; ++i) {
    open_fixture(&f);
    f.fault = (int)i;
    assert(send(&f, &v) == CAI_ERR_PROTOCOL && f.complete == NULL);
    close_fixture(&f);
  }
  for (i = 0U; i < 8U; ++i) {
    open_fixture(&f);
    memset(&h, 0, sizeof(h));
    h.kind = CAI_PROTOCOL_STATUS;
    h.sequence = 1U;
    h.attachment = h.generation = 1U;
    if (i == 0U)
      h.sequence = 0U;
    if (i == 1U)
      h.request = 1U;
    if (i == 2U)
      h.generation = 0U;
    if (i == 3U)
      h.attachment = 0U;
    if (i == 4U)
      h.stream = 1U;
    if (i == 5U)
      h.kind = CAI_PROTOCOL_RESULT;
    e.header = &h;
    e.type = CAI_PROTOCOL_HEADER;
    e.token = NULL;
    if (i < 6U)
      assert(cai_cli_status_decode(f.decoder, &e, &view, NULL) ==
             CAI_ERR_PROTOCOL);
    else {
      assert(cai_cli_status_decode(f.decoder, &e, &view, NULL) == CAI_OK);
      e.type = CAI_PROTOCOL_PAYLOAD;
      memset(&t, 0, sizeof(t));
      t.type = CAI_CBOR_ARRAY;
      t.argument = 2U;
      t.indefinite = i == 6U;
      e.token = &t;
      if (i == 6U)
        assert(cai_cli_status_decode(f.decoder, &e, &view, NULL) ==
               CAI_ERR_PROTOCOL);
      else {
        assert(cai_cli_status_decode(f.decoder, &e, &view, NULL) == CAI_OK);
        e.type = CAI_PROTOCOL_COMPLETE;
        assert(cai_cli_status_decode(f.decoder, &e, &view, NULL) ==
               CAI_ERR_PROTOCOL);
      }
    }
    assert(view == NULL);
    close_fixture(&f);
  }
}
static void *run(void *context) {
  (void)context;
  roundtrips();
  clocks_and_changes();
  failures();
  return NULL;
}
int main(void) {
  pthread_t worker;
  assert(pthread_create(&worker, NULL, run, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
