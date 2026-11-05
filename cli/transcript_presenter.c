#include "transcript_presenter.h"
#include "error.h"
#include "shell_text.h"
#include <stdlib.h>
#include <string.h>

typedef enum presentation_kind {
  PRESENT_SKIP = 0,
  PRESENT_PROMPT,
  PRESENT_TEXT,
  PRESENT_REASONING,
  PRESENT_REASONING_START,
  PRESENT_TEXT_END,
  PRESENT_SHELL_COMMAND,
  PRESENT_SHELL_TAIL,
  PRESENT_SHELL_HEX,
  PRESENT_SHELL_END
} presentation_kind;

/** Consumption projections go directly to the literal presenter. Admission
 * and recovery metadata never become displayed input; no pending prompt body
 * or provenance lookup is retained by the frontend. */
struct cai_cli_transcript_presenter {
  cai_cli_presenter *presenter;
  cai_cli_reasoning reasoning;
  presentation_kind kind;
  uint64_t stream, sequence, offset, timestamp_ns;
  int active, failed, has_data;
  cai_cli_shell_text shell;
  int hex_digit, skip_shell_end;
  uint64_t skip_shell_first, skip_shell_last;
  uint64_t shell_anchor, live_offset;
  int live_seen, live_complete, shell_echo;
};

static int failure(cai_cli_transcript_presenter *p, cai_error *error, int code,
                   const char *message) {
  p->failed = code;
  return cai_cli_error(error, code, message);
}

