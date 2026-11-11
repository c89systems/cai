#define _GNU_SOURCE
#include "frontend.h"
#include "command_help.h"
#include "commands.h"
#include "completion.h"
#include "config.h"
#include "error.h"
#include "input_socket.h"
#include "input_wire.h"
#include "prompt_listener.h"
#include "prompt_send.h"
#include "replay_tools.h"
#include "review_report.h"
#include "startup.h"
#include "status.h"
#include "statusline.h"
#include "terminal_program.h"
#include "tool_result.h"
#include "transcript_presenter.h"
#include "workspace_claims.h"
#include <cai/app_terminal.h>
#include <cai/workspace_discovery.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** Terminal-owner composition only. All runtime/storage policy stays behind
 * the socket. Wake bytes merely schedule transport progress after an editor
 * submission; they are not another input queue. */
/** Inputs typed while the initial recall suffix is in flight. This is bounded
 * native history state, never another model-input queue. Store positions let
 * the completed snapshot avoid importing an already included entry twice. */
typedef struct recall_pending {
  struct recall_pending *next;
  char *text;
  uint64_t request, position;
} recall_pending;
typedef struct frontend_view {
  struct frontend_view *next;
  cai_cli_presenter ui;
  cai_cli_input_socket *queue;
  cai_cli_transcript_presenter *transcript;
  cai_cli_replay_tools *tools;
  cai_cli_completions completions;
  cai_cli_reasoning child_reasoning;
  char session[129];
  unsigned long long producer, epoch;
  char displayed_collection[21];
  uint64_t displayed_position, displayed_sequence;
  sl_watch_id_t socket_watch, boot_watch, timer_watch, wake_watch;
  int socket_fd, boot_fd;
  int activity_known, activity_busy;
  int history_loaded;
  unsigned int history_limit;
  recall_pending *recall_first, *recall_last;
  size_t recall_count;
} frontend_view;
typedef struct frontend_group frontend_group;
typedef struct frontend {
  struct frontend *next;
  frontend_group *group;
  char *workspace, *runtime_directory;
  unsigned char instance[16];
  sl_watch_id_t parked_watch;
  int parked_fd, parked_failed, expected_close;
  int handoff_fd, handoff_failed;
  unsigned int handoff_events;
  char resume_collection[21];
  uint64_t resume_position, resume_sequence;
  int resume_actions;
  char *skills[CAI_SKILLS_MAX_PACKAGES],
      *skill_candidate[CAI_SKILLS_MAX_PACKAGES];
  size_t skill_count, skill_candidate_count;
  uint64_t skill_conversation;
  int skills_ready;
  const cai_app_config *application;
  const cai_cli_options *options;
  const cai_cli_frontend_boot *boot;
  cai_app_client_config target;
  cai_app_client *client;
  cai_cli_input_socket *queue;
  cai_app_client_state state;
  cai_cli_presenter *ui;
  cai_cli_transcript_presenter *transcript;
  cai_cli_status status;
  cai_cli_replay_tools *tools;
  frontend_view *views, *current;
  cai_cli_theme theme;
  long long theme_offsets[3];
  int full_output;
  int review_open;
  int compacting;
  int history_requested, history_loaded;
  int resolve_receipts;
  unsigned int history_limit;
  uint64_t history_request, exit_save;
  char *history_entry, *exit_draft;
  frontend_view *exit_view;
  size_t history_length;
  char history_warning[192];
  char history_command[256];
  char next_session[129];
  int pending_context, switching;
  cai_error error;
  cai_sink *execution_sink;
  int wake[2], timer;
  sl_watch_id_t socket_watch, boot_watch, timer_watch, wake_watch;
  int socket_fd, boot_fd, failed, quit, ready, bound, subscribed, replaying;
  int replay_finished;
  int action_field, shell, awaiting_bind, welcomed, control, greeting_resumed;
  int child_theme_scope;
  int review_header_shown;
  char replay_child[129];
  int transcript_entry, presentation_parked;
  uint64_t bind_request, subscribe_request, control_request;
  unsigned long long startup_request, startup_producer;
  char startup_session[129];
  cai_cli_startup startup;
  int startup_command;
  int startup_control, startup_feeding;
  int startup_select;
  struct {
    uint64_t request, producer;
    char session[129];
  } hidden_recall[CAI_CLI_STARTUP_INSTRUCTIONS];
  size_t action_length;
  char *action;
  char action_call[256], handoff_call[256];
  cai_input_scope capture_scope;
  uint64_t capture_id, capture_request, capture_sequence, capture_deadline;
  uint64_t capture_first, capture_last;
  int capture_waiting, capture_pending, capture_code;
  char capture_reason[192];
  char handoff_notice[512];
  int handoff_notice_error;
  size_t command_length;
  char *command;
  cai_cli_tool_text tool_text;
  cai_cli_prompt_listener *prompt_listener;
  int prompt_announced;
  cai_cli_prompt_send *prompt_send;
  cai_protocol_header prompt_send_scope;
  sl_watch_id_t send_watch;
  sl_t *send_editor;
  uint64_t capability_generation, capability_epoch;
  unsigned int capability_flags;
} frontend;

/** One terminal, several explicitly visited owners. Parked sockets continue
 * processing authority/queue observations, but never render into the selected
 * terminal. Their attachment keeps nonpersistent owners alive until quit. */
struct frontend_group {
  frontend *first, *active, *candidate;
  cai_workspace_discovery *discovery;
  sl_watch_id_t discovery_watch;
  int discovery_fd, listing, listed, switching, requested;
  int terminal_active;
  char *selector;
};

static uint64_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int service(frontend *);
static int interests(frontend *);
static int actions(frontend *);
static int select_view(frontend *, const char *);
static int notice(frontend *, const char *);
static int workspace_begin(frontend *, const char *);
static int workspace_progress(frontend *);
static int workspace_interests(frontend *, uint64_t *);
static int workspace_activate(frontend **);
static int network_event(void *, const cai_app_network_event *, cai_error *);
static int network_progress(frontend *);
static int completion_catalog(void *context, char trigger,
                              sl_completion_emit_t emit, void *emit_context) {
  size_t i;
  int rc = SL_OK;
  frontend *f = context;
  if (trigger == '/') {
    for (i = 0U; rc == SL_OK && i < cai_cli_command_count(); ++i)
      rc = emit(cai_cli_command_at(i), emit_context);
    if (rc == SL_OK)
      rc = emit("/takeover", emit_context);
    if (rc == SL_OK)
      rc = emit("/shutdown", emit_context);
    if (rc == SL_OK)
      rc = emit("/workspace", emit_context);
  } else if (trigger == '$' && f->skills_ready && f->bound &&
             !f->pending_context &&
             f->skill_conversation == f->state.conversation) {
    for (i = 0U; i < f->skill_count && rc == SL_OK; ++i) {
      char name[CAI_SKILL_NAME_MAX + 2U];
      if (!cai_cli_completion_printable(f->skills[i]))
        continue;
      snprintf(name, sizeof(name), "$%s", f->skills[i]);
      rc = emit(name, emit_context);
    }
  }
  return rc;
}
static void clear_skills(frontend *f) {
  size_t i;
  for (i = 0U; i < CAI_SKILLS_MAX_PACKAGES; ++i) {
    free(f->skills[i]);
    free(f->skill_candidate[i]);
    f->skills[i] = f->skill_candidate[i] = NULL;
  }
  f->skill_count = f->skill_candidate_count = 0U;
  f->skills_ready = 0;
}
/** Only the completion consumer retains a bounded name list. Incomplete
 * candidates never replace its available catalog. No resource bodies or
 * descriptions are read here, including while switching workspaces. */
static int catalog(void *context, const cai_app_catalog_event *e,
                   cai_error *error) {
  frontend *f = context;
  if (e->phase == CAI_APP_CATALOG_BEGIN || e->phase == CAI_APP_CATALOG_ERROR) {
    clear_skills(f);
    f->skill_conversation = e->conversation;
    if (e->phase == CAI_APP_CATALOG_ERROR) {
      f->skills_ready = 1; /* Known unavailable: never hold normal input. */
      if (e->code != CAI_ERR_CANCELLED && notice(f, e->text) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "show catalog failure");
    }
  } else if (e->phase == CAI_APP_CATALOG_ITEM) {
    if (f->skill_candidate_count >= CAI_SKILLS_MAX_PACKAGES)
      return cai_cli_error(error, CAI_ERR_LIMIT,
                           "completion catalog limit exceeded");
    f->skill_candidate[f->skill_candidate_count] = strdup(e->text);
    if (f->skill_candidate[f->skill_candidate_count] == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "copy completion label");
    ++f->skill_candidate_count;
  } else if (e->phase == CAI_APP_CATALOG_END) {
    size_t i;
    for (i = 0U; i < f->skill_candidate_count; ++i) {
      f->skills[i] = f->skill_candidate[i];
      f->skill_candidate[i] = NULL;
    }
    f->skill_count = f->skill_candidate_count;
    f->skill_candidate_count = 0U;
    f->skills_ready = 1;
  }
  if (!f->group->terminal_active &&
      (e->phase == CAI_APP_CATALOG_END || e->phase == CAI_APP_CATALOG_ERROR) &&
      cai_cli_completions_open(&f->current->completions, f->ui->sl,
                               f->target.workspace, completion_catalog,
                               f) != SL_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "refresh completion labels");
  return CAI_OK;
}
static int quit_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int enter_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int cancel_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int shell_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int event_ready(sl_t *, const sl_watch_event_t *, void *);
static int markdown(frontend *f, const char *bytes, size_t length) {
  int saved = f->ui->tool_result_body;
  int rc;
  f->ui->tool_result_body = 1;
  rc = cai_cli_presenter_text(f->ui, bytes, length);
  f->ui->tool_result_body = saved;
  return rc;
}
static int tool_markdown(void *context, const char *bytes, size_t length) {
  return markdown(context, bytes, length);
}
/** Reuse the core's structured execution renderer for harness failures and
 * progress records. One borrowed chunk flows directly into this UI's MDF
 * document; no JSON, complete diagnostic or replay copy is accumulated. */
static int execution_markdown(void *context, const void *bytes, size_t length,
                              cai_error *error) {
  return markdown(context, bytes, length) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "render execution diagnostic");
}
static int notice(frontend *f, const char *message) {
  if (f->group->terminal_active) {
    snprintf(f->history_warning, sizeof(f->history_warning), "%s", message);
    return 0;
  }
  if (f->group->active != f)
    return 0;
  return cai_cli_presenter_notice(f->ui, message);
}
/** Project operation transitions once per editor. Both Softline setters reset
 * its animation clock, so feed/status refreshes must leave them untouched.
 * Retained session editors keep their own applied state when switched. */
static int activity(frontend *f, int busy) {
  frontend_view *v = f->current;
  if (!v->activity_known || v->activity_busy != busy) {
    if (sl_set_status_busy(v->ui.sl, busy) != SL_OK ||
        sl_set_status_spinner(v->ui.sl, busy) != SL_OK)
      return -1;
    v->activity_known = 1;
    v->activity_busy = busy;
  }
  return 0;
}
/** Finish only this terminal's presentation. Update the native sticky row
 * while its editor is active; no farewell is appended to the transcript. */
static int goodbye(frontend *f) {
  return activity(f, 0) == 0 &&
                 sl_set_status_message(f->ui->sl, "Goodbye.") == SL_OK
             ? 0
             : -1;
}
static void save_submission(void *context, const cai_input_request *request) {
  frontend *f = context;
  cai_cli_prompt_scope remote;
  cai_input_scope original;
  cai_error local;
  recall_pending *pending = NULL;
  if (request->operation == CAI_INPUT_OFFER &&
      cai_cli_prompt_listener_submission(f->prompt_listener, &remote)) {
    original.conversation = remote.epoch;
    original.generation = remote.generation;
    cai_cli_input_socket_offer_scope(f->queue, &original);
  }
  if ((f->startup_feeding && f->startup_control) || f->history_limit == 0U ||
      request->text[0] == '\0')
    return;
  if (!f->history_loaded) {
    frontend_view *v = f->current;
    pending = calloc(1U, sizeof(*pending));
    if (pending != NULL)
      pending->text = strdup(request->text);
    if (pending == NULL || pending->text == NULL) {
      free(pending);
      snprintf(f->history_warning, sizeof(f->history_warning),
               "Could not retain new prompt recall while loading.");
      return;
    }
    if (v->recall_last != NULL)
      v->recall_last->next = pending;
    else
      v->recall_first = pending;
    v->recall_last = pending;
    ++v->recall_count;
    if (v->recall_count > f->history_limit) {
      recall_pending *old = v->recall_first;
      v->recall_first = old->next;
      --v->recall_count;
      free(old->text);
      free(old);
    }
  }
  if (f->state.transport.controller != f->state.transport.attachment)
    return;
  cai_error_init(&local);
  if (pending != NULL && f->history_request != UINT64_MAX)
    pending->request = f->history_request + 1U;
  if (f->history_request == UINT64_MAX ||
      cai_app_client_save_prompt(f->client, ++f->history_request,
                                 f->state.transport.generation, request->text,
                                 &local) != CAI_OK)
    snprintf(f->history_warning, sizeof(f->history_warning),
             "Prompt history was not saved: %s",
             local.message != NULL ? local.message
                                   : "request identity exhausted");
  cai_error_cleanup(&local);
}
static int prompt_history(void *context, const cai_transcript_event *e,
                          cai_error *error) {
  frontend *f = context;
  if (f->group->terminal_active) {
    f->history_requested = f->history_loaded = 0;
    return CAI_OK;
  }
  if (e->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    f->history_limit = (unsigned int)e->visibility;
    f->history_length = 0U;
    if (sl_history_set_max_len(f->ui->sl, 0) != SL_OK ||
        sl_history_set_max_len(f->ui->sl, (int)f->history_limit) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "configure remote prompt recall");
  } else if (e->kind == CAI_TRANSCRIPT_EVENT_BEGIN)
    f->history_length = 0U;
  else if (e->kind == CAI_TRANSCRIPT_DATA) {
    if (e->length > CAI_CLI_INPUT_TEXT_MAX - f->history_length ||
        e->offset != f->history_length)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "invalid prompt recall chunks");
    if (f->history_entry == NULL) {
      f->history_entry = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
      if (f->history_entry == NULL)
        return cai_cli_error(error, CAI_ERR_NOMEM,
                             "allocate one recalled input");
    }
    memcpy(f->history_entry + f->history_length, e->data, e->length);
    f->history_length += e->length;
    f->history_entry[f->history_length] = '\0';
  } else if (e->kind == CAI_TRANSCRIPT_EVENT_END) {
    if (f->history_length != 0U &&
        sl_history_add(f->ui->sl, f->history_entry) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "import recalled prompt");
    f->history_length = 0U;
  } else if (e->kind == CAI_TRANSCRIPT_REPLAY_END ||
             e->kind == CAI_TRANSCRIPT_ERROR) {
    frontend_view *v = f->current;
    while (v->recall_first != NULL) {
      recall_pending *p = v->recall_first;
      v->recall_first = p->next;
      if (e->kind != CAI_TRANSCRIPT_REPLAY_END || p->position == 0U ||
          p->position > e->watermark)
        (void)sl_history_add(f->ui->sl, p->text);
      free(p->text);
      free(p);
    }
    v->recall_last = NULL;
    v->recall_count = 0U;
    f->history_loaded = 1;
    v->history_loaded = e->kind == CAI_TRANSCRIPT_REPLAY_END;
    v->history_limit = f->history_limit;
    f->ready = f->bound;
    if (sl_history_set_auto_add(f->ui->sl, 1) != SL_OK)
      return CAI_ERR_TRANSPORT;
    if (e->kind == CAI_TRANSCRIPT_ERROR &&
        e->failure != CAI_TRANSCRIPT_CONTEXT_CLOSED)
      (void)notice(f,
                   e->reason != NULL ? e->reason : "Prompt recall unavailable");
  }
  return CAI_OK;
}
static int status(frontend *f) {
  const cai_app_view *view = NULL;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  unsigned long long elapsed;
  long long goal_seconds;
  char message[1024];
  const char *title = NULL;
  int busy = 0, finishing = 0, active;
  memset(&goal, 0, sizeof(goal));
  memset(&metrics, 0, sizeof(metrics));
  if (f->client != NULL &&
      cai_app_client_get_view(f->client, &view, NULL) == CAI_OK &&
      view != NULL && view->available) {
    metrics = view->metrics;
    /* The worker stops its timer before appending the final journal record.
     * Runtime state stays active until that append and its terminal receipt
     * finish; timer completion alone cannot settle this presentation. */
    active = view->runtime_state == CAI_AGENT_SAMPLING ||
             view->runtime_state == CAI_AGENT_DISPATCHING_TOOL;
    busy = active || metrics.turn_active || f->review_open;
    finishing = active && !metrics.turn_active;
    cai_app_view_elapsed(view, clock_ms(), &elapsed, &goal_seconds);
    metrics.turn_elapsed_ms = elapsed;
    goal.has_goal = view->goal.has_goal;
    goal.status = view->goal.status;
    goal.elapsed_seconds = goal_seconds;
    cai_cli_status_build(
        &f->status, view->effective.model, view->effective.reasoning_effort,
        view->effective.service_tier,
        metrics.context_window_tokens > 0
            ? 100.0 * (double)metrics.context_used_tokens /
                  (double)metrics.context_window_tokens
            : 0.0,
        metrics.has_context_usage,
        view->provider != NULL && strcmp(view->provider, "chatgpt") == 0
            ? &view->quota
            : NULL,
        metrics.session_usage.estimated_spend_usd,
        metrics.session_usage.estimated_spend_eur, view->provider, &goal,
        view->yolo);
    if (cai_cli_status_apply(f->ui->sl, &f->status) != 0)
      return -1;
  }
  if (f->review_open) {
    if (cai_cli_reasoning_title(&f->current->child_reasoning, &title) != 0)
      return -1;
  } else {
    if (f->transcript != NULL &&
        cai_cli_transcript_presenter_title(f->transcript, &title) != 0)
      return -1;
  }
  if (f->compacting && metrics.turn_active)
    title = "Compacting context";
  if (f->ready && f->history_loaded && f->skills_ready && f->subscribed &&
      !f->replaying && f->replay_finished &&
      f->skill_conversation == f->state.conversation && !f->welcomed) {
    snprintf(message, sizeof(message), "%s",
             f->state.transport.controller != f->state.transport.attachment
                 ? "Observing workspace. /takeover acquires control. "
                   "Send /help for usage."
             : f->greeting_resumed ? "Session resumed. Send /help for usage."
                                   : "Session ready. Send /help for usage.");
    f->welcomed = 1;
  } else if (f->group->discovery != NULL || f->group->candidate != NULL)
    snprintf(message, sizeof(message), "Looking up workspace...");
  else if (f->exit_save != 0U)
    snprintf(message, sizeof(message), "Saving draft before exit...");
  else if (f->bound && !f->history_loaded)
    snprintf(message, sizeof(message), "Loading prompt history...");
  else if (f->bound &&
           (!f->skills_ready || f->skill_conversation != f->state.conversation))
    snprintf(message, sizeof(message), "Loading completions...");
  else if (!f->welcomed && f->subscribed)
    snprintf(message, sizeof(message), "Replaying session...");
  else if (f->handoff_notice[0] != '\0') {
    snprintf(message, sizeof(message), "Restoring input queue...");
    busy = 1;
  } else if (!f->ready)
    snprintf(message, sizeof(message), "Connecting to workspace server...");
  else if (f->state.transport.controller != f->state.transport.attachment)
    snprintf(message, sizeof(message),
             "Observing workspace. /takeover acquires control.");
  else if (finishing)
    snprintf(message, sizeof(message), "Finishing turn...");
  else if (f->review_open && !metrics.turn_active)
    snprintf(message, sizeof(message), "Review in progress...");
  else if (cai_cli_turn_status_message(message, sizeof(message), title,
                                       &metrics, f->ui->utc, time(NULL)) != 0)
    snprintf(message, sizeof(message), "Ready. /help for usage.");
  if (message[0] == '\0')
    snprintf(message, sizeof(message), "Ready. Send /help for usage.");
  return activity(f, busy) == 0 &&
                 sl_set_status_message(f->ui->sl, message) == SL_OK
             ? 0
             : -1;
}

