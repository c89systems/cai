#include "plain_feed.h"
#include "error.h"
#include "replay_tools.h"
#include "softline_presenter.h"
#include "tool_result.h"
#include "transcript_presenter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct cai_cli_plain_feed {
  cai_cli_presenter ui;
  cai_cli_transcript_presenter *transcript;
  cai_cli_replay_tools *tools;
  cai_cli_tool_text diagnostic;
  cai_sink *sink;
  int mode;
};
static int markdown(void *context, const char *bytes, size_t length) {
  cai_cli_plain_feed *f = context;
  return cai_cli_presenter_tool_markdown(&f->ui, bytes, length);
}
static int sink_write(void *context, const void *bytes, size_t length,
                      cai_error *error) {
  return markdown(context, bytes, length) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "render plain output");
}
int cai_cli_plain_feed_finish(cai_cli_plain_feed *f, cai_error *error) {
  return cai_cli_presenter_finish_response(&f->ui) == 0 &&
                 cai_cli_presenter_finish_reasoning(&f->ui) == 0 &&
                 fflush(stdout) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "finish plain output");
}
int cai_cli_plain_feed_open(const cai_cli_options *o, cai_cli_plain_feed **out,
                            cai_error *error) {
  cai_cli_plain_feed *f = calloc(1U, sizeof(*f));
  cai_sink_callbacks callbacks = {0};
  int rc;
  *out = NULL;
  if (f == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate plain presenter");
  f->ui.batch = 1; /* Direct stdout sink and no displayed input. */
  f->ui.width = 80;
  f->ui.tool_output = o->tool_output;
  f->ui.utc = o->utc;
  f->mode = o->tool_output != NULL && strcmp(o->tool_output, "full") == 0
                ? CAI_TOOL_OUTPUT_FULL
                : CAI_TOOL_OUTPUT_COMPACT;
  rc = cai_cli_theme_parse("plain", &f->ui.theme) == NULL ? CAI_OK
                                                          : CAI_ERR_INVALID;
  f->ui.render_theme = f->ui.theme;
  if (rc == CAI_OK)
    rc = cai_cli_presenter_renderer_create(&f->ui, &f->ui.theme,
                                           &f->ui.renderer, error);
  if (rc == CAI_OK)
    rc = cai_cli_presenter_tool_display_init(&f->ui, error);
  if (rc == CAI_OK)
    rc = cai_cli_transcript_presenter_open(&f->ui, &f->transcript, error);
  callbacks.context = f;
  callbacks.write = sink_write;
  if (rc == CAI_OK)
    rc = cai_sink_from_callbacks(&callbacks, &f->sink, error);
  if (rc == CAI_OK)
    *out = f;
  else
    cai_cli_plain_feed_close(f);
  return rc;
}
int cai_cli_plain_feed_transcript(cai_cli_plain_feed *f,
                                  const cai_transcript_event *event,
                                  cai_error *error) {
  int rc;
  if (event->kind == CAI_TRANSCRIPT_TERMINAL_OUTPUT) {
    rc = cai_cli_plain_feed_finish(f, error);
    if (rc == CAI_OK)
      rc =
          cai_cli_replay_tools_live(f->tools, event, f->ui.tool_display, error);
  } else
    rc = cai_cli_transcript_presenter_event(f->transcript, event, error);
  if (rc == CAI_OK && fflush(stdout) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write plain transcript");
  return rc;
}
int cai_cli_plain_feed_notice(cai_cli_plain_feed *f, const char *text,
                              cai_error *error) {
  int rc = cai_cli_plain_feed_finish(f, error);
  if (rc == CAI_OK) {
    cai_cli_tool_text_init(&f->diagnostic, 1, 72U);
    rc = cai_cli_tool_text_write(&f->diagnostic, text, strlen(text), 1, 1,
                                 markdown, f, error);
  }
  return rc == CAI_OK ? cai_cli_plain_feed_finish(f, error) : rc;
}
int cai_cli_plain_feed_execution(cai_cli_plain_feed *f,
                                 const cai_app_execution_event *event,
                                 cai_error *error) {
  const cai_execution_record *r = event->record;
  size_t n;
  int rc = CAI_OK;
  if (r == NULL)
    return cai_cli_error(error, CAI_ERR_PROTOCOL, "invalid plain execution");
  n = strlen(r->data);
  if (strncmp(r->kind, "tool.", 5U) == 0 ||
      strcmp(r->kind, "hosted.tool") == 0 ||
      strcmp(r->kind, "reasoning.receipt") == 0) {
    int handled = 0;
    if (cai_cli_replay_tools_command_output(r))
      rc = cai_cli_plain_feed_finish(f, error);
    if (rc == CAI_OK)
      rc =
          cai_cli_replay_tools_follow(&f->tools, r, f->ui.tool_display, f->mode,
                                      72U, markdown, f, &handled, error);
  } else if (strcmp(r->kind, "command.markdown") == 0 ||
             strcmp(r->kind, "application.markdown") == 0 ||
             (event->subagent &&
              strcmp(r->kind, "review_markdown_delta") == 0)) {
    rc = sink_write(f, r->data, n, error);
    if (rc == CAI_OK && r->final)
      rc = cai_cli_plain_feed_finish(f, error);
  } else if (strcmp(r->kind, "command.failed") == 0 && r->field != NULL) {
    if (r->offset == 0U)
      cai_cli_tool_text_init(&f->diagnostic, 1, 72U);
    rc = cai_cli_tool_text_write(&f->diagnostic, r->data, n, r->final, 1,
                                 markdown, f, error);
    if (rc == CAI_OK && r->final)
      rc = cai_cli_plain_feed_finish(f, error);
  } else if (event->subagent && strcmp(r->kind, "assistant_text_delta") == 0 &&
             (r->subagent == NULL || strcmp(r->subagent, "review") != 0))
    rc = cai_cli_presenter_text(&f->ui, r->data, n) == 0 ? CAI_OK
                                                         : CAI_ERR_TRANSPORT;
  else if (event->subagent && strcmp(r->kind, "reasoning_summary") == 0)
    rc = cai_cli_presenter_reasoning_body(&f->ui, r->data, n) == 0
             ? CAI_OK
             : CAI_ERR_TRANSPORT;
  else if (event->subagent &&
           (strcmp(r->kind, "assistant_text_end") == 0 ||
            strcmp(r->kind, "reasoning_summary_started") == 0))
    rc = cai_cli_plain_feed_finish(f, error);
  else if (strcmp(r->kind, "request.failed") == 0 ||
           strcmp(r->kind, "turn.failed") == 0 ||
           strcmp(r->kind, "compaction.failed") == 0 ||
           strcmp(r->kind, "request.retry") == 0 ||
           strcmp(r->kind, "compaction.started") == 0 ||
           strcmp(r->kind, "compaction.completed") == 0 ||
           strcmp(r->kind, "turn.cancelled") == 0) {
    rc = cai_execution_record_write_markdown(r, 0, f->sink, error);
    if (rc == CAI_OK && r->final)
      rc = cai_cli_plain_feed_finish(f, error);
  }
  if (rc == CAI_OK && fflush(stdout) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write plain execution");
  return rc;
}
void cai_cli_plain_feed_close(cai_cli_plain_feed *f) {
  if (f == NULL)
    return;
  cai_cli_replay_tools_close(f->tools);
  cai_cli_transcript_presenter_close(f->transcript);
  cai_cli_presenter_close(&f->ui);
  cai_sink_close(f->sink);
  free(f);
}