static presentation_kind kind_for(const char *type) {
  if (strcmp(type, "input_presented") == 0 ||
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
  if (strcmp(type, "user_shell_started") == 0)
    return PRESENT_SHELL_COMMAND;
  if (strcmp(type, "user_shell_output_tail") == 0)
    return PRESENT_SHELL_TAIL;
  if (strcmp(type, "user_shell_output_hex") == 0)
    return PRESENT_SHELL_HEX;
  if (strcmp(type, "user_shell_completed") == 0 ||
      strcmp(type, "user_shell_unshared") == 0)
    return PRESENT_SHELL_END;
  return PRESENT_SKIP;
}

static int shell_output(void *context, const char *bytes, size_t length,
                        cai_error *error) {
  return cai_cli_presenter_sink(context, bytes, length) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "display stored shell output");
}
static int shell_hex(cai_cli_transcript_presenter *p, const void *bytes,
                     size_t length, cai_error *error) {
  const unsigned char *data = bytes;
  char decoded[512];
  size_t i, used = 0U;
  for (i = 0U; i < length; ++i) {
    unsigned int digit;
    int rc;
    if (data[i] >= '0' && data[i] <= '9')
      digit = data[i] - '0';
    else if (data[i] >= 'a' && data[i] <= 'f')
      digit = data[i] - 'a' + 10U;
    else if (data[i] >= 'A' && data[i] <= 'F')
      digit = data[i] - 'A' + 10U;
    else
      return CAI_ERR_PROTOCOL;
    if (p->hex_digit < 0)
      p->hex_digit = (int)digit;
    else {
      decoded[used++] = (char)((unsigned int)p->hex_digit * 16U + digit);
      p->hex_digit = -1;
      if (used == sizeof(decoded)) {
        rc = cai_cli_shell_plain(&p->shell, decoded, used, 0, shell_output,
                                 p->presenter, error);
        if (rc != CAI_OK)
          return rc;
        used = 0U;
      }
    }
  }
  return cai_cli_shell_plain(&p->shell, decoded, used, 0, shell_output,
                             p->presenter, error);
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
    uint64_t first = p->skip_shell_first, last = p->skip_shell_last;
    cai_cli_transcript_presenter_reset(p);
    p->skip_shell_first = first;
    p->skip_shell_last = last;
    p->stream = event->stream;
    return CAI_OK;
  }
  if (event->kind == CAI_TRANSCRIPT_REPLAY_END) {
    if (p->active)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "replay ended inside a transcript event");
    /* Do not finish Markdown: a live delta can continue the document captured
     * at the replay watermark without an artificial block boundary. */
    p->skip_shell_first = p->skip_shell_last = 0U;
    return CAI_OK;
  }
  if (event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT ||
      event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END) {
    if (p->active || p->stream != event->stream ||
        event->sequence != p->shell_anchor || p->live_complete ||
        event->offset != p->live_offset ||
        event->length > UINT64_MAX - p->live_offset)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "live shell presentation identity changed");
    if (!p->live_seen) {
      rc = cai_cli_presenter_finish_response(ui);
      if (rc == 0)
        rc = cai_cli_presenter_finish_reasoning(ui);
    }
    if (rc == 0)
      rc = cai_cli_shell_plain(&p->shell, event->data, event->length,
                               event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END,
                               shell_output, ui, error);
    if (rc != 0)
      return failure(p, error, rc, "cannot present live shell output");
    p->live_seen = 1;
    p->live_offset += event->length;
    p->live_complete = event->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END;
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
    if (strcmp(event->type, "input_presented") == 0)
      cai_cli_reasoning_reset(&p->reasoning);
    if (p->kind == PRESENT_SHELL_COMMAND) {
      p->shell_anchor = event->sequence;
      p->live_offset = 0U;
      p->live_seen = p->live_complete = 0;
      if (p->shell_echo)
        p->kind = PRESENT_SKIP;
      p->shell_echo = 0;
    } else if (p->kind == PRESENT_SHELL_TAIL && p->live_seen) {
      if (p->live_complete)
        p->kind = PRESENT_SKIP;
      else {
        static const char notice[] =
            "\n**Live shell output interrupted; retained tail follows.**\n";
        rc = cai_cli_shell_plain(&p->shell, NULL, 0U, 1, shell_output, ui,
                                 error);
        if (rc == 0)
          rc = cai_cli_presenter_text(ui, notice, sizeof(notice) - 1U);
        if (rc == 0)
          rc = cai_cli_presenter_finish_response(ui);
        if (rc != 0)
          return failure(p, error, rc, "cannot restore saved shell tail");
      }
    }
    p->skip_shell_end = 0;
    if (p->kind >= PRESENT_SHELL_COMMAND && p->skip_shell_first != 0U &&
        event->sequence >= p->skip_shell_first &&
        (p->skip_shell_last == 0U || event->sequence <= p->skip_shell_last)) {
      p->skip_shell_end = p->kind == PRESENT_SHELL_END;
      p->kind = PRESENT_SKIP;
    }
    p->offset = 0U;
    p->active = 1;
    p->has_data = event->has_data;
    if (p->kind == PRESENT_PROMPT)
      rc = cai_cli_presenter_input_begin(ui, 0);
    else if (p->kind == PRESENT_SHELL_COMMAND) {
      rc = cai_cli_presenter_input_begin(ui, 1);
      if (rc == 0)
        rc = cai_cli_presenter_input_write(ui, "!", 1U);
    } else if (p->kind >= PRESENT_SHELL_TAIL) {
      rc = cai_cli_presenter_finish_response(ui);
      if (rc == 0)
        rc = cai_cli_presenter_finish_reasoning(ui);
      p->hex_digit = -1;
    }
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
    if (p->kind == PRESENT_PROMPT)
      rc = cai_cli_presenter_input_write(ui, event->data, event->length);
    else if (p->kind == PRESENT_TEXT)
      rc = cai_cli_presenter_text(ui, event->data, event->length);
    else if (p->kind == PRESENT_REASONING)
      rc = cai_cli_reasoning_feed(&p->reasoning, event->data, event->length,
                                  cai_cli_presenter_reasoning_body, ui);
    else if (p->kind == PRESENT_SHELL_COMMAND)
      rc = cai_cli_presenter_input_write(ui, event->data, event->length);
    else if (p->kind == PRESENT_SHELL_TAIL)
      rc = cai_cli_shell_plain(&p->shell, event->data, event->length, 0,
                               shell_output, ui, error);
    else if (p->kind == PRESENT_SHELL_HEX)
      rc = shell_hex(p, event->data, event->length, error);
    p->offset += event->length;
  } else if (event->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (event->offset != p->offset)
      return failure(p, error, CAI_ERR_PROTOCOL,
                     "transcript presentation length changed");
    if (p->kind == PRESENT_PROMPT || p->kind == PRESENT_SHELL_COMMAND)
      rc = cai_cli_presenter_input_end(ui);
    else if (p->kind == PRESENT_SHELL_HEX && p->hex_digit >= 0)
      return failure(p, error, CAI_ERR_PROTOCOL, "odd stored shell hex length");
    else if (p->kind == PRESENT_SHELL_TAIL || p->kind == PRESENT_SHELL_END)
      rc = cai_cli_shell_plain(&p->shell, NULL, 0U, 1, shell_output, ui, error);
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
    if (p->skip_shell_end)
      p->skip_shell_first = p->skip_shell_last = 0U;
    if (p->kind == PRESENT_SHELL_END || p->skip_shell_end) {
      p->shell_anchor = p->live_offset = 0U;
      p->live_seen = p->live_complete = 0;
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

int cai_cli_transcript_presenter_title(cai_cli_transcript_presenter *p,
                                       const char **out) {
  return cai_cli_reasoning_title(&p->reasoning, out);
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

int cai_cli_transcript_presenter_command(cai_cli_transcript_presenter *p,
                                         const char *command) {
  p->shell_echo = command[0] == '!';
  return cai_cli_presenter_input(p->presenter, command, 1);
}
void cai_cli_transcript_presenter_skip_shell(cai_cli_transcript_presenter *p,
                                             uint64_t first, uint64_t last) {
  p->skip_shell_first = first;
  p->skip_shell_last = last;
}
void cai_cli_transcript_presenter_close(cai_cli_transcript_presenter *p) {
  if (p != NULL) {
    cai_cli_transcript_presenter_reset(p);
    free(p);
  }
}