static int observed(void *context, const cai_app_client_event *event,
                    cai_error *error) {
  frontend *f = context;
  f->state = *event->state;
  if (event->type == CAI_APP_CLIENT_BIND_REJECTED)
    return cai_cli_error(
        error,
        event->input->result.code != CAI_OK ? event->input->result.code
                                            : CAI_ERR_INVALID,
        event->input->result.reason[0] != '\0' ? event->input->result.reason
                                               : "session binding rejected");
  if (event->type == CAI_APP_CLIENT_SHELL_RESULT &&
      event->request == f->capture_request && f->capture_waiting) {
    cai_app_shell_result result;
    int rc = cai_app_client_get_shell_result(f->client, &result, error);
    if (rc != CAI_OK)
      return rc;
    f->capture_waiting = 0;
    f->capture_code = event->input->result.code;
    if (result.code == CAI_OK) {
      f->capture_first = result.first_sequence;
      f->capture_last = result.last_sequence;
    }
    snprintf(f->capture_reason, sizeof(f->capture_reason), "%s",
             event->input->result.reason);
  }
  if (f->queue != NULL && !f->group->terminal_active) {
    int suppress = 0;
    size_t i;
    int rc;
    if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
        event->input->type == CAI_INPUT_OBSERVATION_RESULT)
      for (i = 0U; i < CAI_CLI_STARTUP_INSTRUCTIONS; ++i)
        if (f->hidden_recall[i].request != 0U &&
            f->hidden_recall[i].request == event->request &&
            f->hidden_recall[i].producer == f->state.producer &&
            strcmp(f->hidden_recall[i].session, f->state.session) == 0) {
          suppress = 1;
          f->hidden_recall[i].request = 0U;
          break;
        }
    if (suppress)
      (void)sl_history_set_auto_add(f->ui->sl, 0);
    rc = cai_cli_input_socket_observe(f->queue, event, error);
    if (suppress)
      (void)sl_history_set_auto_add(f->ui->sl, f->history_loaded);
    if (rc != CAI_OK)
      return rc;
  }
  if (event->type == CAI_APP_CLIENT_STATUS && event->status->available)
    cai_cli_status_maybe_refresh_branch(&f->status, f->target.workspace);
  if (event->type == CAI_APP_CLIENT_CONTROL_RESULT &&
      event->input->result.code != CAI_OK) {
    f->expected_close = 0;
    (void)notice(f, event->input->result.reason);
  }
  if (event->type == CAI_APP_CLIENT_HISTORY_RESULT) {
    frontend_view *v;
    for (v = f->views; v != NULL; v = v->next) {
      recall_pending *p;
      for (p = v->recall_first; p != NULL; p = p->next)
        if (p->request != 0U && p->request == event->request &&
            event->input->result.code == CAI_OK)
          p->position = event->input->result.item_id;
    }
    if (event->input->result.code != CAI_OK &&
        !(f->exit_save != 0U && event->request == f->exit_save))
      (void)notice(f, event->input->result.reason);
    if (f->exit_save != 0U && event->request == f->exit_save) {
      if (event->input->result.code == CAI_OK && f->current == f->exit_view &&
          strcmp(sl_buffer(f->ui->sl), f->exit_draft) == 0) {
        if (goodbye(f) != 0)
          return cai_cli_error(error, CAI_ERR_TRANSPORT,
                               "close terminal draft");
        f->quit = 1;
      } else if (event->input->result.code == CAI_OK)
        (void)notice(f, "Draft changed while saving; exit cancelled.");
      else
        (void)notice(f, "Draft not saved; staying open. Retry Alt-Q to save.");
      f->exit_save = 0U;
      free(f->exit_draft);
      f->exit_draft = NULL;
    }
  }
  if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
      event->input->type == CAI_INPUT_OBSERVATION_RESULT &&
      event->input->result.request_id == f->startup_request &&
      f->startup_request != 0U && f->state.producer == f->startup_producer &&
      strcmp(f->state.session, f->startup_session) == 0) {
    if (event->input->result.outcome != CAI_INPUT_ACCEPTED) {
      f->startup.stage = CAI_CLI_STARTUP_DONE;
      f->startup_request = 0U;
      (void)notice(f, "Startup input was not accepted; inspect or recover its "
                      "native queue receipt.");
    } else if (!f->startup_command)
      f->startup_request = 0U;
  }
  if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
      event->input->type == CAI_INPUT_OBSERVATION_SNAPSHOT_END) {
    f->resolve_receipts = 1;
    f->bound = 1;
    f->ready = 1;
    snprintf(f->current->session, sizeof(f->current->session), "%s",
             f->state.session);
    f->current->producer = f->state.producer;
    f->current->epoch = f->state.conversation;
    if (!f->group->terminal_active &&
        cai_cli_completions_open(&f->current->completions, f->ui->sl,
                                 f->target.workspace, completion_catalog,
                                 f) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "bind completion catalog");
  } else if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
             event->input->type == CAI_INPUT_OBSERVATION_DISCONNECTED &&
             !f->state.bound) {
    if (f->queue != NULL && !f->group->terminal_active) {
      (void)cai_cli_input_socket_disconnect(f->queue, NULL);
      f->queue = NULL;
    }
    f->pending_context = 1;
    f->ready = f->bound = f->awaiting_bind = f->subscribed = 0;
  }
  return CAI_OK;
}
/** Park only after a validated entry. Later spans from the same transport pump
 * may advance its cursor, but must not advance the displayed resume position.
 */
