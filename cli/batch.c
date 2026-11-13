#define _POSIX_C_SOURCE 200809L
#include "batch.h"
#include "batch_document.h"
#include "commands.h"
#include "error.h"
#include "goal_input.h"
#include "line_input.h"
#include "log.h"
#include "plain_feed.h"
#include "startup.h"
#include <cai/transcript_selection.h>
#include <errno.h>
#include <fcntl.h>
#include <libmdf/mdf.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** Fixed invocation metadata only. The server owns accepted input, model
 * context and history. Neither provisional responses nor whole tool bodies
 * are accumulated in this adapter. */
typedef struct batch {
  cai_app_client *client;
  cai_app_client_state state;
  cai_cli_startup startup;
  cai_cli_log *log;
  cai_cli_line_input *lines;
  cai_cli_plain_feed *feed;
  int input_ready, input_eof, source_quit, force_quit;
  unsigned long long plain_turns, plain_commands;
  int plain_quiet, plain_handoff_drain;
  unsigned long long plain_barrier, plain_quit;
  int hide_handoff_replay;
  char action_prefix[64];
  size_t action_prefix_used;
  unsigned long long action_offset;
  cai_transcript_position at;
  cai_transcript_selection *selection;
  cai_transcript_selection_result result;
  cai_transcript_projector *projector;
  cai_sink *sink;
  mdf *renderer;
  char *offered;
  char call[256], type[96], goal_status[32];
  uint64_t binding, subscription, generation, old_conversation;
  unsigned long long request, item;
  int awaiting_bind, subscribed, replaying, entry_open;
  int startup_select;
  int pending, command, receipt, command_done, command_code, handoff;
  int work_failed, has_result, projecting, detaching;
  int goal_command, goal_start, goal_follow, settled;
  int awaiting_application, application_settled;
  int shared_logger;
  cai_cli_batch_document command_document, review_candidate, review_json;
  cai_cli_batch_document review_markdown, output;
  cai_cli_batch_document_reader document_reader;
  char review_session[129];
  int startup_control, review_command, review_done, review_code;
  int output_document, selected_review, projecting_document;
  int json_output, header_written, last_byte;
  size_t header_offset;
  FILE *destination;
  int owns_destination;
  /** Live shell bytes are borrowed once, never reconstructed from the tail. */
  uint64_t shell_sequence, shell_offset;
  int shell_command, shell_started, shell_end, shell_done, shell_code,
      shell_last;
} batch;

int cai_cli_batch_mode(const cai_cli_options *o) {
  size_t i;
  if (o->headless || cai_cli_options_utility(o) ||
      (!o->review && !o->non_interactive && isatty(STDIN_FILENO) &&
       isatty(STDOUT_FILENO)))
    return 0;
  for (i = 0U; i < o->instruction_count; ++i) {
    const char *text = cai_cli_instruction_at(o, i);
    if (text == NULL)
      return 0;
  }
  return 1;
}
static uint64_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int observe(void *context, const cai_app_client_event *e,
                   cai_error *error) {
  batch *b = context;
  if (e->type == CAI_APP_CLIENT_BIND_REJECTED)
    return cai_cli_error(error,
                         e->input->result.code != CAI_OK ? e->input->result.code
                                                         : CAI_ERR_INVALID,
                         e->input->result.reason[0] != '\0'
                             ? e->input->result.reason
                             : "batch producer was rejected");
  if (e->type == CAI_APP_CLIENT_INPUT && e->input != NULL) {
    if (e->input->type == CAI_INPUT_OBSERVATION_DISCONNECTED &&
        !e->state->bound) {
      if (!b->handoff)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "batch session changed; outcome unknown");
      b->awaiting_bind = b->subscribed = 0;
    }
    if (e->input->type == CAI_INPUT_OBSERVATION_RESULT && b->pending &&
        e->input->result.request_id == b->request) {
      if (e->input->result.outcome != CAI_INPUT_ACCEPTED)
        return cai_cli_error(error,
                             e->input->result.code != CAI_OK
                                 ? e->input->result.code
                                 : CAI_ERR_INVALID,
                             e->input->result.reason[0] != '\0'
                                 ? e->input->result.reason
                                 : "batch input was not accepted");
      b->item = e->input->result.item_id;
      b->receipt = 1;
    }
  }
  return CAI_OK;
}
/** Retain only counts/labels, never another queue of accepted input. Ordered
 * application settlement confirms EOF drain after every accepted outcome. */
