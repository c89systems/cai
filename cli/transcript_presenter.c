#include "transcript_presenter.h"
#include "error.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum presentation_kind {
  PRESENT_SKIP = 0,
  PRESENT_PROMPT,
  PRESENT_DELIVERY,
  PRESENT_TEXT,
  PRESENT_REASONING,
  PRESENT_REASONING_START,
  PRESENT_TEXT_END
} presentation_kind;

/** Prefix state never retains the accepted prompt. The delivery's source
 * identity is bounded and discarded here; authority/deduplication belong to
 * the backend. The final suffix goes directly to the literal presenter. */
struct cai_cli_transcript_presenter {
  cai_cli_presenter *presenter;
  cai_cli_reasoning reasoning;
  presentation_kind kind;
  uint64_t stream, sequence, offset, timestamp_ns;
  unsigned long long decimal;
  size_t source_left;
  int active, failed, has_data, delivery_stage, digits, body;
};

static int failure(cai_cli_transcript_presenter *p, cai_error *error, int code,
                   const char *message) {
  p->failed = code;
  return cai_cli_error(error, code, message);
}

static presentation_kind kind_for(const char *type) {
  if (strcmp(type, "input_queue_delivery") == 0)
    return PRESENT_DELIVERY;
  if (strcmp(type, "turn_submitted") == 0 || strcmp(type, "turn_queued") == 0 ||
      strcmp(type, "steering_queued") == 0 ||
      strcmp(type, "goal_continuation") == 0 ||
      strcmp(type, "policy_diagnostic") == 0 ||
      strcmp(type, "subagent_input") == 0)
    return PRESENT_PROMPT;
  if (strcmp(type, "assistant_text_delta") == 0)
    return PRESENT_TEXT;
  if (strcmp(type, "reasoning_summary") == 0)
    return PRESENT_REASONING;
  if (strcmp(type, "reasoning_summary_started") == 0)
    return PRESENT_REASONING_START;
  if (strcmp(type, "assistant_text_end") == 0)
    return PRESENT_TEXT_END;
  return PRESENT_SKIP;
}

/** Match the runtime's input_queue_delivery grammar a byte at a time:
 * scope LF source-length LF source LF sequence LF kind LF actual-text.
 * Decimal overflow, empty source/identity/body and truncated headers fail
 * before an input can be completed. No transport boundary is semantic. */
static int delivery(cai_cli_transcript_presenter *p, const void *data,
                    size_t length) {
  const unsigned char *bytes = data;
  size_t i;
  for (i = 0U; i < length; ++i) {
    unsigned char ch = bytes[i];
    if (p->delivery_stage == 8) {
      p->body = 1;
      return cai_cli_presenter_input_write(p->presenter, bytes + i,
                                           length - i) == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    }
    switch (p->delivery_stage) {
    case 0:
      if (ch != 'L' && ch != 'C')
        return CAI_ERR_PROTOCOL;
      p->delivery_stage = 1;
      break;
    case 1:
      if (ch != '\n')
        return CAI_ERR_PROTOCOL;
      p->delivery_stage = 2;
      break;
    case 2:
    case 5:
      if (ch >= '0' && ch <= '9') {
        if (p->digits == 20 || p->decimal > (ULLONG_MAX - (ch - '0')) / 10ULL)
          return CAI_ERR_PROTOCOL;
        p->decimal = p->decimal * 10ULL + (ch - '0');
        ++p->digits;
      } else if (ch == '\n' && p->digits != 0 && p->decimal != 0U) {
        if (p->delivery_stage == 2) {
          if (p->decimal > 128U)
            return CAI_ERR_PROTOCOL;
          p->source_left = (size_t)p->decimal;
          p->delivery_stage = 3;
        } else
          p->delivery_stage = 6;
        p->digits = 0;
        p->decimal = 0U;
      } else
        return CAI_ERR_PROTOCOL;
      break;
    case 3:
      if (ch == 0U)
        return CAI_ERR_PROTOCOL;
      if (--p->source_left == 0U)
        p->delivery_stage = 4;
      break;
    case 4:
      if (ch != '\n')
        return CAI_ERR_PROTOCOL;
      p->delivery_stage = 5;
      break;
    case 6:
      if (ch < '0' || ch > '2')
        return CAI_ERR_PROTOCOL;
      p->delivery_stage = 7;
      break;
    case 7:
      if (ch != '\n' || cai_cli_presenter_input_begin(p->presenter, 0) != 0)
        return CAI_ERR_PROTOCOL;
      p->delivery_stage = 8;
      break;
    default:
      return CAI_ERR_PROTOCOL;
    }
  }
  return CAI_OK;
}