static int park_presentation(frontend *f, const cai_transcript_event *end) {
  if (f->presentation_parked)
    return CAI_OK;
  if (cai_app_client_get_state(f->client, &f->state, &f->error) != CAI_OK)
    return f->error.code;
  snprintf(f->resume_collection, sizeof(f->resume_collection), "%s",
           f->state.transcript.collection);
  f->resume_position =
      end != NULL ? end->position : f->state.transcript.position;
  f->resume_sequence =
      end != NULL ? end->sequence : f->state.transcript.sequence;
  f->presentation_parked = 1;
  return CAI_OK;
}
static int transcript(void *context, const cai_transcript_event *event,
                      cai_error *error) {
  frontend *f = context;
  int rc;
  if (f->group->active != f || f->group->terminal_active ||
      f->presentation_parked)
    return CAI_OK;
  if (event->kind == CAI_TRANSCRIPT_TERMINAL_OUTPUT) {
    if (cai_cli_presenter_finish_response(f->ui) != 0 ||
        cai_cli_presenter_finish_reasoning(f->ui) != 0)
      return CAI_ERR_TRANSPORT;
    return cai_cli_replay_tools_live(f->tools, event, f->ui->tool_display,
                                     error);
  }
  if (event->kind == CAI_TRANSCRIPT_EVENT_BEGIN)
    f->transcript_entry = 1;
  else if (event->kind == CAI_TRANSCRIPT_EVENT_END ||
           event->kind == CAI_TRANSCRIPT_ERROR)
    f->transcript_entry = 0;
  if (event->kind == CAI_TRANSCRIPT_EVENT_BEGIN && event->type != NULL) {
    int review = strcmp(event->type, "review_pending") == 0;
    if (review) {
      f->review_open = 1;
      f->review_header_shown = 0;
      f->child_theme_scope = 2;
      cai_cli_reasoning_reset(&f->current->child_reasoning);
    }
    if (strcmp(event->type, "review_handoff_committed") == 0 ||
        strcmp(event->type, "subagent_handoff_committed") == 0) {
      f->child_theme_scope = f->review_open = 0;
      if (cai_cli_presenter_set_theme_scope(f->ui, 0, error) != CAI_OK)
        return CAI_ERR_TRANSPORT;
    }
    if ((review || strcmp(event->type, "assistant_text_delta") == 0 ||
         strcmp(event->type, "input_presented") == 0 ||
         strcmp(event->type, "reasoning_summary_started") == 0) &&
        cai_cli_presenter_set_theme_scope(f->ui, review ? 2 : 0, error) !=
            CAI_OK)
      return CAI_ERR_TRANSPORT;
    /* /review's accepted command already carries its shared start/end
     * documents. Only the model-invoked tool needs local lifecycle headings. */
  }
  if (event->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    f->replaying = 1;
    f->replay_finished = 0;
    f->replay_child[0] = '\0';
    if (event->omitted != 0U && f->options->replay_turns != 0L) {
      char message[160];
      snprintf(message, sizeof(message),
               "Showing the latest %ld turns. %llu earlier entries omitted.",
               f->options->replay_turns, (unsigned long long)event->omitted);
      if (notice(f, message) != 0)
        return CAI_ERR_TRANSPORT;
    }
  } else if (event->kind == CAI_TRANSCRIPT_REPLAY_END) {
    f->replaying = 0;
    f->replay_finished = 1;
    f->resume_actions = 0;
    f->resume_collection[0] = '\0';
    f->resume_position = f->resume_sequence = 0U;
    f->handoff_call[0] = '\0';
  }
  if (event->kind == CAI_TRANSCRIPT_ERROR) {
    /* A malformed cursor or unavailable index must not trigger an unbounded
     * replay-from-zero loop. Context handoff binds its new stream separately.
     * An acknowledged trim can continue after the last accepted native cursor;
     * the collection keeps those permanent positions even across deletion. */
    if (event->failure == CAI_TRANSCRIPT_CONTEXT_CLOSED) {
      f->subscribed = 0;
      cai_cli_transcript_presenter_reset(f->transcript);
      return CAI_OK;
    }
    cai_cli_transcript_presenter_reset(f->transcript);
    f->replaying = 0;
    if (event->failure == CAI_TRANSCRIPT_HISTORY_CHANGED) {
      const cai_app_transcript_state *cursor;
      if (cai_app_client_get_state(f->client, &f->state, error) != CAI_OK)
        return error != NULL ? error->code : CAI_ERR_INVALID;
      cursor = &f->state.transcript;
      snprintf(f->resume_collection, sizeof(f->resume_collection), "%s",
               cursor->collection);
      f->resume_position = cursor->position;
      f->resume_sequence = cursor->sequence;
      f->subscribed = 0;
    } else
      f->replay_finished =
          1; /* The inline warning settles this replay attempt. */
    return notice(f, event->reason != NULL ? event->reason
                                           : "Transcript unavailable") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  rc = cai_cli_transcript_presenter_event(f->transcript, event, error);
  if (rc == CAI_OK) {
    frontend_view *v = f->current;
    if (event->kind == CAI_TRANSCRIPT_REPLAY_BEGIN)
      snprintf(v->displayed_collection, sizeof(v->displayed_collection), "%s",
               event->collection);
    else if (event->kind == CAI_TRANSCRIPT_EVENT_END) {
      v->displayed_position = event->position;
      v->displayed_sequence = event->sequence;
    } else if (event->kind == CAI_TRANSCRIPT_REPLAY_END)
      v->displayed_position = event->watermark;
  }
  if (rc == CAI_OK && (f->shell || f->group->switching) &&
      event->kind == CAI_TRANSCRIPT_EVENT_END)
    rc = park_presentation(f, event);
  return rc;
}

/** Actions are bounded accepted input commands, not output documents.
 * Never run an action from replay or from another producer's transcript. */
static int action_chunk(frontend *f, const cai_execution_record *record,
                        int replay, cai_error *error) {
  char source[129], tail;
  unsigned long long producer, request, item;
  size_t length = strlen(record->data);
  if ((replay && !f->resume_actions) || record->call_id == NULL ||
      (replay && f->resume_actions &&
       strcmp(record->call_id, f->handoff_call) == 0) ||
      sscanf(record->call_id, "command:%128[^:]:%llu:%llu:%llu%c", source,
             &producer, &request, &item, &tail) != 4 ||
      producer != f->state.producer || strcmp(source, f->state.session) != 0)
    return CAI_OK;
  if (record->offset != f->action_length ||
      length > CAI_CLI_INPUT_TEXT_MAX - f->action_length)
    return cai_cli_error(error, CAI_ERR_PROTOCOL,
                         "invalid frontend command chunks");
  if (f->action == NULL) {
    f->action = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
    if (f->action == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM,
                           "allocate accepted frontend command");
  }
  memcpy(f->action + f->action_length, record->data, length);
  f->action_length += length;
  f->action[f->action_length] = '\0';
  if (record->final) {
    snprintf(f->action_call, sizeof(f->action_call), "%s", record->call_id);
    f->action_field = 1;
    return actions(f) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}
static int execution_body(void *context, const cai_app_execution_event *event,
                          cai_error *error) {
  frontend *f = context;
  const cai_execution_record *r = event->record;
  size_t length;
  int rc = 0;
  if (r == NULL)
    return notice(f, event->reason != NULL ? event->reason
                                           : "Invalid saved execution") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  length = strlen(r->data);
  if (!event->subagent) {
    if (strcmp(r->kind, "compaction.started") == 0)
      f->compacting = 1;
    else if (strcmp(r->kind, "compaction.completed") == 0 ||
             strcmp(r->kind, "compaction.failed") == 0 ||
             strcmp(r->kind, "turn.started") == 0 ||
             strcmp(r->kind, "turn.settled") == 0 ||
             strcmp(r->kind, "turn.failed") == 0 ||
             strcmp(r->kind, "turn.cancelled") == 0)
      f->compacting = 0;
  }
  if (!event->replay && strcmp(r->kind, "command.settled") == 0 &&
      r->code == CAI_OK && r->call_id != NULL &&
      f->history_command[0] != '\0' &&
      strcmp(r->call_id, f->history_command) == 0) {
    f->history_requested = f->history_loaded = f->current->history_loaded = 0;
    (void)sl_history_set_auto_add(f->ui->sl, 0);
    f->history_command[0] = '\0';
  }
  f->ui->presentation_event = 1;
  f->ui->presentation_timestamp_ns = r->timestamp_ns;
  /* Local command documents belong to the currently displayed region. They
   * must not reset an active review/worker's palette just to answer a query.
   * Ordinary parent execution resumes the parent's region. */
  if (r->subagent != NULL)
    f->child_theme_scope = strcmp(r->subagent, "review") == 0 ? 2 : 1;
  else if (strncmp(r->kind, "command.", 8U) != 0 &&
           strcmp(r->kind, "application.markdown") != 0 &&
           !(f->review_open && strcmp(r->kind, "turn.cancelled") == 0))
    f->child_theme_scope = 0;
  if (cai_cli_presenter_set_theme_scope(f->ui, f->child_theme_scope, error) !=
      CAI_OK)
    return CAI_ERR_TRANSPORT;
  if (event->subagent && r->subagent != NULL &&
      strcmp(r->subagent, "review") == 0 && r->parent_call_id != NULL &&
      !cai_cli_manual_review_call(r->parent_call_id) &&
      !f->review_header_shown) {
    if (markdown(f, CAI_CLI_REVIEW_START "\n\n",
                 strlen(CAI_CLI_REVIEW_START "\n\n")) != 0 ||
        cai_cli_presenter_finish_response(f->ui) != 0)
      return CAI_ERR_TRANSPORT;
    f->review_header_shown = f->review_open = 1;
  }
  if (event->replay && event->subagent && r->session_id != NULL &&
      strcmp(f->replay_child, r->session_id) != 0) {
    const char *heading =
        r->subagent != NULL && strcmp(r->subagent, "review") == 0
            ? "### Review subagent\n"
            : "### Subagent\n";
    snprintf(f->replay_child, sizeof(f->replay_child), "%s", r->session_id);
    if (markdown(f, heading, strlen(heading)) != 0 ||
        cai_cli_presenter_finish_response(f->ui) != 0)
      return CAI_ERR_TRANSPORT;
  }
  if (f->startup_request != 0U && f->startup_command && r->call_id != NULL &&
      strcmp(r->kind, "command.settled") == 0) {
    char source[129], tail;
    unsigned long long producer, request, item;
    if (sscanf(r->call_id, "command:%128[^:]:%llu:%llu:%llu%c", source,
               &producer, &request, &item, &tail) == 4 &&
        producer == f->startup_producer && request == f->startup_request &&
        strcmp(source, f->startup_session) == 0) {
      f->startup_request = 0U;
      if (r->code != CAI_OK)
        f->startup.stage = CAI_CLI_STARTUP_DONE;
    }
  }
  if (strncmp(r->kind, "tool.", 5U) == 0 ||
      strcmp(r->kind, "hosted.tool") == 0 ||
      strcmp(r->kind, "reasoning.receipt") == 0) {
    int handled = 0;
    if (cai_cli_replay_tools_command_output(r) &&
        (cai_cli_presenter_finish_response(f->ui) != 0 ||
         cai_cli_presenter_finish_reasoning(f->ui) != 0))
      return CAI_ERR_TRANSPORT;
    rc = cai_cli_replay_tools_follow(&f->tools, r, f->ui->tool_display,
                                     f->full_output, 72U, tool_markdown, f,
                                     &handled, error);
    if (rc == CAI_OK && r->tool_name != NULL &&
        strcmp(r->tool_name, "run_review") == 0) {
      const char *header = NULL;
      if (strcmp(r->kind, "tool.failed") == 0 && r->field != NULL &&
          strcmp(r->field, "message") == 0 && r->final && f->review_open) {
        f->review_open = 0;
        f->review_header_shown = 0;
        header = CAI_CLI_REVIEW_END "\n";
      }
      if (header != NULL && (markdown(f, header, strlen(header)) != 0 ||
                             cai_cli_presenter_finish_response(f->ui) != 0))
        return CAI_ERR_TRANSPORT;
    }
    if (rc == CAI_ERR_PROTOCOL || rc == CAI_ERR_LIMIT) {
      (void)notice(
          f, "Saved tool output is incomplete; inspect the JSONL export.");
      cai_error_cleanup(error);
      return CAI_OK;
    }
    return rc;
  }
  if (strcmp(r->kind, "command.input") == 0) {
    /* Accepted commands have the same bounded size as input admissions. Keep
     * their display atomic when worker deltas interleave journal records. */
    if (r->offset != f->command_length ||
        length > CAI_CLI_INPUT_TEXT_MAX - f->command_length)
      return cai_cli_error(error, CAI_ERR_PROTOCOL,
                           "invalid command input chunks");
    if (f->command == NULL) {
      f->command = malloc(CAI_CLI_INPUT_TEXT_MAX + 1U);
      if (f->command == NULL)
        return cai_cli_error(error, CAI_ERR_NOMEM,
                             "allocate accepted command input");
    }
    memcpy(f->command + f->command_length, r->data, length);
    f->command_length += length;
    f->command[f->command_length] = '\0';
    if (r->final) {
      /* Refresh only when the owner applies an explicit status command, not
       * when queued or replayed. Automatic observations remain debounced. */
      if (!event->replay && !event->subagent &&
          strcmp(f->command, "/status") == 0)
        cai_cli_status_refresh_branch(&f->status, f->target.workspace);
      if (strncmp(f->command, "/history", 8U) == 0 &&
          (f->command[8] == ' ' || f->command[8] == '\t') &&
          r->call_id != NULL && strlen(r->call_id) < sizeof(f->history_command))
        snprintf(f->history_command, sizeof(f->history_command), "%s",
                 r->call_id);
      rc = cai_cli_transcript_presenter_command(f->transcript, f->command);
      f->command_length = 0U;
    }
  } else if (strcmp(r->kind, "command.markdown") == 0 ||
             strcmp(r->kind, "application.markdown") == 0) {
    rc = markdown(f, r->data, length);
    if (rc == 0 && r->final)
      rc = cai_cli_presenter_finish_response(f->ui);
  } else if (strcmp(r->kind, "command.frontend") == 0)
    return action_chunk(f, r, event->replay, error);
  else if (strcmp(r->kind, "command.failed") == 0) {
    if (r->field == NULL)
      rc = notice(f, "ERROR: command failed");
    else {
      if (r->offset == 0U)
        cai_cli_tool_text_init(&f->tool_text, 0, 72U);
      rc = cai_cli_tool_text_write(&f->tool_text, r->data, length, r->final, 1,
                                   tool_markdown, f, error);
    }
    if (rc == 0 && r->final)
      rc = cai_cli_presenter_finish_response(f->ui);
  } else if (event->subagent && strcmp(r->kind, "review_markdown_delta") == 0)
    rc = markdown(f, r->data, length);
  else if (event->subagent &&
           strcmp(r->kind, "reasoning_summary_started") == 0) {
    if (r->final) {
      rc = cai_cli_presenter_finish_response(f->ui);
      if (rc == 0)
        rc = cai_cli_presenter_finish_reasoning(f->ui);
      cai_cli_reasoning_reset(&f->current->child_reasoning);
    }
  } else if (event->subagent && strcmp(r->kind, "reasoning_summary") == 0)
    rc = cai_cli_reasoning_feed(&f->current->child_reasoning, r->data, length,
                                cai_cli_presenter_reasoning_body, f->ui);
  else if (event->subagent && strcmp(r->kind, "review_markdown_end") == 0) {
    rc = cai_cli_presenter_finish_response(f->ui);
    if (rc == 0)
      rc = cai_cli_presenter_finish_reasoning(f->ui);
    if (rc == 0 && (r->parent_call_id == NULL ||
                    !cai_cli_manual_review_call(r->parent_call_id)))
      rc =
          markdown(f, CAI_CLI_REVIEW_END "\n", strlen(CAI_CLI_REVIEW_END "\n"));
    if (rc == 0)
      rc = cai_cli_presenter_finish_response(f->ui);
    f->review_open = 0;
    f->review_header_shown = 0;
    f->child_theme_scope = 0;
  } else if (event->subagent && strcmp(r->kind, "assistant_text_delta") == 0 &&
             (r->subagent == NULL || strcmp(r->subagent, "review") != 0))
    rc = cai_cli_presenter_text(f->ui, r->data, length);
  else if (event->subagent && strcmp(r->kind, "assistant_text_end") == 0) {
    rc = cai_cli_presenter_finish_response(f->ui);
    if (rc == 0)
      rc = cai_cli_presenter_finish_reasoning(f->ui);
    cai_cli_reasoning_end(&f->current->child_reasoning);
  } else if (strcmp(r->kind, "request.failed") == 0 ||
             strcmp(r->kind, "turn.failed") == 0 ||
             strcmp(r->kind, "compaction.failed") == 0 ||
             strcmp(r->kind, "request.retry") == 0 ||
             strcmp(r->kind, "compaction.started") == 0 ||
             strcmp(r->kind, "compaction.completed") == 0) {
    rc = cai_execution_record_write_markdown(r, 0, f->execution_sink, error);
    if (rc == CAI_OK && r->final)
      rc = cai_cli_presenter_finish_response(f->ui);
  } else if (strcmp(r->kind, "turn.cancelled") == 0) {
    const char *message =
        f->review_open ||
                (event->subagent &&
                 ((r->subagent != NULL && strcmp(r->subagent, "review") == 0) ||
                  (r->parent_call_id != NULL &&
                   cai_cli_manual_review_call(r->parent_call_id))))
            ? "**WARN: Review stopped.**\n"
            : "**WARN: Turn stopped.**\n";
    rc = cai_cli_presenter_finish_response(f->ui);
    if (rc == 0)
      rc = cai_cli_presenter_finish_reasoning(f->ui);
    if (rc == 0)
      rc = markdown(f, message, strlen(message));
    if (rc == 0)
      rc = cai_cli_presenter_finish_response(f->ui);
  }
  return rc == 0 ? CAI_OK
                 : cai_cli_error(error, rc > 0 ? rc : CAI_ERR_TRANSPORT,
                                 "render execution entry");
}

static int execution(void *context, const cai_app_execution_event *event,
                     cai_error *error) {
  frontend *f = context;
  int old_event = f->ui->presentation_event;
  unsigned long long old_stamp = f->ui->presentation_timestamp_ns;
  int rc;
  if (f->group->active != f || f->group->terminal_active ||
      f->presentation_parked)
    return CAI_OK;
  rc = execution_body(context, event, error);
  f->ui->presentation_event = old_event;
  f->ui->presentation_timestamp_ns = old_stamp;
  return rc;
}

static int connect_target(frontend *f) {
  int rc;
  cai_app_client_config config = f->target;
  config.event = observed;
  config.context = f;
  config.transcript = transcript;
  config.transcript_context = f;
  config.execution = execution;
  config.execution_context = f;
  config.prompt_history = prompt_history;
  config.prompt_history_context = f;
  config.catalog = catalog;
  config.catalog_context = f;
  rc = cai_app_client_open(&config, clock_ms(), &f->client, &f->error);
  if (rc == CAI_OK)
    rc = cai_app_client_network_handler(f->client, network_event, f, &f->error);
  if (rc == CAI_OK)
    rc = cai_cli_input_socket_reconnect(f->queue, f->client, &f->error);
  return rc;
}
static int theme_write(void *context, const void *bytes, size_t length,
                       cai_error *error) {
  (void)error;
  return markdown(context, bytes, length) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}
static int actions(frontend *f) {
  const char *line = f->action;
  int rc = 0;
  if (!f->action_field)
    return 0;
  f->action_field = 0;
  if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
    rc = goodbye(f);
    f->quit = 1;
  } else if (strncmp(line, "/theme ", 7U) == 0 ||
             strncmp(line, "/t ", 3U) == 0) {
    cai_cli_theme selected;
    char *argument = strchr(f->action, ' ') + 1U;
    char *end;
    const char *why;
    /* Slash arguments allow surrounding whitespace; theme identifiers do not.
     * Normalize the owned action, preserving the strict settings parser. */
    argument += strspn(argument, " \t");
    end = argument + strlen(argument);
    while (end > argument && (end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';
    why = cai_cli_theme_parse(argument, &selected);
    if (why != NULL)
      rc = notice(f, why);
    else if (cai_cli_presenter_set_theme(f->ui, &selected, &f->error) != CAI_OK)
      return -1;
    else {
      char message[256];
      snprintf(message, sizeof(message), "Theme changed to %s:%s.\n%s",
               cai_cli_theme_prompt_name(&selected),
               selected.ascii ? "ascii" : selected.markdown,
               cai_environment_get(f->options->environment, "NO_COLOR") != NULL
                   ? "\n`NO_COLOR` keeps the prompt plain and Markdown ANSI "
                     "colors disabled.\n"
                   : "");
      rc = markdown(f, message, strlen(message));
    }
    if (why == NULL && rc == 0)
      f->theme = selected;
  } else if (strcmp(line, "/theme") == 0 || strcmp(line, "/t") == 0) {
    cai_sink_callbacks callbacks;
    cai_sink *sink = NULL;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.write = theme_write;
    callbacks.context = f;
    rc = cai_sink_from_callbacks(&callbacks, &sink, &f->error);
    if (rc == CAI_OK)
      rc = cai_cli_theme_describe(&f->theme, sink, &f->error);
    cai_sink_close(sink);
  } else if (cai_cli_command_is_theme(line)) {
    const char *argument = line + strcspn(line, " \t");
    char *end;
    int valid = 1;
    int scope = strncmp(line, "/subagent-", 10U) == 0 ? 1
                : strncmp(line, "/review-", 8U) == 0  ? 2
                                                      : 0;
    char message[160];
    argument += strspn(argument, " \t");
    if (*argument != '\0') {
      long long selected;
      errno = 0;
      selected = strtoll(argument, &end, 10);
      end += strspn(end, " \t");
      if (errno != 0 || end == argument || *end != '\0') {
        valid = 0;
        rc = notice(f, "ERROR: Theme offset requires a signed 64-bit integer.");
      } else {
        f->theme_offsets[scope] = selected;
        f->ui->theme_offsets[scope] = selected;
        rc = cai_cli_presenter_set_theme(f->ui, &f->theme, &f->error);
      }
    }
    if (rc == 0 && valid) {
      snprintf(message, sizeof(message), "%s theme offset: **%lld**.\n",
               scope == 1   ? "Subagent"
               : scope == 2 ? "Review"
                            : "Parent",
               f->theme_offsets[scope]);
      rc = markdown(f, message, strlen(message));
    }
  } else if (strncmp(line, "/output", 7U) == 0) {
    const char *argument = line + 7U;
    const char *message;
    argument += strspn(argument, " \t");
    if (*argument != '\0' && strcmp(argument, "full") != 0 &&
        strcmp(argument, "compact") != 0)
      rc = notice(f, "ERROR: use /output compact or /output full");
    else {
      if (*argument != '\0') {
        f->full_output = strcmp(argument, "full") == 0;
        rc = cai_tool_display_set_mode(f->ui->tool_display,
                                       f->full_output ? CAI_TOOL_OUTPUT_FULL
                                                      : CAI_TOOL_OUTPUT_COMPACT,
                                       &f->error);
      }
      f->ui->tool_output = f->full_output ? "full" : "compact";
      message = f->full_output ? "Tool output: **full**. Use `/output compact` "
                                 "or `/output full`.\n"
                               : "Tool output: **compact**. Use `/output "
                                 "compact` or `/output full`.\n";
      if (rc == 0)
        rc = markdown(f, message, strlen(message));
      if (rc == 0 && *argument != '\0') {
        message = "**Tool output setting** applies from the next tool call.\n";
        rc = markdown(f, message, strlen(message));
      }
    }
  } else if (strcmp(line, "/shell") == 0 || strcmp(line, "!sh") == 0)
    f->shell = 1;
  else if (strcmp(line, "/shellshare") == 0 || strcmp(line, "!sh+") == 0)
    f->shell = 4;
  else if (cai_cli_command_pager(line) == 2)
    f->shell = 2;
  else if (cai_cli_command_pager(line) == 1)
    f->shell = 3;
  else
    rc = notice(f, "Unsupported frontend action.");
  f->action_length = 0U;
  if (rc == 0)
    rc = cai_cli_presenter_finish_response(f->ui);
  return rc;
}

static int event_ready(sl_t *sl, const sl_watch_event_t *event, void *context) {
  frontend *f = context;
  char bytes[128];
  uint64_t ticks;
  ssize_t n;
  (void)sl;
  if (event->fd == f->wake[0]) {
    do {
      n = read(f->wake[0], bytes, sizeof(bytes));
    } while (n > 0 || (n < 0 && errno == EINTR));
  } else if (event->fd == f->timer)
    (void)read(f->timer, &ticks, sizeof(ticks));
  if (service(f) != 0) {
    f->failed = 1;
    f->quit = 1;
  }
  if ((f->shell || f->group->switching) && !f->transcript_entry &&
      park_presentation(f, NULL) != CAI_OK) {
    f->failed = f->quit = 1;
  }
  if (f->quit ||
      ((f->shell || f->group->switching) && f->presentation_parked) ||
      f->switching)
    return sl_suspend(f->ui->sl);
  return SL_OK;
}
/** Native idle follows buffered keys, receipt resolution and deferred edits.
 * Socket watches must never substitute for this editor-owned boundary. */
static void editor_idle(sl_t *editor, void *context) {
  frontend *f = context;
  frontend *parked;
  cai_cli_prompt_listener_idle(f->prompt_listener);
  if (f->queue != NULL &&
      cai_cli_input_socket_idle(f->queue, editor, &f->error) != CAI_OK) {
    f->failed = f->quit = 1;
    (void)sl_suspend(editor);
  }
  /* Parked editors have no input loop of their own. Their callback-driven
   * queue offers and receipts still settle at the active editor's idle seam,
   * after all watched sockets/native queue callbacks have been processed. */
  for (parked = f->group->first; parked != NULL; parked = parked->next) {
    if (parked != f && !parked->parked_failed && parked->queue != NULL &&
        cai_cli_input_socket_idle(parked->queue, parked->ui->sl,
                                  &parked->error) != CAI_OK) {
      parked->parked_failed = 1;
      parked->ready = 0;
    }
  }
  if (!f->failed && f->handoff_notice[0] != '\0' && f->queue != NULL) {
    sl_queue_backend_info_t info;
    if (sl_queue_backend_info(editor, &info) != SL_OK) {
      f->failed = f->quit = 1;
      (void)sl_suspend(editor);
    } else if (info.synchronized) {
      char byte = 'h';
      ssize_t n;
      int rc = f->handoff_notice_error
                   ? notice(f, f->handoff_notice)
                   : markdown(f, f->handoff_notice, strlen(f->handoff_notice));
      if (rc != 0 || cai_cli_presenter_finish_response(f->ui) != 0) {
        f->failed = f->quit = 1;
        (void)sl_suspend(editor);
        return;
      }
      f->handoff_notice[0] = '\0';
      do {
        n = write(f->wake[1], &byte, 1U);
      } while (n < 0 && errno == EINTR);
    }
  }
}
static int watch(frontend *f, sl_watch_id_t *id, int *old,
                 const struct pollfd *fd) {
  unsigned int flags = (fd->events & POLLIN ? SL_WATCH_READ : 0U) |
                       (fd->events & POLLOUT ? SL_WATCH_WRITE : 0U);
  if (*old != fd->fd && *old >= 0) {
    (void)sl_watch_remove(f->ui->sl, *id);
    *old = -1;
  }
  if (fd->fd < 0)
    return 0;
  if (*old < 0) {
    if (sl_watch_add(f->ui->sl, fd->fd, flags, event_ready, f, id) != SL_OK)
      return -1;
    *old = fd->fd;
    return 0;
  }
  return sl_watch_modify(f->ui->sl, *id, flags) == SL_OK ? 0 : -1;
}
static int interests(frontend *f) {
  frontend *other;
  struct pollfd descriptor;
  struct itimerspec timer;
  uint64_t deadline = 0U, boot_deadline = 0U, now = clock_ms();
  const cai_app_view *view = NULL;
  if (f->group->active == f)
    for (other = f->group->first; other != NULL; other = other->next) {
      /* A parked producer can enqueue transport writes after prepare(), in
       * the native idle callback. Wake the editor that owns the poll loop. */
      if (other->queue != NULL)
        cai_cli_input_socket_set_wakeup(other->queue, f->wake[1]);
      if (cai_cli_prompt_listener_watch(other->prompt_listener, f->ui->sl,
                                        &f->error) != CAI_OK ||
          network_progress(other) != 0)
        return -1;
    }
  if (f->client != NULL) {
    if (cai_app_client_prepare(f->client, &descriptor, &deadline, &f->error) !=
            CAI_OK ||
        watch(f, &f->socket_watch, &f->socket_fd, &descriptor) != 0)
      return -1;
    (void)cai_app_client_get_view(f->client, &view, NULL);
    if (view != NULL && view->available && view->metrics.turn_active &&
        (deadline == 0U || deadline > now + 1000U))
      deadline = now + 1000U;
  }
  if (f->boot != NULL) {
    if (f->boot->prepare(f->boot->context, &descriptor, &boot_deadline,
                         &f->error) != CAI_OK ||
        watch(f, &f->boot_watch, &f->boot_fd, &descriptor) != 0)
      return -1;
    if (boot_deadline != 0U && (deadline == 0U || boot_deadline < deadline))
      deadline = boot_deadline;
  }
  memset(&timer, 0, sizeof(timer));
  if (workspace_interests(f, &deadline) != 0)
    return -1;
  if (deadline != 0U) {
    uint64_t remaining = deadline > now ? deadline - now : 1U;
    timer.it_value.tv_sec = (time_t)(remaining / 1000U);
    timer.it_value.tv_nsec = (long)(remaining % 1000U) * 1000000L;
  }
  return timerfd_settime(f->timer, 0, &timer, NULL) == 0 ? 0 : -1;
}

/** Feed explicit CLI intents through the editor's native remote queue. A
 * durable offer receipt precedes the next input; state-changing commands also
 * require their application record, including across a session switch. */
static int startup_input(frontend *f) {
  char *text = NULL;
  int rc, control;
  sl_queue_request_t request;
  sl_queue_request_id_t id;
  if (cai_cli_prompt_listener_active(f->prompt_listener) || !f->ready ||
      !f->subscribed || f->replaying || f->startup_request != 0U ||
      !f->state.transcript.active || f->state.transcript.subscribing ||
      f->state.transcript.replaying ||
      f->state.transport.controller != f->state.transport.attachment ||
      f->startup.stage >= CAI_CLI_STARTUP_DONE)
    return 0;
  rc =
      cai_cli_startup_next(&f->startup, f->options, &text, &control, &f->error);
  if (rc != CAI_OK || text == NULL)
    return rc;
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_OFFER;
  request.mode = SL_PROMPT_QUEUE_MODE_QUEUED;
  request.text = text;
  f->startup_command = text[0] == '/' || text[0] == '!';
  f->startup_control = control;
  f->startup_feeding = 1;
  {
    int submitted = sl_queue_backend_request(f->ui->sl, &request, &id);
    f->startup_feeding = 0;
    if (submitted != SL_OK) {
      free(text);
      f->startup.stage = CAI_CLI_STARTUP_DONE;
      return notice(
          f, "Startup input handoff failed; its native receipt was retained.");
    }
  }
  free(text);
  f->startup_request = cai_cli_input_socket_last_issued(f->queue);
  f->startup_producer = f->state.producer;
  snprintf(f->startup_session, sizeof(f->startup_session), "%s",
           f->state.session);
  if (f->startup_control) {
    size_t i;
    for (i = 0U; i < CAI_CLI_STARTUP_INSTRUCTIONS; ++i)
      if (f->hidden_recall[i].request == 0U) {
        f->hidden_recall[i].request = f->startup_request;
        f->hidden_recall[i].producer = f->startup_producer;
        snprintf(f->hidden_recall[i].session,
                 sizeof(f->hidden_recall[i].session), "%s", f->startup_session);
        break;
      }
  }
  return 0;
}
/** A verified baseline permits read-only reconciliation of uncertain native
 * receipts. Ask for their original outcomes; never repeat an offer/edit. */
static int resolve_receipts(frontend *f) {
  sl_queue_backend_info_t info;
  size_t i, count;
  if (!f->resolve_receipts ||
      sl_queue_backend_info(f->ui->sl, &info) != SL_OK || !info.synchronized)
    return 0;
  f->resolve_receipts = 0;
  count = info.pending_count;
  for (i = 0U; i < count; ++i) {
    sl_queue_pending_info_t pending;
    if (sl_queue_backend_pending(f->ui->sl, i, &pending) == SL_OK &&
        !pending.pending && pending.status == SL_QUEUE_UNKNOWN) {
      sl_queue_request_t request;
      sl_queue_request_id_t id;
      memset(&request, 0, sizeof(request));
      request.operation = SL_QUEUE_RESOLVE;
      request.target_request_id = pending.request.request_id;
      if (sl_queue_backend_request(f->ui->sl, &request, &id) != SL_OK)
        return notice(f, "Could not resolve an uncertain queue receipt; "
                         "its original request remains retained.");
    }
  }
  return 0;
}
static int service(frontend *f) {
  int rc, ready = 0;
  if (workspace_progress(f) != 0)
    return -1;
  if (f->boot != NULL) {
    cai_app_client_config target = f->target;
    rc =
        f->boot->pump(f->boot->context, clock_ms(), &target, &ready, &f->error);
    if (rc != CAI_OK)
      return -1;
    if (ready == CAI_CLI_FRONTEND_DETACH) {
      f->expected_close = f->quit = 1;
      return 0;
    }
    if (ready == CAI_CLI_FRONTEND_STOPPED) {
      f->expected_close = 1;
      if (f->client == NULL) {
        f->quit = 1;
        return 0;
      }
    }
    if (ready && f->client == NULL) {
      char *workspace;
      if ((ready != CAI_CLI_FRONTEND_EXISTING &&
           ready != CAI_CLI_FRONTEND_STARTED) ||
          target.workspace == NULL || target.expected_instance == NULL)
        return cai_cli_error(&f->error, CAI_ERR_PROTOCOL,
                             "startup target was not verified");
      workspace = strdup(target.workspace);
      if (workspace == NULL)
        return cai_cli_error(&f->error, CAI_ERR_NOMEM,
                             "copy verified startup workspace");
      /* A named selector can resolve outside the provisional cwd. Keep every
       * editor consumer on that verified directory before accepting input. */
      free(f->workspace);
      f->workspace = workspace;
      f->target = target;
      f->target.workspace = f->workspace;
      f->target.runtime_directory = f->runtime_directory;
      memcpy(f->instance, target.expected_instance, sizeof(f->instance));
      f->target.expected_instance = f->instance;
      cai_cli_status_init(&f->status, f->workspace, getenv("HOME"));
      (void)cai_cli_status_fields_parse(f->options->status_fields,
                                        &f->status.fields);
      if (cai_cli_completions_open(&f->current->completions, f->ui->sl,
                                   f->workspace, completion_catalog,
                                   f) != SL_OK)
        return cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                             "select verified completion directory");
      f->startup.stage = ready == CAI_CLI_FRONTEND_STARTED
                             ? CAI_CLI_STARTUP_REVIEW_AND_FIX
                             : CAI_CLI_STARTUP_MODEL;
      if (ready == CAI_CLI_FRONTEND_STARTED)
        f->startup_select = 0;
      if (connect_target(f) != CAI_OK)
        return -1;
    }
  }
  if (f->client != NULL) {
    rc = cai_app_client_pump(f->client, clock_ms(), &f->error);
    if (rc != CAI_OK && f->expected_close) {
      f->quit = 1;
      return 0;
    }
    if (rc != CAI_OK)
      return -1;
    (void)cai_app_client_get_state(f->client, &f->state, NULL);
    if (f->state.transport.phase == CAI_WORKSPACE_DISCONNECTED ||
        f->state.transport.phase == CAI_WORKSPACE_FAILED) {
      if (f->expected_close) {
        f->quit = 1;
        return 0;
      }
      return cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                           "workspace server disconnected");
    }
    if (f->pending_context) {
      const cai_app_view *view = NULL;
      /* Status and queue reset are independent ordered channels. The new
       * completed status can precede RESET, so reconcile the cached view
       * after the pump instead of waiting for another status change. */
      if (cai_app_client_get_view(f->client, &view, &f->error) != CAI_OK)
        return -1;
      if (view != NULL && view->available &&
          view->conversation != f->current->epoch) {
        snprintf(f->next_session, sizeof(f->next_session), "%s",
                 view->session_id);
        f->switching = 1;
      }
      return status(f) == 0 ? interests(f) : -1;
    }
    if (f->state.transport.phase == CAI_WORKSPACE_CONNECTED && !f->bound &&
        !f->awaiting_bind) {
      if (f->startup_select)
        rc = cai_app_client_select(
            f->client, ++f->bind_request,
            f->options->new_session ? NULL : f->options->resume_id, &f->error);
      else
        rc = cai_app_client_bind(
            f->client, ++f->bind_request, 0U, f->current->producer,
            f->current->session[0] != '\0' ? f->current->session : NULL,
            &f->error);
      if (rc != CAI_OK)
        return -1;
      f->awaiting_bind = 1;
      f->startup_select = 0;
    }
    if (f->bound && !f->subscribed && f->handoff_notice[0] == '\0') {
      rc = cai_app_client_subscribe(
          f->client, ++f->subscribe_request, f->state.conversation,
          f->state.session,
          f->resume_collection[0] != '\0' ? f->resume_collection : NULL,
          f->resume_position, f->resume_sequence,
          f->resume_collection[0] != '\0' ? -1LL : f->options->replay_turns,
          &f->error);
      if (rc != CAI_OK)
        return -1;
      f->subscribed = 1;
      f->replay_finished = 0;
    }
    if (f->bound && !f->history_requested) {
      rc = cai_app_client_prompt_history(
          f->client, ++f->subscribe_request, f->state.conversation,
          f->state.session, (unsigned int)f->options->prompt_history_limit,
          &f->error);
      if (rc != CAI_OK)
        return -1;
      f->history_requested = 1;
    }
    if (f->history_warning[0] != '\0') {
      if (notice(f, f->history_warning) != 0)
        return -1;
      f->history_warning[0] = '\0';
    }
    if (resolve_receipts(f) != 0 || actions(f) != 0)
      return -1;
    if (startup_input(f) != 0)
      return -1;
    if (f->prompt_listener != NULL && !f->prompt_announced && f->ready) {
      char text[256];
      snprintf(text, sizeof(text), "TCP prompt listener: %s",
               cai_cli_prompt_listener_address(f->prompt_listener));
      if (notice(f, text) != 0)
        return -1;
      f->prompt_announced = 1;
    }
  }
  return status(f) == 0 ? interests(f) : -1;
}
static int quit_key(sl_t *sl, sl_key_t key, void *context,
                    sl_key_action_t *action) {
  frontend *f = context;
  (void)sl;
  (void)key;
  /* Unfinished remote input must never become saved prompt recall on exit.
   * Separate receipt buffers leave the user's ordinary draft untouched. */
  cai_cli_prompt_listener_cancel(f->prompt_listener);
  if (sl_buffer(sl)[0] != '\0') {
    if (f->exit_save != 0U) {
      *action = SL_KEY_ACTION_HANDLED;
      return SL_OK;
    }
    if (!f->ready || f->history_limit == 0U ||
        f->history_request == UINT64_MAX) {
      *action = SL_KEY_ACTION_HANDLED;
      return sl_set_status_message(
          sl,
          "Draft retained: history is unavailable; clear the draft to quit.");
    }
    f->exit_draft = strdup(sl_buffer(sl));
    if (f->exit_draft == NULL)
      return SL_ERROR_NOMEM;
    f->exit_save = ++f->history_request;
    f->exit_view = f->current;
    if (cai_app_client_save_prompt(f->client, f->exit_save,
                                   f->state.transport.generation, f->exit_draft,
                                   &f->error) != CAI_OK) {
      f->exit_save = 0U;
      free(f->exit_draft);
      f->exit_draft = NULL;
      *action = SL_KEY_ACTION_HANDLED;
      return sl_set_status_message(
          sl, "Draft retained: could not save; clear it to quit.");
    }
    {
      char byte = 'h';
      (void)write(f->wake[1], &byte, 1U);
    }
    *action = SL_KEY_ACTION_HANDLED;
    return SL_OK;
  }
  if (goodbye(f) != 0)
    return SL_ERROR_IO;
  f->quit = 1;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}
/** Keep native forward-delete while editing; an empty Ctrl-D follows the
 * same local goodbye/detach path as Alt-Q before Softline leaves its surface.
 */
static int eof_key(sl_t *sl, sl_key_t key, void *context,
                   sl_key_action_t *action) {
  if (sl_buffer(sl)[0] != '\0') {
    *action = SL_KEY_ACTION_PASS;
    return SL_OK;
  }
  return quit_key(sl, key, context, action);
}
static int cancel_key(sl_t *sl, sl_key_t key, void *context,
                      sl_key_action_t *action) {
  frontend *f = context;
  (void)sl;
  (void)key;
  if (cai_cli_prompt_listener_active(f->prompt_listener)) {
    cai_cli_prompt_listener_cancel(f->prompt_listener);
    *action = SL_KEY_ACTION_HANDLED;
    return SL_OK;
  }
  /* Match Softline's chat adapter: CANCEL owns stopped-backlog and draft
   * semantics; the outer owner then cancels the remote foreground operation. */
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}
static int enter_key(sl_t *sl, sl_key_t key, void *context,
                     sl_key_action_t *action) {
  frontend *f = context;
  (void)key;
  if (cai_cli_prompt_listener_draft(f->prompt_listener)) {
    *action = SL_KEY_ACTION_HANDLED;
    return SL_OK;
  }
  if (!sl->history_search_active(sl) &&
      strncmp(sl_buffer(sl), "/workspace", 10U) == 0 &&
      (sl_buffer(sl)[10] == '\0' || sl_buffer(sl)[10] == ' ' ||
       sl_buffer(sl)[10] == '\t')) {
    sl_prompt_queue_entry_info_t draft;
    if (sl_prompt_queue_draft_info(sl, &draft) != SL_OK)
      return SL_ERROR_IO;
    if (!draft.id) {
      int rc = workspace_begin(f, sl_buffer(sl));
      if (rc < 0)
        return SL_ERROR_IO;
      if (rc > 0) {
        *action = SL_KEY_ACTION_HANDLED;
        return SL_OK;
      }
      if (sl_set_buffer(sl, "") != SL_OK)
        return SL_ERROR_IO;
      *action = SL_KEY_ACTION_HANDLED;
      return SL_OK;
    }
  }
  if (!sl->history_search_active(sl) && (strcmp(sl_buffer(sl), "/quit") == 0 ||
                                         strcmp(sl_buffer(sl), "/exit") == 0)) {
    sl_prompt_queue_entry_info_t draft;
    if (sl_prompt_queue_draft_info(sl, &draft) != SL_OK)
      return SL_ERROR_IO;
    if (!draft.id) {
      if (cai_cli_transcript_presenter_command(f->transcript, sl_buffer(sl)) !=
          0)
        return SL_ERROR_IO;
      if (sl_set_buffer(sl, "") != SL_OK)
        return SL_ERROR_IO;
      if (goodbye(f) != 0)
        return SL_ERROR_IO;
      f->quit = 1;
      *action = SL_KEY_ACTION_SUSPEND;
      return SL_OK;
    }
  }
  if (f->ready && (strcmp(sl_buffer(sl), "/takeover") == 0 ||
                   strcmp(sl_buffer(sl), "/shutdown") == 0)) {
    f->control = strcmp(sl_buffer(sl), "/takeover") == 0 ? 1 : 2;
    if (sl_set_buffer(sl, "") != SL_OK)
      return SL_ERROR_IO;
    *action = SL_KEY_ACTION_SUSPEND;
    return SL_OK;
  }
  *action = f->ready ? SL_KEY_ACTION_PASS : SL_KEY_ACTION_HANDLED;
  if (!f->ready)
    return sl_set_status_message(sl, "Connecting; your draft is retained.");
  return SL_OK;
}

/** Request local handoff at a complete transcript entry, without submitting
 * the draft or waiting for the owner's current generation to finish. */
static int shell_key(sl_t *sl, sl_key_t key, void *context,
                     sl_key_action_t *action) {
  frontend *f = context;
  const cai_app_view *view = NULL;
  (void)key;
  *action = SL_KEY_ACTION_HANDLED;
  if (!f->ready || !f->history_loaded || f->exit_save != 0U ||
      f->group->discovery != NULL || f->group->candidate != NULL ||
      cai_app_client_get_view(f->client, &view, NULL) != CAI_OK ||
      view == NULL || !view->available || f->replaying || !f->subscribed ||
      f->state.transcript.subscribing)
    return sl_set_status_message(
        sl, "Wait for workspace attachment before opening a local shell.");
  f->shell = 1;
  f->action_call[0] = '\0';
  if (!f->transcript_entry) {
    if (park_presentation(f, NULL) != CAI_OK)
      return SL_ERROR_IO;
    *action = SL_KEY_ACTION_SUSPEND;
  }
  return SL_OK;
}
/** Park the complete native editor, including its draft, undo state and queue
 * receipts. Only the active view participates in the owner's poll loop. */
static void park_view(frontend *f) {
  frontend_view *v = f->current;
  if (v == NULL)
    return;
  v->queue = f->queue != NULL ? f->queue : v->queue;
  v->transcript = f->transcript;
  v->tools = f->tools;
  v->socket_watch = f->socket_watch;
  v->boot_watch = f->boot_watch;
  v->timer_watch = f->timer_watch;
  v->wake_watch = f->wake_watch;
  v->socket_fd = f->socket_fd;
  v->boot_fd = f->boot_fd;
}

/** Clipboard capture belongs to this frontend, using its configured cache
 * rather than the attached owner's paths or Softline's unrelated defaults. */
static int configure_clipboard(frontend *f) {
  const char *directory = f->options->clipboard_dir;
  char *path;
  size_t length;
  int rc;
  if (directory == NULL || directory[0] == '\0')
    return CAI_OK;
  length = strlen(directory);
  if (length > (size_t)-1 - 3U)
    return cai_cli_error(&f->error, CAI_ERR_LIMIT,
                         "clipboard directory is too long");
  path = malloc(length + 3U);
  if (path == NULL)
    return cai_cli_error(&f->error, CAI_ERR_NOMEM,
                         "allocate clipboard path template");
  memcpy(path, directory, length);
  memcpy(path + length, "/*", 3U);
  rc = sl_set_image_paste_path_template(f->ui->sl, path);
  free(path);
  return rc == SL_OK ? CAI_OK
                     : cai_cli_error(&f->error, CAI_ERR_INVALID,
                                     "configure frontend clipboard path");
}

static int select_view(frontend *f, const char *session) {
  frontend_view *v;
  sl_config_t config;
  int fresh = 0;
  int same_session = f->current != NULL && session != NULL &&
                     strcmp(f->current->session, session) == 0;
  f->child_theme_scope = 0;
  if (f->current != NULL || session != NULL)
    f->greeting_resumed = session != NULL;
  if (f->current != NULL) {
    park_view(f);
    cai_cli_transcript_presenter_reset(f->transcript);
    if (!same_session) {
      cai_cli_replay_tools_close(f->tools);
      f->current->tools = f->tools = NULL;
    }
    if (sl_output_stream_end(f->ui->sl) != SL_OK)
      return -1;
  }
  for (v = f->views; v != NULL; v = v->next)
    if (session != NULL && strcmp(v->session, session) == 0)
      break;
  if (v == NULL) {
    v = calloc(1U, sizeof(*v));
    if (v == NULL)
      return cai_cli_error(&f->error, CAI_ERR_NOMEM, "allocate session editor");
    v->next = f->views;
    f->views = v;
    v->socket_fd = v->boot_fd = -1;
    if (session != NULL)
      snprintf(v->session, sizeof(v->session), "%s", session);
    fresh = 1;
  }
  f->current = v;
  f->ui = &v->ui;
  f->queue = v->queue;
  f->transcript = v->transcript;
  f->tools = v->tools;
  f->socket_watch = v->socket_watch;
  f->boot_watch = v->boot_watch;
  f->timer_watch = v->timer_watch;
  f->wake_watch = v->wake_watch;
  f->socket_fd = v->socket_fd;
  f->boot_fd = v->boot_fd;
  if (fresh) {
    f->ui->interactive = 1;
    /* A new output surface starts on the terminal's current line boundary.
     * Banner bytes below replace this baseline, including an unfinished line.
     * Requiring two newlines from zero added two blank rows before replay. */
    f->ui->feed_breaks = 1;
    f->ui->utc = f->options->utc;
    f->ui->tool_output = f->full_output ? "full" : "compact";
    memcpy(f->ui->theme_offsets, f->theme_offsets, sizeof(f->theme_offsets));
    f->ui->theme = f->theme;
    cai_cli_presenter_theme_for_scope(f->ui, &f->theme, 0,
                                      &f->ui->render_theme);
    if (cai_cli_input_socket_open(f->client, 0U, &f->queue, &f->error) !=
        CAI_OK)
      return -1;
    v->queue = f->queue;
    cai_cli_input_socket_set_wakeup(f->queue, f->wake[1]);
    cai_cli_input_socket_prompt(f->queue, save_submission, f);
    sl_config_init(&config);
    config.prompt_theme =
        getenv("NO_COLOR") != NULL ? SL_PROMPT_THEME_PLAIN : f->theme.prompt;
    config.line_max_len = CAI_CLI_INPUT_TEXT_MAX;
    cai_cli_input_adapter_configure(cai_cli_input_socket_adapter(f->queue),
                                    &config);
    f->ui->sl = sl_create_with_config(&config);
    if (f->ui->sl == NULL || configure_clipboard(f) != CAI_OK ||
        cai_cli_input_adapter_bind(cai_cli_input_socket_adapter(f->queue),
                                   f->ui->sl, &f->error) != CAI_OK ||
        sl_set_statusline(f->ui->sl, 1, 0) != SL_OK ||
        sl_history_set_max_len(f->ui->sl, f->options->prompt_history_limit) !=
            SL_OK ||
        sl_history_set_auto_add(f->ui->sl, 0) != SL_OK ||
        sl_set_prompt_queue_profile(
            f->ui->sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) != SL_OK ||
        sl_set_prompt_theme(f->ui->sl, config.prompt_theme) != SL_OK ||
        sl_set_idle_callback(f->ui->sl, editor_idle, f) != SL_OK ||
        cai_cli_completions_open(&v->completions, f->ui->sl,
                                 f->target.workspace, completion_catalog,
                                 f) != SL_OK ||
        sl_watch_add(f->ui->sl, f->wake[0], SL_WATCH_READ, event_ready, f,
                     &f->wake_watch) != SL_OK ||
        sl_watch_add(f->ui->sl, f->timer, SL_WATCH_READ, event_ready, f,
                     &f->timer_watch) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ALT_BASE + 'q', quit_key, f) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_CTRL_D, eof_key, f) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ALT_BASE + 's', shell_key, f) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ESCAPE, cancel_key, f) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ENTER, enter_key, f) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ALT_ENTER, enter_key, f) != SL_OK ||
        cai_cli_presenter_renderer_create(f->ui, &f->ui->render_theme,
                                          &f->ui->renderer,
                                          &f->error) != CAI_OK ||
        cai_cli_presenter_geometry(f->ui) != 0 ||
        cai_cli_presenter_tool_display_init(f->ui, &f->error) != CAI_OK ||
        cai_cli_transcript_presenter_open(f->ui, &f->transcript, &f->error) !=
            CAI_OK)
      return -1;
  } else {
    memcpy(f->ui->theme_offsets, f->theme_offsets, sizeof(f->theme_offsets));
    f->ui->tool_output = f->full_output ? "full" : "compact";
    if (cai_cli_input_socket_reconnect(f->queue, f->client, &f->error) !=
            CAI_OK ||
        cai_cli_presenter_set_theme(f->ui, &f->theme, &f->error) != CAI_OK ||
        cai_cli_completions_open(&v->completions, f->ui->sl,
                                 f->target.workspace, completion_catalog,
                                 f) != SL_OK)
      return -1;
  }
  f->pending_context = f->switching = f->ready = f->bound = f->awaiting_bind =
      f->subscribed = f->welcomed = 0;
  f->resume_collection[0] = '\0';
  f->resume_position = f->resume_sequence = 0U;
  if (same_session) {
    snprintf(f->resume_collection, sizeof(f->resume_collection), "%s",
             v->displayed_collection);
    f->resume_position = v->displayed_position;
    f->resume_sequence = v->displayed_sequence;
  }
  f->resume_actions = 0;
  f->presentation_parked = f->transcript_entry = f->shell = 0;
  f->history_requested = f->history_loaded = v->history_loaded;
  f->history_limit = v->history_loaded
                         ? v->history_limit
                         : (unsigned int)f->options->prompt_history_limit;
  f->history_length = 0U;
  f->action_field = f->action_length = f->command_length = f->review_open = 0U;
  f->compacting = 0;
  park_view(f);
  return sl_output_stream_begin(f->ui->sl) == SL_OK ? 0 : -1;
}