static int plain_execution(batch *b, const cai_app_execution_event *e,
                           cai_error *error) {
  const cai_execution_record *r = e->record;
  char kind[16], session[129], tail;
  unsigned long long producer, request, item;
  int owned = !e->subagent && r->call_id != NULL &&
              sscanf(r->call_id, "%15[^:]:%128[^:]:%llu:%llu:%llu%c", kind,
                     session, &producer, &request, &item, &tail) == 5 &&
              ((!e->replay && producer == b->state.producer &&
                strcmp(session, b->at.session) == 0) ||
               (b->plain_barrier != 0U && strcmp(r->call_id, b->call) == 0));
  int rc = e->replay && b->hide_handoff_replay && !owned
               ? CAI_OK
               : cai_cli_plain_feed_execution(b->feed, e, error);
  if (rc != CAI_OK)
    return rc;
  if (owned && strcmp(kind, "command") == 0) {
    if (strcmp(r->kind, "command.handoff") == 0) {
      b->handoff = 1;
      b->old_conversation = e->boundary->conversation;
      b->plain_handoff_drain = 1;
    }
    if (strcmp(r->kind, "command.settled") == 0 && r->final) {
      if (b->plain_commands == 0U)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "unmatched pipe command settlement");
      --b->plain_commands;
      if (request == b->plain_barrier)
        b->plain_barrier = 0U;
    }
    if (strcmp(r->kind, "command.frontend") == 0) {
      size_t n = strlen(r->data), keep;
      if (r->offset == 0U) {
        b->action_prefix_used = 0U;
        b->action_offset = 0U;
      }
      if (r->offset != b->action_offset || n > ULLONG_MAX - b->action_offset)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "invalid pipe frontend action");
      keep = sizeof(b->action_prefix) - 1U - b->action_prefix_used;
      if (keep > n)
        keep = n;
      memcpy(b->action_prefix + b->action_prefix_used, r->data, keep);
      b->action_prefix_used += keep;
      b->action_prefix[b->action_prefix_used] = '\0';
      b->action_offset += n;
      if (r->final) {
        int quit = cai_cli_command_quit(b->action_prefix);
        if (request == b->plain_quit || quit > 0) {
          b->input_eof = 1;
          b->force_quit = quit == 2;
        } else if (quit < 0)
          rc = cai_cli_plain_feed_notice(
              b->feed, "Usage: /quit [now|force|-f|--force] or /terminate",
              error);
        else
          rc = cai_cli_plain_feed_notice(
              b->feed,
              strncmp(b->action_prefix, "/shell", 6U) == 0 ||
                      strncmp(b->action_prefix, "!sh", 3U) == 0
                  ? "interactive shell requires terminal input and output"
                  : "This command requires an interactive terminal attachment.",
              error);
      }
    }
  } else if (owned && strcmp(kind, "input") == 0 &&
             strcmp(r->kind, "turn.settled") == 0 && r->final) {
    if (b->plain_turns == 0U)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "unmatched pipe turn settlement");
    --b->plain_turns;
  }
  if ((!e->replay || b->plain_handoff_drain) && !e->subagent && !b->pending &&
      b->plain_turns == 0U && b->plain_commands == 0U &&
      strcmp(r->kind, "application.settled") == 0 && r->field == NULL &&
      r->final)
    b->plain_quiet = 1;
  return rc;
}
static int execution(void *context, const cai_app_execution_event *e,
                     cai_error *error) {
  batch *b = context;
  const cai_execution_record *r = e->record;
  if (b->projecting)
    return b->projecting_document
               ? cai_cli_batch_document_execution(&b->document_reader, e, error)
               : CAI_OK;
  if (r == NULL)
    return cai_cli_error(error, e->code != CAI_OK ? e->code : CAI_ERR_PROTOCOL,
                         e->reason != NULL ? e->reason
                                           : "invalid execution history");
  if (b->feed != NULL)
    return plain_execution(b, e, error);
  if (b->pending && (b->command || b->shell_command) && !e->subagent &&
      r->call_id != NULL && strcmp(r->call_id, b->call) == 0) {
    if (b->shell_command && strcmp(r->kind, "command.shell_started") == 0)
      b->shell_started = 1;
    if (b->shell_command && strcmp(r->kind, "command.shell_settled") == 0 &&
        r->final) {
      b->shell_done = 1;
      b->shell_code = (int)r->code;
    }
    if (strcmp(r->kind, "command.handoff") == 0) {
      b->handoff = 1;
      b->old_conversation = e->boundary->conversation;
      cai_cli_batch_document_reset(&b->command_document);
    }
    if (r->final && strcmp(r->kind, "command.settled") == 0) {
      b->command_done = 1;
      b->command_code = (int)r->code;
    }
    if (strcmp(r->kind, "command.frontend") == 0)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "this command requires an interactive terminal attachment");
  }
  if (b->pending && b->command_document.kind[0] != '\0') {
    int rc =
        cai_cli_batch_document_capture(&b->command_document, &b->at, e, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (b->pending && b->review_command && r->parent_call_id != NULL &&
      strcmp(r->parent_call_id, b->call) == 0 && r->subagent != NULL &&
      strcmp(r->subagent, "review") == 0 &&
      strcmp(r->session_id, b->at.session) != 0) {
    int rc;
    if (b->review_session[0] == '\0') {
      snprintf(b->review_session, sizeof(b->review_session), "%s",
               r->session_id);
      rc = cai_cli_batch_document_init(&b->review_candidate,
                                       "assistant_text_delta",
                                       b->review_session, NULL, b->call, error);
      if (rc == CAI_OK)
        rc = cai_cli_batch_document_init(
            &b->review_markdown, "review_markdown_delta", b->review_session,
            NULL, b->call, error);
      if (rc != CAI_OK)
        return rc;
    }
    if (strcmp(r->session_id, b->review_session) == 0) {
      if (strcmp(r->kind, "request.retry") == 0 && r->offset == 0U &&
          (r->field == NULL || strcmp(r->field, "message") == 0))
        cai_cli_batch_document_reset(&b->review_candidate);
      rc = cai_cli_batch_document_capture(&b->review_candidate, &b->at, e,
                                          error);
      if (rc == CAI_OK)
        rc = cai_cli_batch_document_capture(&b->review_markdown, &b->at, e,
                                            error);
      if (rc != CAI_OK)
        return rc;
      if (strcmp(r->kind, "response.completed") == 0 && r->field == NULL) {
        b->review_json = b->review_candidate;
        cai_cli_batch_document_reset(&b->review_candidate);
      }
    }
  }
  if (b->pending && b->review_command && !e->subagent && r->call_id != NULL &&
      strcmp(r->call_id, b->call) == 0 &&
      strcmp(r->kind, "command.review_settled") == 0) {
    b->review_done = 1;
    b->review_code = (int)r->code;
  }
  if (b->selection != NULL && strcmp(r->kind, "turn.settled") == 0 &&
      strcmp(r->session_id, b->at.session) == 0 &&
      ((b->goal_follow && r->call_id == NULL) ||
       (!b->goal_follow && r->call_id != NULL &&
        strcmp(r->call_id, b->call) == 0))) {
    if (!r->final || r->offset != 0U || r->field == NULL ||
        strcmp(r->field, "goal_status") != 0 ||
        strlen(r->data) >= sizeof(b->goal_status))
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "invalid batch turn settlement");
    snprintf(b->goal_status, sizeof(b->goal_status), "%s", r->data);
    b->settled = 1;
  }
  /* An earlier background turn may settle after the goal's OFFER receipt but
   * before that command applies. Only its actual command outcome releases
   * this dependency, including when both records arrive in the same pump. */
  if ((b->awaiting_application || (b->pending && b->goal_command)) &&
      (!b->goal_command || !b->command || b->command_done) &&
      strcmp(r->kind, "application.settled") == 0 &&
      strcmp(r->session_id, b->at.session) == 0 && r->field == NULL && r->final)
    b->application_settled = 1;
  if (!b->shared_logger &&
      (r->data[0] != '\0' ||
       (r->field == NULL && strncmp(r->kind, "tool.", 5U) == 0)) &&
      strcmp(r->kind, "turn.settled") != 0) {
    pslog_field fields[7];
    const char *name = r->kind;
    const char *role = "runtime";
    const char *text = r->data;
    char status[48];
    fields[0] = pslog_str("tool_call_id", r->call_id);
    fields[1] = pslog_str("field", r->field);
    fields[2] = pslog_i64("code", r->code);
    snprintf(status, sizeof(status), "HTTP %lld", r->http_status);
    fields[3] = pslog_str("http", r->http_status > 0 ? status : "");
    fields[4] = pslog_str("tool", r->tool_name);
    fields[5] = pslog_str("session_id", r->session_id);
    fields[6] = pslog_str("parent_tool_call_id", r->parent_call_id);
    if (strncmp(r->kind, "tool.", 5U) == 0) {
      role = "tool";
      if (r->field == NULL) {
        if (strcmp(r->kind, "tool.started") == 0)
          name = "tool_call_started";
        else if (strcmp(r->kind, "tool.completed") == 0)
          name = "tool_call_completed";
        else if (strcmp(r->kind, "tool.failed") == 0)
          name = "tool_call_failed";
        text = r->tool_name != NULL ? r->tool_name : name;
      }
    }
    cai_cli_log_message(b->log->logger,
                        r->code != CAI_OK ? PSLOG_LEVEL_ERROR
                                          : PSLOG_LEVEL_INFO,
                        name, role, text, strlen(text), fields, 7U);
  }
  return CAI_OK;
}
static int transcript(void *context, const cai_transcript_event *e,
                      cai_error *error) {
  batch *b = context;
  int rc = CAI_OK;
  if (b->projecting)
    return b->projecting_document
               ? cai_cli_batch_document_event(&b->document_reader, e, error)
           : b->projector != NULL
               ? cai_transcript_projector_event(b->projector, e, error)
               : CAI_OK;
  if (e->kind == CAI_TRANSCRIPT_ERROR) {
    if (b->handoff && e->failure == CAI_TRANSCRIPT_CONTEXT_CLOSED) {
      b->entry_open = b->subscribed = 0;
      return CAI_OK;
    }
    return cai_cli_error(error, e->code != CAI_OK ? e->code : CAI_ERR_TRANSPORT,
                         e->reason != NULL ? e->reason
                                           : "batch transcript unavailable");
  }
  if (b->feed != NULL && (!b->replaying || !b->hide_handoff_replay ||
                          e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN ||
                          e->kind == CAI_TRANSCRIPT_REPLAY_END)) {
    rc = cai_cli_plain_feed_transcript(b->feed, e, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (b->pending && b->shell_command && b->shell_sequence != 0U &&
      e->sequence == b->shell_sequence &&
      (e->kind == CAI_TRANSCRIPT_SHELL_OUTPUT ||
       e->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END)) {
    if (b->shell_end || e->offset != b->shell_offset)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "batch shell output lost its ordered span");
    if (e->kind == CAI_TRANSCRIPT_SHELL_OUTPUT_END)
      b->shell_end = 1;
    else {
      if (b->feed != NULL) {
        /* The ordinary transcript presenter owns live/tail deduplication. */
      } else if (b->shell_last) {
        if (fwrite(e->data, 1U, e->length, stdout) != e->length ||
            fflush(stdout) != 0)
          return cai_cli_error(error, CAI_ERR_TRANSPORT,
                               "write batch shell output");
      } else if (!b->shared_logger)
        cai_cli_log_message(b->log->logger, PSLOG_LEVEL_INFO, "shell_output",
                            "user", e->data, e->length, NULL, 0U);
      b->shell_offset += e->length;
    }
  }
  if (b->selection != NULL) {
    rc = cai_transcript_selection_event(b->selection, e, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    memset(&b->at, 0, sizeof(b->at));
    snprintf(b->at.session, sizeof(b->at.session), "%s", e->session);
    snprintf(b->at.collection, sizeof(b->at.collection), "%s", e->collection);
    b->at.stream = e->stream;
    b->at.conversation = e->conversation;
    b->replaying = 1;
  } else if (e->kind == CAI_TRANSCRIPT_EVENT_BEGIN) {
    snprintf(b->type, sizeof(b->type), "%s", e->type);
    b->entry_open = 1;
  } else if (!b->shared_logger && e->kind == CAI_TRANSCRIPT_DATA &&
             (strcmp(b->type, "assistant_text_delta") == 0 ||
              strcmp(b->type, "reasoning_summary") == 0 ||
              strcmp(b->type, "turn_submitted") == 0 ||
              strcmp(b->type, "goal_continuation") == 0)) {
    pslog_field field = pslog_str("prompt_kind", "normal");
    int assistant = strcmp(b->type, "assistant_text_delta") == 0 ||
                    strcmp(b->type, "reasoning_summary") == 0;
    cai_cli_log_message(b->log->logger, PSLOG_LEVEL_INFO, b->type,
                        assistant ? "assistant" : "user", e->data, e->length,
                        &field, 1U);
  } else if (e->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (b->pending && b->shell_command && b->shell_started &&
        b->shell_sequence == 0U && strcmp(b->type, "user_shell_started") == 0)
      b->shell_sequence = e->sequence;
    b->at.position = e->position;
    b->at.sequence = e->sequence;
    b->entry_open = 0;
    if (b->settled) {
      rc = cai_transcript_selection_get(b->selection, &b->result, error);
      if (rc != CAI_OK)
        return rc;
      if (b->result.state < CAI_TRANSCRIPT_WORK_COMPLETED)
        return cai_cli_error(
            error, CAI_ERR_PROTOCOL,
            "batch settlement has no matched terminal outcome");
      b->has_result = 1;
      b->output_document = b->selected_review = 0;
      b->work_failed = b->result.state != CAI_TRANSCRIPT_WORK_COMPLETED;
      cai_transcript_selection_close(b->selection);
      b->selection = NULL;
      b->settled = 0;
      if (!b->work_failed && strcmp(b->goal_status, "active") == 0) {
        /* Install before returning this END: the next unowned turn may already
         * follow in the same socket pump. No status-channel or idle race. */
        b->goal_follow = 1;
        rc = cai_transcript_selection_open(&b->at, NULL, &b->selection, error);
        if (rc != CAI_OK)
          return rc;
      } else {
        if (strcmp(b->goal_status, "none") != 0 &&
            strcmp(b->goal_status, "complete") != 0)
          b->work_failed = 1;
        b->awaiting_application = 1;
        b->goal_follow = 1;
        rc = cai_transcript_selection_open(&b->at, NULL, &b->selection, error);
        if (rc != CAI_OK)
          return rc;
      }
    }
    if (b->application_settled) {
      cai_transcript_selection_result waiting;
      rc = cai_transcript_selection_get(b->selection, &waiting, error);
      if (rc != CAI_OK)
        return rc;
      if (waiting.state != CAI_TRANSCRIPT_WORK_WAITING)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "application settled during an active batch turn");
      cai_transcript_selection_close(b->selection);
      b->selection = NULL;
      b->pending = b->awaiting_application = b->application_settled = 0;
      if (b->work_failed)
        b->startup.stage = CAI_CLI_STARTUP_DONE;
    }
  } else if (e->kind == CAI_TRANSCRIPT_REPLAY_END) {
    b->at.position = e->watermark;
    b->replaying = 0;
    b->hide_handoff_replay = 0;
    if (b->feed != NULL && b->handoff &&
        b->at.conversation != b->old_conversation) {
      b->handoff = 0;
      b->plain_handoff_drain = 0;
    }
  }
  return CAI_OK;
}
static int rendered(void *context, const char *bytes, size_t count) {
  (void)context;
  return fwrite(bytes, 1U, count, stdout) == count ? 0 : -1;
}
static int project(void *context, const void *bytes, size_t length,
                   cai_error *error) {
  batch *b = context;
  if (b->selected_review) {
    static const char header[] = "# Review handoff";
    const unsigned char *p = bytes;
    if (!b->json_output) {
      while (length && b->header_offset < sizeof(header) - 1U) {
        if (*p++ != (unsigned char)header[b->header_offset++])
          return cai_cli_error(error, CAI_ERR_PROTOCOL,
                               "review Markdown header changed");
        --length;
      }
      if (b->header_offset == sizeof(header) - 1U && !b->header_written) {
        static const char title[] = "# Review findings";
        if (fwrite(title, 1U, sizeof(title) - 1U, b->destination) !=
            sizeof(title) - 1U)
          return cai_cli_error(error, CAI_ERR_TRANSPORT, "write review title");
        b->header_written = 1;
      }
    }
    if (length != 0U) {
      if (fwrite(p, 1U, length, b->destination) != length)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "write review output");
      b->last_byte = p[length - 1U];
    }
    return CAI_OK;
  }
  return b->renderer->feed(b->renderer, bytes, length) == MDF_OK
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "render batch response");
}
static int projection_open(batch *b, const cai_cli_options *o,
                           cai_error *error) {
  mdf_options options;
  mdf_sink output;
  cai_sink_callbacks callbacks = {0};
  int rc;
  const cai_transcript_selection_result *range =
      b->output_document ? &b->output.range : &b->result;
  if (b->selected_review) {
    b->destination = stdout;
    if (o->out != NULL) {
      int directory = open(o->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      int fd = directory >= 0
                   ? openat(directory, o->out,
                            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666)
                   : -1;
      if (directory >= 0)
        close(directory);
      if (fd < 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "cannot open review output");
      b->destination = fdopen(fd, "w");
      if (b->destination == NULL) {
        close(fd);
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "cannot open review output stream");
      }
      b->owns_destination = 1;
    }
  } else {
    mdf_options_init(&options);
    options.output_fd = STDOUT_FILENO;
    options.width = 80;
    options.boring = 1;
    options.margin_left = 0;
    options.ansi_mode = MDF_ANSI_OFF;
    options.table_buffer_mode = MDF_TABLE_BUFFER_FULL;
    if (mdf_create(MDF_FORMAT_ANSI, &options, &b->renderer) != MDF_OK)
      return cai_cli_error(error, CAI_ERR_NOMEM, "create batch renderer");
    output.userdata = b;
    output.write = rendered;
    if (b->renderer->set_sink(b->renderer, &output) != MDF_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "connect batch renderer");
  }
  callbacks.context = b;
  callbacks.write = project;
  rc = cai_sink_from_callbacks(&callbacks, &b->sink, error);
  if (rc == CAI_OK) {
    if (b->output_document) {
      rc = cai_cli_batch_document_read(&b->document_reader, &b->output, b->sink,
                                       error);
      b->projecting_document = 1;
    } else
      rc = cai_transcript_projector_open(&b->result, b->sink, &b->projector,
                                         error);
  }
  if (rc == CAI_OK) {
    b->projecting = 1;
    rc = cai_app_client_subscribe(
        b->client, ++b->subscription, range->after.conversation,
        range->after.session, range->after.collection, range->after.position,
        range->after.sequence, -1LL, error);
  }
  return rc;
}
/** Never turn silence, a shared idle status or a queue receipt into success.
 * Delivery stays gated until this exact input's observer has been installed. */