static int event_body(cai_cli_transcript_presenter *p,
                      const cai_transcript_event *event, cai_error *error) {
  cai_cli_presenter *ui = p->presenter;
  int rc = 0;
  if (p->failed)
    return cai_cli_error(error, p->failed,
                         "transcript presenter requires reset");
  if (event->kind == CAI_TRANSCRIPT_ERROR) {
    cai_cli_transcript_presenter_reset(p);
    return CAI_OK; /* The frontend handles this typed subscription notice. */
  }
  if (event->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    cai_cli_transcript_presenter_reset(p);
    p->stream = event->stream;
    return CAI_OK;
  }
  if (event->kind == CAI_TRANSCRIPT_REPLAY_END) {
    if (p->active)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "replay ended inside a transcript event");
    /* Do not finish Markdown: a live delta can continue the document captured
     * at the replay watermark without an artificial block boundary. */
    return CAI_OK;
  }
  if (event->kind == CAI_TRANSCRIPT_EVENT_BEGIN) {
    if (p->active || event->type == NULL || event->sequence == 0U ||
        event->stream == 0U || (p->stream != 0U && p->stream != event->stream))
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "invalid transcript presentation begin");
    p->stream = event->stream;
    p->sequence = event->sequence;
    p->timestamp_ns = event->timestamp_ns;
    p->kind = kind_for(event->type);
    p->offset = 0U;
    p->active = 1;
    p->has_data = event->has_data;
    p->delivery_stage = p->digits = p->body = 0;
    p->source_left = 0U;
    p->decimal = 0U;
    if (p->kind == PRESENT_PROMPT)
      rc = cai_cli_presenter_input_begin(ui, 0);
  } else if (!p->active || event->sequence != p->sequence ||
             event->stream != p->stream) {
    return failure(p, error, CAI_ERR_PROTOCOL,
                   "transcript presentation identity changed");
  } else if (event->kind == CAI_TRANSCRIPT_DATA) {
    if (!p->has_data || event->offset != p->offset ||
        (event->data == NULL && event->length != 0U) ||
        event->length > UINT64_MAX - p->offset)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "invalid transcript presentation offset");
    if (p->kind == PRESENT_DELIVERY) {
      int code = delivery(p, event->data, event->length);
      if (code != CAI_OK)
        return failure(p, error, code,
                       "cannot present accepted input delivery");
    } else if (p->kind == PRESENT_PROMPT)
      rc = cai_cli_presenter_input_write(ui, event->data, event->length);
    else if (p->kind == PRESENT_TEXT)
      rc = cai_cli_presenter_text(ui, event->data, event->length);
    else if (p->kind == PRESENT_REASONING)
      rc = cai_cli_reasoning_feed(&p->reasoning, event->data, event->length,
                                  cai_cli_presenter_reasoning_body, ui);
    p->offset += event->length;
  } else if (event->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (event->offset != p->offset)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "transcript presentation length changed");
    if (p->kind == PRESENT_DELIVERY && (p->delivery_stage != 8 || !p->body))
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "truncated accepted input delivery");
    if (p->kind == PRESENT_PROMPT || p->kind == PRESENT_DELIVERY)
      rc = cai_cli_presenter_input_end(ui);
    else if (p->kind == PRESENT_REASONING_START ||
             p->kind == PRESENT_TEXT_END) {
      rc = cai_cli_presenter_finish_response(ui);
      if (rc == 0)
        rc = cai_cli_presenter_finish_reasoning(ui);
      if (p->kind == PRESENT_REASONING_START)
        cai_cli_reasoning_reset(&p->reasoning);
      else
        cai_cli_reasoning_end(&p->reasoning);
    }
    p->active = 0;
  } else
    return failure(p, error, CAI_ERR_PROTOCOL,
                   "unknown transcript presentation operation");
  return rc == 0 ? CAI_OK
                 : failure(p, error, CAI_ERR_TRANSPORT,
                           "cannot render transcript entry");
}

int cai_cli_transcript_presenter_event(void *context,
                                       const cai_transcript_event *event,
                                       cai_error *error) {
  cai_cli_transcript_presenter *p = context;
  unsigned long long saved;
  int scoped, rc;
  if (p == NULL || event == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "transcript presentation event required");
  saved = p->presenter->presentation_timestamp_ns;
  scoped = p->presenter->presentation_event;
  p->presenter->presentation_timestamp_ns =
      event->kind == CAI_TRANSCRIPT_EVENT_BEGIN ? event->timestamp_ns
                                                : p->timestamp_ns;
  p->presenter->presentation_event = 1;
  rc = event_body(p, event, error);
  p->presenter->presentation_timestamp_ns = saved;
  p->presenter->presentation_event = scoped;
  return rc;
}

int cai_cli_transcript_presenter_open(cai_cli_presenter *presenter,
                                      cai_cli_transcript_presenter **out,
                                      cai_error *error) {
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "transcript presenter output required");
  *out = NULL;
  if (presenter == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "presenter required");
  *out = calloc(1U, sizeof(**out));
  if (*out == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "open transcript presenter");
  (*out)->presenter = presenter;
  return CAI_OK;
}

void cai_cli_transcript_presenter_reset(cai_cli_transcript_presenter *p) {
  cai_cli_presenter *ui;
  if (p == NULL)
    return;
  ui = p->presenter;
  cai_cli_presenter_input_abort(ui);
  if (ui->response_open || ui->reasoning_open) {
    if (ui->renderer != NULL)
      (void)mdf_reset(ui->renderer);
    ui->response_open = ui->reasoning_open = ui->documents_rendered = 0;
    ui->block_spacing_pending = 1;
    cai_cli_reasoning_quote_init(&ui->reasoning_quote);
  }
  memset(p, 0, sizeof(*p));
  p->presenter = ui;
}

void cai_cli_transcript_presenter_close(cai_cli_transcript_presenter *p) {
  if (p != NULL) {
    cai_cli_transcript_presenter_reset(p);
    free(p);
  }
}