static void prompt_scope(void *context, cai_cli_prompt_scope *scope) {
  frontend *f = context;
  sl_queue_backend_info_t info;
  if (f->client != NULL)
    (void)cai_app_client_get_state(f->client, &f->state, NULL);
  scope->attachment = f->state.transport.attachment;
  scope->generation = f->state.transport.generation;
  scope->epoch = f->state.conversation;
  memcpy(scope->instance, f->state.transport.instance, sizeof(scope->instance));
  scope->editor = f->ui != NULL ? f->ui->sl : NULL;
  scope->parked = f->group->active != f;
  scope->ready = f->ready && f->bound && !f->pending_context && !f->switching &&
                 !f->replaying && f->state.ready &&
                 f->state.transport.phase == CAI_WORKSPACE_CONNECTED &&
                 scope->attachment != 0U &&
                 scope->attachment == f->state.transport.controller &&
                 scope->editor != NULL &&
                 sl_queue_backend_info(scope->editor, &info) == SL_OK &&
                 info.synchronized;
}
static void prompt_notice(void *context, const char *message) {
  frontend *f = context;
  if (f->client != NULL && f->state.bound) {
    cai_app_network_event diagnostic;
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.header.kind = CAI_PROTOCOL_NETWORK;
    diagnostic.header.attachment = f->state.transport.attachment;
    diagnostic.header.generation = f->state.transport.generation;
    diagnostic.header.conversation = f->state.conversation;
    diagnostic.operation = CAI_APP_NETWORK_NOTICE;
    diagnostic.reason = message;
    (void)cai_app_client_network_write(f->client, &diagnostic, NULL);
  }
  if (f->group->active == f) {
    if (notice(f, message) != 0)
      f->failed = f->quit = 1;
  } else {
    snprintf(f->handoff_notice, sizeof(f->handoff_notice), "%s", message);
    f->handoff_notice_error = 1;
  }
}
static void prompt_changed(void *context) {
  frontend *f = context;
  frontend *active = f->group->active;
  char byte = 'p';
  ssize_t written;
  if (active == NULL)
    return;
  do {
    written = write(active->wake[1], &byte, 1U);
  } while (written < 0 && errno == EINTR);
}
static int network_scope(frontend *f, const cai_protocol_header *h) {
  cai_app_client_state state;
  if (f->client == NULL ||
      cai_app_client_get_state(f->client, &state, NULL) != CAI_OK)
    return 0;
  return state.transport.phase == CAI_WORKSPACE_CONNECTED &&
         state.transport.attachment == h->attachment &&
         state.transport.controller == h->attachment &&
         state.transport.generation == h->generation &&
         state.conversation == h->conversation;
}
static int network_event(void *context, const cai_app_network_event *event,
                         cai_error *error) {
  frontend *f = context;
  if (event->operation == CAI_APP_NETWORK_NOTICE) {
    prompt_notice(f, event->reason);
    return f->failed ? CAI_ERR_TRANSPORT : CAI_OK;
  }
  if (event->operation == CAI_APP_NETWORK_PUBLISHER) {
    char *text = malloc(4608U);
    int rc;
    if (text == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM,
                           "format multicast startup notice");
    snprintf(text, 4608U, "Workspace event multicast: %s\n%s", event->address,
             event->socat);
    prompt_notice(f, text);
    free(text);
    rc = f->failed ? CAI_ERR_TRANSPORT : CAI_OK;
    return rc;
  }
  if (event->operation == CAI_APP_NETWORK_CANCEL) {
    if (f->prompt_send != NULL &&
        event->header.request == f->prompt_send_scope.request &&
        event->header.generation == f->prompt_send_scope.generation)
      cai_cli_prompt_send_cancel(f->prompt_send);
    return CAI_OK;
  }
  if (event->operation == CAI_APP_NETWORK_SEND) {
    cai_app_network_event rejected;
    cai_error local;
    int rc;
    cai_error_init(&local);
    rc = f->prompt_send != NULL || !network_scope(f, &event->header)
             ? cai_cli_error(&local, CAI_ERR_INVALID,
                             "frontend TCP action is busy or authority changed")
             : cai_cli_prompt_send_open(event,
                                        strcmp(f->options->proto, "tcpv6") == 0,
                                        &f->prompt_send, &local);
    if (rc == CAI_OK)
      f->prompt_send_scope = event->header;
    else {
      memset(&rejected, 0, sizeof(rejected));
      rejected.header = event->header;
      rejected.operation = CAI_APP_NETWORK_RECEIPT;
      rejected.code = rc;
      rejected.reason = local.message;
      rc = cai_app_client_network_write(f->client, &rejected, error);
    }
    cai_error_cleanup(&local);
    return rc;
  }
  return cai_cli_error(error, CAI_ERR_PROTOCOL,
                       "unexpected backend network operation");
}
static int network_progress(frontend *f) {
  sl_t *editor = f->group->active != NULL ? f->group->active->ui->sl : NULL;
  const cai_app_network_event *result;
  cai_cli_prompt_scope scope;
  cai_app_network_event capability;
  unsigned int flags;
  if (f->prompt_send != NULL && !network_scope(f, &f->prompt_send_scope))
    cai_cli_prompt_send_cancel(f->prompt_send);
  if (f->prompt_send != NULL &&
      cai_cli_prompt_send_result(f->prompt_send, &result)) {
    if (f->send_editor != NULL) {
      (void)sl_watch_remove(f->send_editor, f->send_watch);
      f->send_editor = NULL;
    }
    if (cai_app_client_network_write(f->client, result, &f->error) != CAI_OK)
      return -1;
    cai_cli_prompt_send_close(f->prompt_send);
    f->prompt_send = NULL;
  }
  if (f->prompt_send != NULL && editor != f->send_editor) {
    if (f->send_editor != NULL)
      (void)sl_watch_remove(f->send_editor, f->send_watch);
    f->send_editor = NULL;
    if (editor != NULL &&
        sl_watch_add(editor, cai_cli_prompt_send_fd(f->prompt_send),
                     SL_WATCH_READ, event_ready, f->group->active,
                     &f->send_watch) != SL_OK)
      return -1;
    f->send_editor = editor;
  }
  memset(&scope, 0, sizeof(scope));
  prompt_scope(f, &scope);
  flags = CAI_APP_NETWORK_CAN_SEND |
          (scope.ready && !cai_cli_prompt_listener_active(f->prompt_listener)
               ? CAI_APP_NETWORK_AVAILABLE
               : 0U);
  if (f->state.bound && f->state.transport.phase == CAI_WORKSPACE_CONNECTED &&
      (f->capability_generation != scope.generation ||
       f->capability_epoch != scope.epoch || f->capability_flags != flags)) {
    memset(&capability, 0, sizeof(capability));
    capability.header.kind = CAI_PROTOCOL_NETWORK;
    capability.header.attachment = scope.attachment;
    capability.header.generation = scope.generation;
    capability.header.conversation = scope.epoch;
    capability.operation = CAI_APP_NETWORK_CAPABILITY;
    capability.flags = flags;
    capability.address = cai_cli_prompt_listener_address(f->prompt_listener);
    capability.advertise = f->options->prompt_advertise;
    if (cai_app_client_network_write(f->client, &capability, &f->error) !=
        CAI_OK)
      return -1;
    f->capability_generation = scope.generation;
    f->capability_epoch = scope.epoch;
    f->capability_flags = flags;
  }
  return 0;
}
static frontend *frontend_open(frontend_group *group,
                               const cai_app_config *application,
                               const cai_cli_options *options,
                               const cai_app_client_config *target,
                               const cai_cli_frontend_boot *boot,
                               cai_error *error) {
  frontend *f = calloc(1U, sizeof(*f));
  cai_sink_callbacks diagnostic;
  if (f == NULL) {
    cai_cli_error(error, CAI_ERR_NOMEM, "allocate terminal frontend");
    return NULL;
  }
  f->group = group;
  f->failed = 1;
  f->next = group->first;
  group->first = f;
  f->workspace = strdup(target->workspace);
  f->runtime_directory = target->runtime_directory != NULL
                             ? strdup(target->runtime_directory)
                             : NULL;
  f->application = application;
  f->options = options;
  f->greeting_resumed = options->resume_id != NULL;
  f->target = *target;
  f->target.workspace = f->workspace;
  f->target.runtime_directory = f->runtime_directory;
  if (target->expected_instance != NULL) {
    memcpy(f->instance, target->expected_instance, sizeof(f->instance));
    f->target.expected_instance = f->instance;
  }
  f->boot = boot;
  /* Select the session before binding/replay. Only subsequent explicit
   * invocation inputs use admissions after that selected replay completes. */
  f->startup.stage = CAI_CLI_STARTUP_MODEL;
  f->startup_select = options->resume_id != NULL || options->new_session;
  f->theme_offsets[0] = options->theme_offset;
  f->theme_offsets[1] = options->subagent_theme_offset;
  f->theme_offsets[2] = options->review_theme_offset;
  f->full_output =
      options->tool_output != NULL && strcmp(options->tool_output, "full") == 0;
  f->wake[0] = f->wake[1] = f->timer = f->socket_fd = f->boot_fd = -1;
  f->parked_fd = -1;
  cai_error_init(&f->error);
  memset(&diagnostic, 0, sizeof(diagnostic));
  diagnostic.context = f;
  diagnostic.write = execution_markdown;
  if (cai_sink_from_callbacks(&diagnostic, &f->execution_sink, error) != CAI_OK)
    return NULL;
  if (f->workspace == NULL ||
      (target->runtime_directory != NULL && f->runtime_directory == NULL)) {
    cai_cli_error(error, CAI_ERR_NOMEM, "copy frontend workspace identity");
    return NULL;
  }
  if (pipe2(f->wake, O_CLOEXEC | O_NONBLOCK) != 0 ||
      (f->timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) <
          0) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "initialize frontend event loop");
    return NULL;
  }
  if (cai_cli_theme_parse(options->theme, &f->theme) != NULL ||
      select_view(f, NULL) != 0) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "create terminal editor/theme");
    return NULL;
  }
  if (group->active == NULL && options->prompt_listener != NULL &&
      options->prompt_listener[0] != '\0') {
    if (cai_cli_prompt_listener_open(
            options->prompt_listener, strcmp(options->proto, "tcpv6") == 0,
            (unsigned long)options->simulate_typing_ms, prompt_scope,
            prompt_notice, prompt_changed, f, &f->prompt_listener,
            error) != CAI_OK)
      return NULL;
  }
  if (group->active != NULL && sl_output_stream_end(f->ui->sl) != SL_OK) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "park new workspace editor");
    return NULL;
  }
  cai_cli_status_init(&f->status, target->workspace, getenv("HOME"));
  (void)cai_cli_status_fields_parse(options->status_fields, &f->status.fields);
  if (group->active == NULL && !options->no_banner) {
    char welcome[192];
    int n = snprintf(welcome, sizeof(welcome), "Welcome to %s.\n\n",
                     options->program_name);
    if (n < 0 || (size_t)n >= sizeof(welcome) ||
        cai_cli_presenter_sink(f->ui, welcome, (size_t)n) != 0) {
      cai_cli_error(error, CAI_ERR_TRANSPORT, "write frontend welcome");
      return NULL;
    }
  }
  if (group->active == NULL && !options->no_banner &&
      application->banner != NULL &&
      cai_cli_presenter_sink(f->ui, application->banner,
                             strlen(application->banner)) != 0) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "write frontend banner");
    return NULL;
  }
  if (group->active == NULL && !options->no_banner &&
      cai_cli_presenter_introduction(f->ui, application->introduction_markdown,
                                     application->introduction_margin_left,
                                     application->introduction_margin_right) !=
          0) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "write frontend introduction");
    return NULL;
  }
  f->failed = boot == NULL && connect_target(f) != CAI_OK;
  return f;
}