static int advance(batch *b, const cai_cli_options *o, cai_error *error) {
  cai_input_scope scope;
  int rc;
  if (!b->state.bound || !b->state.ready || !b->subscribed || b->replaying ||
      b->entry_open || b->at.stream == 0U)
    return CAI_OK;
  scope.conversation = b->state.conversation;
  scope.generation = b->state.transport.generation;
  if (b->pending && b->receipt == 1) {
    snprintf(b->call, sizeof(b->call), "%s:%s:%llu:%llu:%llu",
             b->command ? "command" : "input", b->state.session,
             b->state.producer, b->request, b->item);
    if (b->command && b->feed == NULL) {
      rc = cai_cli_batch_document_init(&b->command_document, "command.markdown",
                                       NULL, b->call, NULL, error);
      if (rc != CAI_OK)
        return rc;
    }
    if (b->feed == NULL && (!b->command || b->goal_command || b->goal_start)) {
      b->goal_follow = b->goal_command;
      rc = cai_transcript_selection_open(
          &b->at, b->goal_follow ? NULL : b->call, &b->selection, error);
      if (rc != CAI_OK)
        return rc;
    }
    if (!b->shared_logger) {
      pslog_field field =
          pslog_str("prompt_kind", b->command ? "command" : "normal");
      cai_cli_log_message(b->log->logger, PSLOG_LEVEL_INFO,
                          b->command ? "command_input" : "turn_submitted",
                          "user", b->offered, strlen(b->offered), &field, 1U);
    }
    free(b->offered);
    b->offered = NULL;
    rc = cai_app_client_settled(b->client, &scope, b->request, error);
    if (rc != CAI_OK)
      return rc;
    b->receipt = 2;
    if (b->feed != NULL) {
      if (b->command)
        ++b->plain_commands;
      else
        ++b->plain_turns;
      if (b->source_quit) {
        b->input_eof = 1;
        b->plain_quit = b->request;
      }
      b->pending = 0;
    }
  }
  if (b->command_done) {
    b->command_done = b->handoff = 0;
    b->command = 0;
    if (!b->goal_command && !b->goal_start && !b->review_command &&
        !b->shell_command)
      b->pending = 0;
    if (b->command_code != CAI_OK && b->feed == NULL)
      return cai_cli_error(
          error, b->command_code,
          "batch startup command failed; instructions not submitted");
    if (b->feed == NULL && !b->startup_control && !b->goal_command &&
        !b->goal_start && !b->review_command && !b->shell_command) {
      rc = cai_cli_batch_document_finish(&b->command_document, CAI_OK, error);
      if (rc != CAI_OK)
        return rc;
      b->output = b->command_document;
      b->output_document = 1;
      b->selected_review = b->has_result = 0;
    }
  }
  if (b->shell_done) {
    if (!b->shell_end)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "batch shell settled without complete live output");
    b->pending = b->shell_done = 0;
    b->has_result = b->output_document = b->selected_review = 0;
    if (b->shell_code != CAI_OK && b->feed == NULL) {
      b->work_failed = 1;
      b->startup.stage = CAI_CLI_STARTUP_DONE;
    }
  }
  if (b->review_done) {
    b->pending = b->review_done = 0;
    if (b->review_code != CAI_OK) {
      b->work_failed = 1;
      b->has_result = b->output_document = b->selected_review = 0;
      b->startup.stage = CAI_CLI_STARTUP_DONE;
    } else {
      b->json_output = strcmp(o->output_type, "json") == 0;
      b->output = b->json_output ? b->review_json : b->review_markdown;
      rc = cai_cli_batch_document_finish(&b->output, CAI_OK, error);
      if (rc != CAI_OK)
        return rc;
      if (b->output.range.text_bytes == 0U)
        return cai_cli_error(error, CAI_ERR_PROTOCOL,
                             "review finished without its validated document");
      b->output_document = b->selected_review = 1;
      b->has_result = 0;
    }
  }
  if (!b->pending && !b->plain_barrier && !b->work_failed &&
      (b->startup.stage < CAI_CLI_STARTUP_DONE ||
       (b->lines != NULL && !b->input_eof))) {
    cai_input_request request = {0};
    cai_cli_command_tokens tokens;
    const char *goal_argument;
    size_t goal_word;
    int is_goal;
    char *text = NULL;
    int control = 0;
    rc = cai_cli_startup_next(&b->startup, o, &text, &control, error);
    if (rc == CAI_OK && text == NULL && b->lines != NULL && !b->input_eof) {
      rc = cai_cli_line_input_next(b->lines, b->input_ready, &text,
                                   &b->input_eof, error);
      b->input_ready = 0;
    }
    if (rc != CAI_OK || text == NULL)
      return rc;
    b->request = b->state.last_request + 1U;
    b->startup_control = control;
    b->source_quit = 0;
    b->command = control || text[0] == '/' || text[0] == '!';
    cai_cli_command_parse(text, &tokens);
    if (b->feed != NULL) {
      if (cai_cli_command_quit(text) > 0)
        b->source_quit = 1;
      if ((tokens.name_length == 4U && memcmp(tokens.name, "/new", 4U) == 0) ||
          (tokens.name_length == 7U && memcmp(tokens.name, "/resume", 7U) == 0))
        b->plain_barrier = b->request;
      b->plain_quiet = 0;
    }
    /* Match the owner's command spans and goal-action whitespace. A launch
     * acknowledgement cannot settle work accepted under another spelling. */
    is_goal = tokens.name_length == 5U && memcmp(tokens.name, "/goal", 5U) == 0;
    goal_argument = tokens.arguments + strspn(tokens.arguments, " \t\r\n");
    goal_word = strcspn(goal_argument, " \t\r\n");
    b->goal_command =
        b->command &&
        ((is_goal &&
          ((goal_word == 3U && memcmp(goal_argument, "new", 3U) == 0) ||
           (goal_word == 6U && memcmp(goal_argument, "resume", 6U) == 0))) ||
         (tokens.name_length == 15U &&
          memcmp(tokens.name, "/review-and-fix", 15U) == 0));
    b->goal_start =
        is_goal && cai_cli_goal_input_argument(tokens.arguments) != NULL;
    b->review_command = b->command && tokens.name_length == 7U &&
                        memcmp(tokens.name, "/review", 7U) == 0;
    b->shell_command = text[0] == '!';
    b->shell_started = b->shell_end = b->shell_done = 0;
    b->shell_sequence = b->shell_offset = 0U;
    b->shell_last = b->startup.instruction == o->instruction_count;
    b->review_session[0] = '\0';
    memset(&b->review_json, 0, sizeof(b->review_json));
    b->pending = 1;
    b->receipt = 0;
    request.request_id = b->request;
    request.operation = CAI_INPUT_OFFER;
    request.intent = CAI_INPUT_QUEUED;
    request.text = text;
    rc = cai_app_client_request(b->client, &scope, &request, error);
    b->offered = text;
    return rc;
  }
  return CAI_OK;
}