static int workspace_begin(frontend *f, const char *line) {
  frontend_group *g = f->group;
  char byte = 'w';
  if (f->client == NULL) {
    (void)sl_set_status_message(
        f->ui->sl, "Workspace is still starting; command draft retained.");
    return 1;
  }
  if (g->discovery != NULL || g->requested || g->candidate != NULL ||
      f->exit_save != 0U) {
    (void)sl_set_status_message(f->ui->sl,
                                "Workspace lookup is already in progress.");
    return 1;
  }
  g->selector = strdup(line);
  if (g->selector == NULL)
    return -1;
  g->requested = 1;
  (void)write(f->wake[1], &byte, 1U);
  return 0;
}

/** Discovery callbacks display one verified record at a time. Neither names
 * nor filesystem bytes are trusted Markdown/terminal control sequences. */
static int workspace_entry(void *context,
                           const cai_workspace_discovery_event *event,
                           cai_error *error) {
  frontend *f = context;
  cai_cli_tool_text text;
  const char *fields[3];
  size_t i;
  int rc;
  if (event->entry == NULL || event->error != NULL)
    return CAI_OK;
  ++f->group->listed;
  fields[0] = event->entry->record.name;
  fields[1] = event->entry->record.workspace;
  fields[2] = event->entry->record.id;
  cai_cli_tool_text_init(&text, 1, 72U);
  rc = markdown(f,
                strcmp(fields[1], f->workspace) == 0
                    ? "**Current workspace**\n\n"
                    : "**Workspace**\n\n",
                strcmp(fields[1], f->workspace) == 0 ? 23U : 15U);
  for (i = 0U; rc == 0 && i < 3U; ++i) {
    if (fields[i] == NULL || fields[i][0] == '\0')
      continue;
    rc = cai_cli_tool_text_write(&text, fields[i], strlen(fields[i]), 0, 1,
                                 tool_markdown, f, error);
    if (rc == CAI_OK)
      rc = cai_cli_tool_text_write(&text, "\n", 1U, 0, 1, tool_markdown, f,
                                   error);
  }
  if (rc == CAI_OK)
    rc = cai_cli_tool_text_write(&text, "", 0U, 1, 1, tool_markdown, f, error);
  return rc;
}

static int workspace_interests(frontend *f, uint64_t *deadline) {
  frontend_group *g = f->group;
  frontend *p;
  struct pollfd descriptor;
  uint64_t due;
  for (p = g->first; p != NULL; p = p->next) {
    if (p == f || p->client == NULL || p->parked_failed)
      continue;
    if (cai_app_client_prepare(p->client, &descriptor, &due, &f->error) !=
            CAI_OK ||
        watch(f, &p->parked_watch, &p->parked_fd, &descriptor) != 0)
      return -1;
    if (due != 0U && (*deadline == 0U || due < *deadline))
      *deadline = due;
  }
  if (g->discovery != NULL) {
    if (cai_workspace_discovery_prepare(g->discovery, &descriptor, &due,
                                        &f->error) != CAI_OK ||
        watch(f, &g->discovery_watch, &g->discovery_fd, &descriptor) != 0)
      return -1;
    if (due != 0U && (*deadline == 0U || due < *deadline))
      *deadline = due;
  }
  return 0;
}

static void workspace_query_close(frontend *f) {
  frontend_group *g = f->group;
  if (g->discovery_fd >= 0) {
    (void)sl_watch_remove(f->ui->sl, g->discovery_watch);
    g->discovery_fd = -1;
  }
  cai_workspace_discovery_close(g->discovery);
  g->discovery = NULL;
  free(g->selector);
  g->selector = NULL;
}

static int workspace_progress(frontend *f) {
  frontend_group *g = f->group;
  frontend *p;
  cai_error local;
  int rc = CAI_OK;
  cai_error_init(&local);
  if (g->requested) {
    cai_workspace_discovery_config config;
    char *argument = g->selector + 10U;
    size_t length;
    g->requested = 0;
    if (cai_cli_presenter_input(f->ui, g->selector, 1) != 0)
      return -1;
    argument += strspn(argument, " \t");
    length = strlen(argument);
    while (length != 0U && strchr(" \t", argument[length - 1U]) != NULL)
      argument[--length] = '\0';
    g->listing = length == 0U;
    g->listed = 0;
    cai_workspace_discovery_config_init(&config);
    config.runtime_directory = f->runtime_directory;
    config.directory = f->workspace;
    config.selector = g->listing ? NULL : argument;
    config.event = g->listing ? workspace_entry : NULL;
    config.context = f;
    rc = cai_workspace_discovery_open(&config, clock_ms(), &g->discovery,
                                      &local);
  }
  if (rc == CAI_OK && g->discovery != NULL) {
    cai_workspace_discovery_state state;
    rc = cai_workspace_discovery_pump(g->discovery, clock_ms(), &local);
    if (rc == CAI_OK)
      rc = cai_workspace_discovery_get_state(g->discovery, &state, &local);
    if (rc == CAI_OK && state.phase != CAI_WORKSPACE_DISCOVERING) {
      if (state.phase == CAI_WORKSPACE_FOUND) {
        const cai_workspace_discovery_result *found =
            cai_workspace_discovery_get_result(g->discovery);
        for (p = g->first; p != NULL; p = p->next)
          if (!p->parked_failed && !p->failed &&
              strcmp(p->workspace, found->record.workspace) == 0 &&
              memcmp(p->state.transport.instance, found->record.instance,
                     16U) == 0)
            break;
        if (p == f)
          (void)notice(f, "Already viewing this workspace.");
        else {
          if (p == NULL) {
            cai_app_client_config target = f->target;
            target.workspace = found->record.workspace;
            target.expected_instance = found->record.instance;
            /* Invocation requirements identify only the initial owner. An
             * explicit /workspace selects another already configured owner;
             * never impose the first workspace's store/tool identity on it. */
            target.requirements = NULL;
            target.requirement_count = 0U;
            if (target.handshake_timeout_ms == 0U)
              target.handshake_timeout_ms = 10000U;
            p = frontend_open(g, f->application, f->options, &target, NULL,
                              &local);
            if (p != NULL) {
              p->startup.stage = CAI_CLI_STARTUP_DONE;
              p->startup_select = 0;
              p->greeting_resumed = 0;
            } else
              rc = local.code != CAI_OK ? local.code : CAI_ERR_TRANSPORT;
          }
          g->candidate = p;
        }
      } else if (!g->listing)
        (void)notice(f, state.phase == CAI_WORKSPACE_AMBIGUOUS
                            ? "Workspace selector is ambiguous."
                            : "No running workspace matches that selector.");
      else if (g->listed == 0)
        (void)notice(f, "No running workspaces were found.");
      if (cai_cli_presenter_finish_response(f->ui) != 0)
        rc = CAI_ERR_TRANSPORT;
      workspace_query_close(f);
    }
  }
  for (p = g->first; p != NULL; p = p->next) {
    if (p == f || p->client == NULL || p->parked_failed)
      continue;
    if (cai_app_client_pump(p->client, clock_ms(), &p->error) != CAI_OK ||
        cai_app_client_get_state(p->client, &p->state, NULL) != CAI_OK ||
        p->state.transport.phase == CAI_WORKSPACE_DISCONNECTED ||
        p->state.transport.phase == CAI_WORKSPACE_FAILED) {
      p->parked_failed = 1;
      if (p->parked_fd >= 0) {
        (void)sl_watch_remove(f->ui->sl, p->parked_watch);
        p->parked_fd = -1;
      }
    }
  }
  if (g->candidate != NULL) {
    if (g->candidate->parked_failed || g->candidate->failed) {
      (void)notice(
          f, "Could not attach that workspace; current workspace retained.");
      g->candidate = NULL;
    } else if (g->candidate->state.transport.phase == CAI_WORKSPACE_CONNECTED)
      g->switching = 1;
  }
  if (rc != CAI_OK) {
    (void)notice(f, local.message != NULL ? local.message
                                          : "Workspace lookup failed.");
    workspace_query_close(f);
  }
  cai_error_cleanup(&local);
  return 0;
}