int cai_cli_batch_run(const cai_app_config *app, const cai_cli_options *options,
                      const cai_app_client_config *initial,
                      const cai_cli_frontend_boot *boot, int *code,
                      cai_error *error) {
  batch *b = calloc(1U, sizeof(*b));
  cai_app_client_config target = *initial;
  int rc = CAI_OK;
  uint64_t detach_deadline = 0U;
  *code = 1;
  if (b == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate batch adapter");
  b->shared_logger = app->logger != NULL;
  b->plain_quiet = 1;
  rc = cai_cli_log_new_at(&b->log, PSLOG_LEVEL_INFO, 0, app->logger,
                          options->environment, options->directory, error);
  if (rc == CAI_OK && !options->non_interactive && !options->review &&
      !options->review_and_fix && options->instruction_count == 0U &&
      options->goal == NULL) {
    rc = cai_cli_line_input_open(STDIN_FILENO, &b->lines, error);
    if (rc == CAI_OK)
      rc = cai_cli_plain_feed_open(options, &b->feed, error);
  }
  while (rc == CAI_OK) {
    struct pollfd fds[3];
    uint64_t now = clock_ms(), deadline = 0U, boot_deadline = 0U;
    int ready = 0, timeout = -1;
    memset(fds, 0, sizeof(fds));
    fds[0].fd = fds[1].fd = fds[2].fd = -1;
    if (boot != NULL) {
      rc = boot->pump(boot->context, now, &target, &ready, error);
      if (rc != CAI_OK) {
        if (b->detaching) {
          /* Accepted work and output are already settled. Native composition
           * reports the owner's shutdown code after joining; a retained turn
           * failure must not become a second attachment failure here. */
          cai_error_cleanup(error);
          cai_error_init(error);
          rc = CAI_OK;
        }
        break;
      }
      if (ready == CAI_CLI_FRONTEND_STOPPED ||
          ready == CAI_CLI_FRONTEND_DETACH) {
        if (!b->detaching)
          b->work_failed = 0; /* Explicit native host STOP owns this outcome. */
        break;
      }
    } else
      ready = CAI_CLI_FRONTEND_EXISTING;
    if (ready == CAI_CLI_FRONTEND_EXISTING && b->client == NULL &&
        options->review &&
        options->developer_instructions_override_file != NULL) {
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "--review with --developer-instructions requires a "
                         "new workspace owner; stop the existing server and "
                         "retry (startup overrides are not hot-loaded)");
      break;
    }
    if (ready && b->client == NULL) {
      target.event = observe;
      target.context = b;
      target.transcript = transcript;
      target.transcript_context = b;
      target.execution = execution;
      target.execution_context = b;
      b->startup.stage = ready == CAI_CLI_FRONTEND_STARTED
                             ? CAI_CLI_STARTUP_REVIEW_AND_FIX
                             : CAI_CLI_STARTUP_MODEL;
      b->startup_select = ready != CAI_CLI_FRONTEND_STARTED &&
                          (options->resume_id != NULL || options->new_session);
      rc = cai_app_client_open(&target, now, &b->client, error);
      if (rc == CAI_OK)
        rc = cai_app_client_manage_delivery(b->client, error);
      if (rc != CAI_OK)
        break;
    }
    if (b->client != NULL) {
      const cai_app_view *view = NULL;
      rc = cai_app_client_pump(b->client, now, error);
      if (rc != CAI_OK)
        break;
      rc = cai_app_client_get_state(b->client, &b->state, error);
      if (rc != CAI_OK)
        break;
      if (b->state.transport.phase == CAI_WORKSPACE_DISCONNECTED ||
          b->state.transport.phase == CAI_WORKSPACE_FAILED) {
        if (!b->detaching)
          rc = cai_cli_error(
              error, CAI_ERR_TRANSPORT,
              "batch server disconnected; accepted work outcome unknown");
        break;
      }
      /* A force-quit receipt is attachment-local, not another turn to await.
       * Flush only the rendered feed before ending this connection. */
      if (!b->detaching && b->force_quit) {
        rc = cai_cli_plain_feed_finish(b->feed, error);
        if (rc == CAI_OK)
          rc = cai_app_client_detach_now(b->client, error);
        if (rc != CAI_OK)
          break;
        b->detaching = 1;
        detach_deadline = now + 2000U;
      }
      if (!b->detaching &&
          b->state.transport.phase == CAI_WORKSPACE_CONNECTED) {
        if (b->state.transport.controller != b->state.transport.attachment ||
            (b->generation != 0U &&
             b->generation != b->state.transport.generation)) {
          rc = cai_cli_error(
              error, CAI_ERR_INVALID,
              "batch requires controller authority; no automatic takeover");
          break;
        }
        b->generation = b->state.transport.generation;
        rc = cai_app_client_get_view(b->client, &view, error);
        if (rc != CAI_OK)
          break;
        if (!b->state.bound && !b->awaiting_bind && view != NULL &&
            view->available &&
            (!b->handoff || view->conversation != b->old_conversation)) {
          rc =
              b->startup_select
                  ? cai_app_client_select(
                        b->client, ++b->binding,
                        options->new_session ? NULL : options->resume_id, error)
                  : cai_app_client_bind(b->client, ++b->binding, 0U, 0U, NULL,
                                        error);
          b->startup_select = 0;
          b->awaiting_bind = 1;
        }
        if (rc == CAI_OK && b->state.ready && !b->subscribed) {
          long long replay = b->feed != NULL ? options->replay_turns : 0LL;
          /* Catch up an accepted context-changing command's result even when
           * console replay is disabled. Only its exact result is displayed. */
          b->hide_handoff_replay =
              b->feed != NULL && b->handoff && replay == 0LL;
          if (b->handoff && replay == 0LL)
            replay = 1LL;
          rc = cai_app_client_subscribe(b->client, ++b->subscription,
                                        b->state.conversation, b->state.session,
                                        NULL, 0U, 0U, replay, error);
          b->subscribed = 1;
          b->replaying = 1;
        }
        if (rc == CAI_OK && !b->projecting)
          rc = advance(b, options, error);
        if (rc == CAI_OK && !b->projecting && !b->pending &&
            b->startup.stage == CAI_CLI_STARTUP_DONE &&
            (b->lines == NULL ||
             (b->input_eof && b->plain_quiet && b->plain_turns == 0U &&
              b->plain_commands == 0U) ||
             b->work_failed)) {
          if (b->feed != NULL) {
            rc = cai_cli_plain_feed_finish(b->feed, error);
            b->has_result = b->output_document = 0;
          }
          if ((b->output_document && b->output.range.text_bytes != 0U) ||
              (b->has_result && b->result.text_bytes != 0U))
            rc = projection_open(b, options, error);
          else
            b->projecting = 1;
        }
        if (rc == CAI_OK && b->projecting &&
            (b->projecting_document
                 ? b->document_reader.done
                 : b->projector == NULL ||
                       cai_transcript_projector_done(b->projector))) {
          if (b->selected_review) {
            if ((!b->json_output && !b->header_written) ||
                (b->last_byte != '\n' && fputc('\n', b->destination) == EOF) ||
                fflush(b->destination) != 0)
              rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                                 "finish review output");
          }
          if (b->renderer != NULL &&
              (b->renderer->finish_document(b->renderer) != MDF_OK ||
               fflush(stdout) != 0))
            rc =
                cai_cli_error(error, CAI_ERR_TRANSPORT, "write batch response");
          if (rc == CAI_OK)
            rc = cai_app_client_detach(b->client, error);
          b->detaching = 1;
          detach_deadline = now + 2000U;
        }
      }
    }
    if (rc != CAI_OK)
      break;
    if (b->client != NULL)
      rc = cai_app_client_prepare(b->client, &fds[0], &deadline, error);
    if (rc == CAI_OK && boot != NULL)
      rc = boot->prepare(boot->context, &fds[1], &boot_deadline, error);
    if (rc != CAI_OK)
      break;
    if (b->lines != NULL && !b->input_eof && !b->pending && !b->plain_barrier &&
        b->state.bound && b->state.ready && b->subscribed && !b->replaying &&
        !b->projecting && b->startup.stage == CAI_CLI_STARTUP_DONE)
      cai_cli_line_input_prepare(b->lines, &fds[2]);
    if (boot_deadline && (!deadline || boot_deadline < deadline))
      deadline = boot_deadline;
    if (b->detaching && (!deadline || detach_deadline < deadline))
      deadline = detach_deadline;
    now = clock_ms();
    if (b->detaching && now >= detach_deadline) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "batch detach timed out");
      break;
    }
    if (deadline)
      timeout = deadline <= now            ? 0
                : deadline - now > INT_MAX ? INT_MAX
                                           : (int)(deadline - now);
    if (poll(fds, 3U, timeout) < 0 && errno != EINTR) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for batch attachment");
      break;
    }
    if (fds[2].revents != 0) {
      if (fds[2].revents & POLLNVAL) {
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "redirected input is closed");
        break;
      }
      b->input_ready = 1;
    }
  }
  if (rc == CAI_OK)
    *code = b->work_failed ? 1 : 0;
  if (rc != CAI_OK || b->work_failed) {
    cai_error failed;
    cai_error_init(&failed);
    if (rc != CAI_OK)
      cai_cli_log_error(b->log != NULL ? b->log->logger : NULL, "batch", error);
    else if (!b->shared_logger) {
      cai_cli_error(&failed, CAI_ERR_TRANSPORT, "batch turn failed");
      cai_cli_log_message(b->log->logger, PSLOG_LEVEL_ERROR, "run_failed",
                          "runtime", failed.message, strlen(failed.message),
                          NULL, 0U);
    }
    cai_error_cleanup(&failed);
  }
  cai_app_client_close(b->client);
  cai_cli_line_input_close(b->lines);
  cai_cli_plain_feed_close(b->feed);
  cai_transcript_selection_close(b->selection);
  cai_transcript_projector_close(b->projector);
  cai_sink_close(b->sink);
  if (b->renderer != NULL)
    b->renderer->destroy(b->renderer);
  if (b->owns_destination && fclose(b->destination) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "close review output");
  if (cai_cli_log_release(b->log) != 0 && rc == CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "write batch activity log");
  free(b->offered);
  free(b);
  return rc;
}