static int workspace_activate(frontend **current) {
  frontend *f = *current, *p;
  frontend_group *g = f->group;
  frontend *next = g->candidate;
  const cai_app_view *view = NULL;
  g->candidate = NULL;
  g->switching = 0;
  if (next == NULL)
    return 0;
  /* Stop only presentation. Accepted input and model work stay with their
   * owner. Capture the authenticated cursor before discarding any late spans;
   * activation requests precisely this missed suffix, not a full replay. */
  if (cai_app_client_get_state(f->client, &f->state, NULL) != CAI_OK)
    return -1;
  if (f->state.transcript.collection[0] != '\0') {
    if (!f->presentation_parked && park_presentation(f, NULL) != CAI_OK)
      return -1;
    f->resume_actions = 1;
    if (cai_app_client_unsubscribe(f->client, &f->error) != CAI_OK)
      return -1;
  }
  for (p = g->first; p != NULL; p = p->next)
    if (p->parked_fd >= 0) {
      (void)sl_watch_remove(f->ui->sl, p->parked_watch);
      p->parked_fd = -1;
    }
  if (cai_cli_presenter_finish_response(f->ui) != 0 ||
      cai_cli_presenter_finish_reasoning(f->ui) != 0 ||
      sl_output_stream_end(f->ui->sl) != SL_OK)
    return -1;
  cai_cli_transcript_presenter_reset(f->transcript);
  f->presentation_parked = f->transcript_entry = 0;
  f->command_length = f->action_length = 0U;
  f->action_field = 0;
  /* Keep bounded call metadata: a yielded command can finish while parked,
   * and its later write_stdin output still needs the original command label. */
  park_view(f);
  g->active = next;
  *current = next;
  if (sl_output_stream_begin(next->ui->sl) != SL_OK)
    return -1;
  (void)cai_app_client_get_view(next->client, &view, NULL);
  if (next->pending_context && view != NULL && view->available) {
    if (select_view(next, view->session_id) != 0)
      return -1;
  } else {
    cai_cli_transcript_presenter_reset(next->transcript);
    next->subscribed = next->welcomed = 0;
  }
  return 0;
}

/** Maintain native clients while Softline owns its terminal relay. Queue
 * routing is paused, not copied: the original producer is synchronized after
 * return. Catalog metadata is bounded; transcript bodies are unsubscribed.
 * This callback must never enter an editor or presenter. */
static int terminal_service(void *context, int epoll_fd, int *timeout,
                            cai_error *error) {
  frontend *active = context, *p;
  uint64_t now = clock_ms();
  for (p = active->group->first; p != NULL; p = p->next) {
    struct pollfd fd;
    struct epoll_event event;
    uint64_t deadline = 0U;
    unsigned int flags;
    if (p->client == NULL || p->handoff_failed)
      continue;
    if (p->boot != NULL) {
      cai_app_client_config target = p->target;
      struct pollfd boot_fd;
      uint64_t boot_deadline = 0U;
      int ready = 0;
      if (p->boot->pump(p->boot->context, now, &target, &ready, &p->error) !=
              CAI_OK ||
          p->boot->prepare(p->boot->context, &boot_fd, &boot_deadline,
                           &p->error) != CAI_OK) {
        active->handoff_failed = 1;
        break;
      }
      if (ready == CAI_CLI_FRONTEND_DETACH ||
          ready == CAI_CLI_FRONTEND_STOPPED) {
        active->expected_close = active->quit = 1;
        return cai_cli_error(error, CAI_ERR_CANCELLED,
                             "terminal attachment stopped during handoff");
      }
      /* After attachment these are retained lifetime interests, not new
       * discovery clients. The relay owns this transient epoll registration. */
      if (boot_fd.fd >= 0) {
        memset(&event, 0, sizeof(event));
        event.events = EPOLLET | (boot_fd.events & POLLIN ? EPOLLIN : 0U) |
                       (boot_fd.events & POLLOUT ? EPOLLOUT : 0U);
        event.data.fd = boot_fd.fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, boot_fd.fd, &event) != 0 &&
            (errno != EEXIST ||
             epoll_ctl(epoll_fd, EPOLL_CTL_MOD, boot_fd.fd, &event) != 0))
          return cai_cli_error(error, CAI_ERR_TRANSPORT,
                               "watch frontend lifetime during handoff");
      }
      if (boot_deadline != 0U) {
        uint64_t remaining = boot_deadline > now ? boot_deadline - now : 0U;
        int wait = remaining > INT_MAX ? INT_MAX : (int)remaining;
        if (*timeout < 0 || wait < *timeout)
          *timeout = wait;
      }
    }
    if (cai_app_client_pump(p->client, now, &p->error) != CAI_OK ||
        cai_app_client_get_state(p->client, &p->state, &p->error) != CAI_OK ||
        p->state.transport.phase == CAI_WORKSPACE_DISCONNECTED ||
        p->state.transport.phase == CAI_WORKSPACE_FAILED ||
        cai_app_client_prepare(p->client, &fd, &deadline, &p->error) !=
            CAI_OK) {
      p->handoff_failed = 1;
      if (p->handoff_fd >= 0)
        (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, p->handoff_fd, NULL);
      p->handoff_fd = -1;
      continue;
    }
    flags = EPOLLET | (fd.events & POLLIN ? EPOLLIN : 0U) |
            (fd.events & POLLOUT ? EPOLLOUT : 0U);
    if (p->handoff_fd >= 0 && p->handoff_fd != fd.fd) {
      (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, p->handoff_fd, NULL);
      p->handoff_fd = -1;
    }
    if (fd.fd >= 0 && (p->handoff_fd != fd.fd || p->handoff_events != flags)) {
      memset(&event, 0, sizeof(event));
      event.events = flags;
      event.data.fd = fd.fd;
      if (epoll_ctl(epoll_fd, p->handoff_fd < 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD,
                    fd.fd, &event) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "watch owner during terminal handoff");
      p->handoff_fd = fd.fd;
      p->handoff_events = flags;
    }
    if (deadline != 0U) {
      uint64_t remaining = deadline > now ? deadline - now : 0U;
      int wait = remaining > INT_MAX ? INT_MAX : (int)remaining;
      if (*timeout < 0 || wait < *timeout)
        *timeout = wait;
    }
  }
  if (active->capture_id != 0U &&
      (active->state.conversation != active->capture_scope.conversation ||
       active->state.transport.generation != active->capture_scope.generation ||
       active->state.transport.controller !=
           active->state.transport.attachment))
    return cai_cli_error(error, CAI_ERR_CANCELLED,
                         "shared shell recording lost its original authority");
  if (active->capture_waiting) {
    int remaining;
    now = clock_ms();
    if (now >= active->capture_deadline)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "shared shell capture acknowledgement timed out");
    remaining = (int)(active->capture_deadline - now);
    if (*timeout < 0 || remaining < *timeout)
      *timeout = remaining;
  }
  return active->handoff_failed
             ? cai_cli_error(error, CAI_ERR_CANCELLED,
                             "workspace owner stopped during terminal handoff")
             : CAI_OK;
}
/** At most one bounded recording operation is outstanding. Its original
 * generation/sequence is never upgraded or retried after an uncertain result.
 */
static int capture_send(frontend *f, cai_app_shell_operation operation,
                        const void *data, size_t length, unsigned int code,
                        uint64_t duration, cai_error *error) {
  cai_app_shell_request request;
  int rc;
  if (f->control_request == UINT64_MAX || f->capture_waiting ||
      (operation != CAI_APP_SHELL_BEGIN && f->capture_sequence == UINT64_MAX))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "shared shell request is pending or exhausted");
  memset(&request, 0, sizeof(request));
  request.operation = operation;
  request.recording = operation == CAI_APP_SHELL_BEGIN ? 0U : f->capture_id;
  request.sequence =
      operation == CAI_APP_SHELL_BEGIN ? 0U : f->capture_sequence + 1U;
  request.data = data;
  request.length = length;
  request.exit_code = code;
  request.duration_ms = duration;
  f->capture_request = ++f->control_request;
  rc = cai_app_client_shell(f->client, f->capture_request, &f->capture_scope,
                            &request, error);
  if (rc == CAI_OK) {
    f->capture_waiting = 1;
    f->capture_code = CAI_OK;
    f->capture_reason[0] = '\0';
    f->capture_deadline = clock_ms() + 10000U;
  }
  return rc;
}
static int capture_result(frontend *f, cai_error *error) {
  return f->capture_code == CAI_OK
             ? CAI_OK
             : cai_cli_error(error, f->capture_code,
                             f->capture_reason[0] != '\0'
                                 ? f->capture_reason
                                 : "shared shell capture failed");
}
static int capture_output(void *context, const void *bytes, size_t length,
                          int *accepted, cai_error *error) {
  frontend *f = context;
  int rc;
  *accepted = 0;
  if (!f->capture_pending) {
    rc = capture_send(f, CAI_APP_SHELL_WRITE, bytes, length, 0U, 0U, error);
    if (rc == CAI_OK)
      f->capture_pending = 1;
    return rc;
  }
  if (f->capture_waiting)
    return CAI_OK;
  rc = capture_result(f, error);
  if (rc == CAI_OK) {
    ++f->capture_sequence;
    f->capture_pending = 0;
    *accepted = 1;
  }
  return rc;
}
/** Finite begin/finish handshake while the editor is suspended. All visited
 * owners remain serviced; no Softline or renderer calls occur in this wait.
 * Chunk acknowledgement uses the relay's descriptor loop instead. */
static int capture_wait(frontend *f, cai_error *error) {
  struct epoll_event events[32];
  frontend *p;
  int descriptor = epoll_create1(EPOLL_CLOEXEC), rc = CAI_OK;
  if (descriptor < 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "watch shared shell recording");
  /* terminal_service retains registrations between calls on one epoll only. */
  for (p = f->group->first; p != NULL; p = p->next)
    p->handoff_fd = -1;
  while (f->capture_waiting) {
    int timeout = -1;
    rc = terminal_service(f, descriptor, &timeout, error);
    if (rc != CAI_OK || !f->capture_waiting)
      break;
    if (epoll_wait(descriptor, events, 32, timeout) < 0 && errno != EINTR) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for shell recording");
      break;
    }
  }
  close(descriptor);
  for (p = f->group->first; p != NULL; p = p->next)
    p->handoff_fd = -1;
  if (rc == CAI_OK)
    rc = capture_result(f, error);
  return rc;
}
/** Resolve a fixed local executable using the frontend's captured PATH and
 * selected workspace, including relative/empty PATH components. No shell. */
static char *pager_path(frontend *f) {
  const char *path = cai_environment_get(f->options->environment, "PATH");
  char *result = malloc(PATH_MAX);
  if (result == NULL)
    return NULL;
  if (path == NULL)
    path = "/usr/bin:/bin";
  do {
    struct stat st;
    size_t length = strcspn(path, ":");
    int n = -1;
    if (length < PATH_MAX) {
      if (path[0] == '/')
        n = snprintf(result, PATH_MAX, "%.*s/less", (int)length, path);
      else
        n = snprintf(result, PATH_MAX, "%s/%.*s%sless", f->workspace,
                     (int)length, path, length == 0U ? "" : "/");
    }
    if (n >= 0 && n < PATH_MAX && access(result, X_OK) == 0 &&
        stat(result, &st) == 0 && S_ISREG(st.st_mode))
      return result;
    path += length;
    if (*path == '\0')
      break;
    ++path;
  } while (1);
  free(result);
  return NULL;
}
/** The fallback reads on demand through libmdf's bounded pager cache. Its hook
 * services owner connections without invoking Softline or another renderer. */
typedef struct pager_service {
  frontend *frontend;
  int epoll;
  uint64_t next;
} pager_service;
static int pager_cancel(void *context) {
  pager_service *p = context;
  uint64_t now = clock_ms();
  int timeout = -1;
  if (now < p->next)
    return 0;
  p->next = now + 50U;
  return terminal_service(p->frontend, p->epoll, &timeout,
                          &p->frontend->error) != CAI_OK ||
         p->frontend->handoff_failed || p->frontend->quit;
}
/** Help is generated from static paragraphs and a bounded invocation example.
 * Reposition that producer directly instead of buffering a forward-only source.
 */
static int help_pager_seek(void *context, const mdf_source_offset *offset) {
  size_t position;
  if (offset == NULL || offset->high != 0UL)
    return -1;
  position = (size_t)offset->low;
  if ((unsigned long)position != offset->low)
    return -1;
  return cai_cli_help_reader_seek(context, position);
}
static int native_pager(frontend *f, const char *log, int help) {
  pager_service service;
  mdf_pager_control control;
  mdf_options options;
  mdf_status status;
  cai_cli_help_reader *reader = NULL;
  mdf_seekable_source source;
  int rc = CAI_OK;
  memset(&service, 0, sizeof(service));
  service.frontend = f;
  service.epoll = epoll_create1(EPOLL_CLOEXEC);
  if (service.epoll < 0)
    return cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                         "watch owner during log pager");
  if (help) {
    rc = cai_cli_help_reader_open(f->options->invocation_name, 1, &reader,
                                  &f->error);
    if (rc != CAI_OK) {
      close(service.epoll);
      return rc;
    }
  }
  if (sl_output_stream_end(f->ui->sl) != SL_OK) {
    cai_cli_help_reader_close(reader);
    close(service.epoll);
    return cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                         "release editor for log pager");
  }
  memset(&control, 0, sizeof(control));
  control.userdata = &service;
  control.should_cancel = pager_cancel;
  mdf_options_init(&options);
  cai_cli_theme_markdown(&f->ui->render_theme, &options);
  if (cai_environment_get(f->options->environment, "NO_COLOR") != NULL)
    options.ansi_mode = MDF_ANSI_OFF;
  source.userdata = reader;
  source.read = cai_cli_help_reader_read;
  source.seek = help_pager_seek;
  status =
      help
          ? mdf_pager_seekable_source("Help.md", &source, &options,
                                      MDF_PAGER_FORMAT_MARKDOWN, &control, NULL)
          : mdf_pager_file(log, &options, MDF_PAGER_FORMAT_TEXT, &control);
  cai_cli_help_reader_close(reader);
  close(service.epoll);
  if (sl_output_stream_begin(f->ui->sl) != SL_OK)
    rc = cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                       "restore editor after log pager");
  else if (status != MDF_OK && status != MDF_CANCELLED)
    rc = cai_cli_error(&f->error, CAI_ERR_TRANSPORT, mdf_status_string(status));
  else if (f->handoff_failed)
    rc = cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                       "owner disconnected during log pager");
  return rc;
}
static int terminal_action(frontend *f, int kind) {
  frontend *p;
  const cai_app_view *view = NULL;
  const char *arguments[4], *shell;
  char *pager = NULL, *log = NULL, message[128];
  uint64_t capture_started = 0U;
  int raw_status = 127 << 8, escaped = 0, rc, result = 0;
  if (f->group->discovery != NULL || f->group->candidate != NULL ||
      f->exit_save != 0U || !f->ready || f->handoff_notice[0] != '\0') {
    result =
        notice(f, "Finish the current workspace transition before handoff.");
    goto done;
  }
  if (kind == 2) {
    (void)cai_app_client_get_view(f->client, &view, NULL);
    if (view == NULL || view->log_path == NULL) {
      result = notice(f, "No log file is configured for this session.");
      goto done;
    }
    if (!f->options->prefer_native_pager)
      pager = pager_path(f);
    log = strdup(view->log_path);
    if (log == NULL) {
      free(pager);
      return cai_cli_error(&f->error, CAI_ERR_NOMEM, "copy log pager path");
    }
    arguments[0] = pager;
    arguments[1] = "--";
    arguments[2] = log;
    arguments[3] = NULL;
  } else if (kind == 1 || kind == 4) {
    shell = cai_environment_get(f->options->environment, "SHELL");
    arguments[0] = shell != NULL && shell[0] != '\0' ? shell : "/bin/sh";
    arguments[1] = "-i";
    arguments[2] = NULL;
  }
  if (cai_cli_presenter_finish_response(f->ui) != 0 ||
      cai_cli_presenter_finish_reasoning(f->ui) != 0 ||
      cai_app_client_get_state(f->client, &f->state, &f->error) != CAI_OK) {
    result = -1;
    goto done;
  }
  if (f->state.transcript.collection[0] != '\0') {
    if (!f->presentation_parked && park_presentation(f, NULL) != CAI_OK) {
      result = -1;
      goto done;
    }
    f->resume_actions = 1;
    snprintf(f->handoff_call, sizeof(f->handoff_call), "%s", f->action_call);
    if (cai_app_client_unsubscribe(f->client, &f->error) != CAI_OK) {
      result = -1;
      goto done;
    }
  }
  for (p = f->group->first; p != NULL; p = p->next) {
    p->handoff_fd = -1;
    p->handoff_events = 0U;
    p->handoff_failed = p->parked_failed || p->failed;
    if (p->queue != NULL && !p->handoff_failed && p->state.bound &&
        cai_cli_input_socket_pause(p->queue, &p->error) != CAI_OK) {
      result = -1;
      goto done;
    }
  }
  cai_cli_transcript_presenter_reset(f->transcript);
  f->subscribed = 0;
  f->group->terminal_active = 1;
  f->capture_first = f->capture_last = 0U;
  rc = CAI_OK;
  if (kind == 4) {
    f->capture_scope.conversation = f->state.conversation;
    f->capture_scope.generation = f->state.transport.generation;
    f->capture_sequence = 0U;
    f->capture_pending = 0;
    capture_started = clock_ms();
    rc = capture_send(f, CAI_APP_SHELL_BEGIN, arguments[0],
                      strlen(arguments[0]), 0U, 0U, &f->error);
    if (rc == CAI_OK) {
      f->capture_id = f->capture_request;
      rc = capture_wait(f, &f->error);
      if (rc != CAI_OK && !f->capture_waiting)
        f->capture_id = 0U; /* Rejected BEGIN opened no recording. */
    }
  }
  if (rc == CAI_OK) {
    if (kind == 3 || (kind == 2 && pager == NULL))
      rc = native_pager(f, log, kind == 3);
    else if (sl_output_stream_end(f->ui->sl) != SL_OK)
      rc = cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                         "release output before terminal relay");
    else {
      rc = cai_cli_terminal_program(f->ui->sl, arguments, f->workspace,
                                    f->options->environment, terminal_service,
                                    kind == 4 ? capture_output : NULL, f,
                                    &raw_status, &escaped, &f->error);
      /* Reopen only after the relay has restored descriptor flags. A large
       * PTY burst can leave stdout backpressured; reopening the persistent
       * stream while it is still nonblocking can fail its setup writes. */
      if (sl_output_stream_begin(f->ui->sl) != SL_OK)
        rc = cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                           "restore output after terminal relay");
    }
  }
  if (f->quit && (rc == CAI_OK || rc == CAI_ERR_CANCELLED)) {
    /* Terminal ownership has been restored. Connection detach will settle an
     * interrupted recording; do not resynchronize an editor that is exiting. */
    f->group->terminal_active = 0;
    f->presentation_parked = f->transcript_entry = 0;
    cai_error_cleanup(&f->error);
    cai_error_init(&f->error);
    goto done;
  }
  if (kind == 4 && f->capture_id != 0U) {
    if (rc == CAI_OK && f->capture_pending) {
      rc = capture_wait(f, &f->error);
      if (rc == CAI_OK) {
        ++f->capture_sequence;
        f->capture_pending = 0;
      }
    }
    if (rc == CAI_OK) {
      unsigned int code = WIFEXITED(raw_status)
                              ? (unsigned int)WEXITSTATUS(raw_status)
                              : (unsigned int)(128 + WTERMSIG(raw_status));
      rc = capture_send(f, CAI_APP_SHELL_FINISH, NULL, 0U, code,
                        clock_ms() - capture_started, &f->error);
      if (rc == CAI_OK)
        rc = capture_wait(f, &f->error);
    }
    if (rc != CAI_OK &&
        f->state.conversation == f->capture_scope.conversation &&
        f->state.transport.generation == f->capture_scope.generation) {
      /* An unknown operation cannot be retried. Detach finalizes the owner
       * recording rather than leaving new model turns held indefinitely. */
      (void)cai_app_client_detach(f->client, NULL);
      f->handoff_failed = 1;
    }
    f->capture_id = 0U;
    f->capture_waiting = f->capture_pending = 0;
  }
  f->group->terminal_active = 0;
  f->presentation_parked = f->transcript_entry = 0;
  f->ui->feed_breaks = f->ui->feed_ansi = 0;
  if (cai_cli_presenter_geometry(f->ui) != 0) {
    cai_cli_error(&f->error, CAI_ERR_TRANSPORT,
                  sl_last_error(f->ui->sl) != NULL
                      ? sl_last_error(f->ui->sl)
                      : "restore terminal handoff geometry");
    result = -1;
    goto done;
  }
  for (p = f->group->first; p != NULL; p = p->next) {
    if (p->queue != NULL && !p->handoff_failed && p->state.bound &&
        strcmp(p->state.session, p->current->session) == 0) {
      sl_queue_request_t request;
      sl_queue_request_id_t id;
      if (cai_cli_input_socket_resume(p->queue, p->client, &p->error) !=
          CAI_OK) {
        result = -1;
        goto done;
      }
      memset(&request, 0, sizeof(request));
      request.operation = SL_QUEUE_SYNCHRONIZE;
      if (sl_queue_backend_request(p->ui->sl, &request, &id) != SL_OK) {
        result = -1;
        goto done;
      }
    } else if (p->queue != NULL) {
      (void)cai_cli_input_socket_disconnect(p->queue, NULL);
      /* The parked view retains its editor/queue. A session reset while
       * the terminal was lent out must not route new events to that queue. */
      p->queue = NULL;
    }
    if (p->handoff_failed)
      p->parked_failed = 1;
  }
  if (f->switching) {
    if (select_view(f, f->next_session) != 0) {
      result = -1;
      goto done;
    }
  } else if (cai_cli_completions_open(&f->current->completions, f->ui->sl,
                                      f->workspace, completion_catalog,
                                      f) != SL_OK) {
    result = -1;
    goto done;
  }
  cai_cli_status_maybe_refresh_branch(&f->status, f->workspace);
  if (f->capture_first != 0U &&
      f->capture_scope.conversation == f->current->epoch)
    cai_cli_transcript_presenter_skip_shell(f->transcript, f->capture_first,
                                            f->capture_last);
  if (rc != CAI_OK) {
    snprintf(f->handoff_notice, sizeof(f->handoff_notice), "%s",
             f->error.message != NULL ? f->error.message
                                      : "Local terminal handoff failed.");
    f->handoff_notice_error = 1;
    cai_error_cleanup(&f->error);
  } else {
    const char *name = kind == 1 || kind == 4 ? "Shell" : "Pager";
    if (kind == 3 || (kind == 2 && pager == NULL))
      snprintf(message, sizeof(message), "**Pager closed.**\n");
    else if (escaped)
      snprintf(message, sizeof(message), "**%s escaped.**\n", name);
    else if (WIFSIGNALED(raw_status))
      snprintf(message, sizeof(message), "**%s terminated by signal %d.**\n",
               name, WTERMSIG(raw_status));
    else
      snprintf(message, sizeof(message), "**%s exited %d.**\n", name,
               WEXITSTATUS(raw_status));
    snprintf(f->handoff_notice, sizeof(f->handoff_notice), "%s", message);
    f->handoff_notice_error = 0;
  }
done:
  if (result == 0 && f->presentation_parked) {
    /* A rejected local action must also resume any spans skipped after its
     * entry seam. Do not replay the action itself on that suffix. */
    f->resume_actions = 1;
    snprintf(f->handoff_call, sizeof(f->handoff_call), "%s", f->action_call);
    if (cai_app_client_unsubscribe(f->client, &f->error) != CAI_OK)
      result = -1;
    cai_cli_transcript_presenter_reset(f->transcript);
    f->subscribed = f->presentation_parked = f->transcript_entry = 0;
  }
  free(pager);
  free(log);
  return result;
}
int cai_cli_frontend_run(const cai_app_config *application,
                         const cai_cli_options *options,
                         const cai_app_client_config *target,
                         const cai_cli_frontend_boot *boot, int *exit_code,
                         cai_error *error) {
  frontend_group group;
  frontend *f;
  uint64_t close_deadline;
  int rc = CAI_OK;
  memset(&group, 0, sizeof(group));
  group.discovery_fd = -1;
  f = frontend_open(&group, application, options, target, boot, error);
  if (f == NULL) {
    rc = error != NULL && error->code != CAI_OK ? error->code : CAI_ERR_NOMEM;
    goto done;
  }
  group.active = f;
  if (!f->failed && service(f) != 0)
    f->failed = 1;
  while (!f->quit && !f->failed) {
    sl_prompt_source_t source;
    char *line = NULL;
    /* A local terminal handoff can finish with a pending context change.
     * Apply it at this seam before entering an editor whose queue is parked;
     * there need not be another socket event to wake that editor. */
    if (!group.switching && !f->switching && !f->control && !f->shell)
      line = sl_next_prompt(f->ui->sl, "> ", &source);
    if (line != NULL) {
      /* The remote backend accepts every submission; a returned unmanaged
       * line would otherwise bypass receipt/queue ownership. */
      sl_free_string(f->ui->sl, line);
      f->failed = 1;
      cai_cli_error(&f->error, CAI_ERR_PROTOCOL, "unmanaged frontend input");
    } else if (!f->quit && group.switching) {
      if (workspace_activate(&f) != 0)
        f->failed = 1;
    } else if (!f->quit && f->switching) {
      if (select_view(f, f->next_session) != 0)
        f->failed = 1;
    } else if (!f->quit && f->control) {
      int control = f->control;
      const char *command = control == 1 ? "/takeover" : "/shutdown";
      f->control = 0;
      if (cai_cli_presenter_input(f->ui, command, 1) != 0)
        f->failed = 1;
      else {
        int submitted =
            control == 1
                ? cai_app_client_takeover(f->client, ++f->control_request,
                                          f->state.transport.generation,
                                          &f->error)
                : cai_app_client_shutdown(f->client, ++f->control_request,
                                          f->state.transport.generation,
                                          &f->error);
        if (submitted == CAI_OK && control == 2)
          f->expected_close = 1;
        if (submitted != CAI_OK) {
          (void)notice(f, f->error.message != NULL ? f->error.message
                                                   : "Control rejected");
          cai_error_cleanup(&f->error);
        }
      }
    } else if (!f->quit && f->shell) {
      int kind = f->shell;
      f->shell = 0;
      if (terminal_action(f, kind) != 0)
        f->failed = 1;
    } else if (!f->quit) {
      int input_status = sl_last_readline_status(f->ui->sl);
      if (input_status == SL_READLINE_CANCELLED ||
          input_status == SL_READLINE_INTERRUPTED) {
        if (f->client != NULL &&
            cai_app_client_cancel(f->client, ++f->control_request,
                                  f->state.transport.generation,
                                  &f->error) != CAI_OK) {
          (void)notice(f, f->error.message != NULL
                              ? f->error.message
                              : "Cancellation unavailable");
          cai_error_cleanup(&f->error);
        }
      } else if (input_status == SL_READLINE_EOF) {
        if (goodbye(f) != 0)
          f->failed = 1;
        f->quit = 1;
      } else
        f->quit = 1;
    }
    if (!f->quit && service(f) != 0)
      f->failed = 1;
  }
  if (f->failed) {
    rc = cai_cli_error(
        error, f->error.code != CAI_OK ? f->error.code : CAI_ERR_TRANSPORT,
        f->error.message != NULL           ? f->error.message
        : sl_last_error(f->ui->sl) != NULL ? sl_last_error(f->ui->sl)
                                           : "terminal attachment failed");
    /* Keep unexpected transport failures visible in the rendered feed before
     * releasing the editor. Return the original error while acknowledging a
     * successful presentation so the executable does not print it again.
     * An unusable output sink cannot be repaired by another diagnostic write.
     */
    if (!f->ui->sink_failed && group.active == f && !group.terminal_active &&
        notice(f, f->error.message != NULL
                      ? f->error.message
                      : "terminal attachment failed") == 0 &&
        options->diagnostic_presented != NULL)
      *options->diagnostic_presented = 1;
  }
done:
  *exit_code = rc == CAI_OK ? 0 : 1;
  cai_workspace_discovery_close(group.discovery);
  free(group.selector);
  close_deadline = clock_ms() + 2000U;
  for (f = group.first; f != NULL; f = f->next) {
    if (f->send_editor != NULL)
      (void)sl_watch_remove(f->send_editor, f->send_watch);
    cai_cli_prompt_send_close(f->prompt_send);
    f->prompt_send = NULL;
    cai_cli_prompt_listener_close(f->prompt_listener);
    f->prompt_listener = NULL;
  }
  for (f = group.first; f != NULL;) {
    frontend *next = f->next;
    if (f->client != NULL) {
      uint64_t until = close_deadline;
      (void)cai_app_client_detach(f->client, NULL);
      while (clock_ms() < until) {
        struct pollfd descriptor;
        uint64_t deadline, now;
        if (cai_app_client_get_state(f->client, &f->state, NULL) != CAI_OK ||
            f->state.transport.phase == CAI_WORKSPACE_DISCONNECTED ||
            cai_app_client_prepare(f->client, &descriptor, &deadline, NULL) !=
                CAI_OK ||
            descriptor.fd < 0)
          break;
        now = clock_ms();
        if (now >= until)
          break;
        if (poll(&descriptor, 1U, (int)(until - now)) < 0 && errno != EINTR)
          break;
        if (cai_app_client_pump(f->client, clock_ms(), NULL) != CAI_OK)
          break;
      }
    }
    cai_app_client_close(f->client);
    park_view(f);
    while (f->views != NULL) {
      frontend_view *v = f->views;
      sl_t *editor = v->ui.sl;
      f->views = v->next;
      cai_cli_replay_tools_close(v->tools);
      cai_cli_transcript_presenter_close(v->transcript);
      cai_cli_presenter_close(&v->ui);
      if (editor != NULL) {
        if (v == f->current)
          (void)sl_output_stream_end(editor);
        sl_destroy(editor);
      }
      cai_cli_input_socket_close(v->queue);
      while (v->recall_first != NULL) {
        recall_pending *p = v->recall_first;
        v->recall_first = p->next;
        free(p->text);
        free(p);
      }
      free(v);
    }
    if (f->wake[0] >= 0)
      close(f->wake[0]);
    if (f->wake[1] >= 0)
      close(f->wake[1]);
    if (f->timer >= 0)
      close(f->timer);
    cai_error_cleanup(&f->error);
    free(f->action);
    free(f->command);
    free(f->history_entry);
    free(f->exit_draft);
    free(f->workspace);
    free(f->runtime_directory);
    clear_skills(f);
    cai_sink_close(f->execution_sink);
    free(f);
    f = next;
  }
  return rc;
}

int cai_app_terminal_run(const cai_app_config *application,
                         const cai_app_client_config *target, int argc,
                         const char *const *argv, int *exit_code,
                         cai_error *error) {
  cai_app_config defaults;
  cai_cli_options options;
  cai_cli_workspace_claims requirements = {0};
  cai_config_context context;
  cai_config *configuration = NULL;
  cai_environment *environment = NULL;
  char **arguments;
  int rc = CAI_OK, i;
  if (exit_code != NULL)
    *exit_code = 1;
  if (target == NULL || target->workspace == NULL || exit_code == NULL ||
      argc < 0 || argc == INT_MAX || (argc > 0 && argv == NULL) ||
      !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "terminal attachment requires a target and terminal input/output");
  if (application == NULL) {
    cai_app_config_init(&defaults);
    application = &defaults;
  }
  arguments = calloc((size_t)argc + 2U, sizeof(*arguments));
  if (arguments == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate terminal arguments");
  for (i = 0; i <= argc; ++i) {
    const char *value = i == 0 ? application->invocation_name != NULL
                                     ? application->invocation_name
                                     : application->program_name
                               : argv[i - 1];
    if (value == NULL || (arguments[i] = strdup(value)) == NULL) {
      rc = cai_cli_error(error, value == NULL ? CAI_ERR_INVALID : CAI_ERR_NOMEM,
                         "copy terminal arguments");
      break;
    }
  }
  memset(&context, 0, sizeof(context));
  context.directory = target->workspace;
  if (rc == CAI_OK) {
    int configured =
        cai_cli_configure_at(argc + 1, arguments, &options, &configuration,
                             application, &context, error);
    if (configured == 1 && cai_cli_theme_validate(&options, error) != CAI_OK)
      configured = -1;
    if (configured != 1)
      rc = configured == 0 ? CAI_OK : CAI_ERR_INVALID;
    else if (strcmp(options.directory, target->workspace) != 0 ||
             options.server != NULL || options.headless || options.temporary ||
             cai_cli_options_utility(&options) || options.non_interactive ||
             options.review)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "terminal receiver accepts frontend options for its "
                         "explicit workspace only");
    else {
      cai_app_client_config checked = *target;
      rc = cai_cli_workspace_claims_build(&options, configuration, 1,
                                          &requirements, error);
      if (rc == CAI_OK)
        rc = cai_cli_workspace_claims_merge(&requirements, target->requirements,
                                            target->requirement_count, error);
      if (rc == CAI_OK) {
        checked.requirements = requirements.values;
        checked.requirement_count = requirements.count;
        rc = cai_config_environment(configuration, &environment, error);
        options.environment = environment;
        if (rc == CAI_OK)
          rc = cai_cli_frontend_run(application, &options, &checked, NULL,
                                    exit_code, error);
      }
    }
    if (configured == 0)
      *exit_code = 0;
  }
  cai_environment_close(environment);
  cai_cli_workspace_claims_cleanup(&requirements);
  cai_config_close(configuration);
  for (i = 0; i <= argc; ++i)
    free(arguments[i]);
  free(arguments);
  return rc;
}
