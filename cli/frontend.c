#define _GNU_SOURCE
#include "frontend.h"
#include "commands.h"
#include "completion.h"
#include "config.h"
#include "error.h"
#include "input_socket.h"
#include "input_wire.h"
#include "replay_tools.h"
#include "startup.h"
#include "status.h"
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
#include <sys/timerfd.h>
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
  sl_watch_id_t socket_watch, boot_watch, timer_watch, wake_watch;
  int socket_fd, boot_fd;
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
  int history_requested, history_loaded;
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
  int wake[2], timer;
  sl_watch_id_t socket_watch, boot_watch, timer_watch, wake_watch;
  int socket_fd, boot_fd, failed, quit, ready, bound, subscribed, replaying;
  int action_field, shell, awaiting_bind, welcomed, control;
  uint64_t bind_request, subscribe_request, control_request;
  unsigned long long startup_request, startup_producer;
  char startup_session[129];
  cai_cli_startup startup;
  int startup_command;
  int startup_control, startup_feeding;
  struct {
    uint64_t request, producer;
    char session[129];
  } hidden_recall[CAI_CLI_STARTUP_INSTRUCTIONS];
  size_t action_length;
  char *action;
  size_t command_length;
  char *command;
  cai_cli_tool_text tool_text;
} frontend;

/** One terminal, several explicitly visited owners. Parked sockets continue
 * processing authority/queue observations, but never render into the selected
 * terminal. Their attachment keeps nonpersistent owners alive until quit. */
struct frontend_group {
  frontend *first, *active, *candidate;
  cai_workspace_discovery *discovery;
  sl_watch_id_t discovery_watch;
  int discovery_fd, listing, listed, switching, requested;
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
  if ((e->phase == CAI_APP_CATALOG_END || e->phase == CAI_APP_CATALOG_ERROR) &&
      cai_cli_completions_open(&f->current->completions, f->ui->sl,
                               f->target.workspace, completion_catalog,
                               f) != SL_OK)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "refresh completion labels");
  return CAI_OK;
}
static int quit_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int enter_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
static int cancel_key(sl_t *, sl_key_t, void *, sl_key_action_t *);
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
static int notice(frontend *f, const char *message) {
  if (f->group->active != f)
    return 0;
  return cai_cli_presenter_notice(f->ui, message);
}
static void save_submission(void *context, const cai_input_request *request) {
  frontend *f = context;
  cai_error local;
  recall_pending *pending = NULL;
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
  int busy = 0;
  memset(&goal, 0, sizeof(goal));
  memset(&metrics, 0, sizeof(metrics));
  if (f->client != NULL &&
      cai_app_client_get_view(f->client, &view, NULL) == CAI_OK &&
      view != NULL && view->available) {
    metrics = view->metrics;
    busy = metrics.turn_active;
    cai_app_view_elapsed(view, clock_ms(), &elapsed, &goal_seconds);
    metrics.turn_elapsed_ms = elapsed;
    goal.has_goal = view->goal.has_goal;
    goal.status = view->goal.status;
    goal.elapsed_seconds = goal_seconds;
    cai_cli_status_build(&f->status, view->effective.model,
                         view->effective.reasoning_effort,
                         view->effective.service_tier,
                         metrics.context_window_tokens > 0
                             ? 100.0 * (double)metrics.context_used_tokens /
                                   (double)metrics.context_window_tokens
                             : 0.0,
                         metrics.has_context_usage, &view->quota,
                         metrics.session_usage.estimated_spend_usd,
                         metrics.session_usage.estimated_spend_eur,
                         view->provider, &goal, view->yolo);
    if (cai_cli_status_apply(f->ui->sl, &f->status) != 0)
      return -1;
  }
  if (f->ready && f->history_loaded && f->skills_ready &&
      f->skill_conversation == f->state.conversation && !f->welcomed) {
    snprintf(message, sizeof(message), "%s",
             f->state.transport.controller != f->state.transport.attachment
                 ? "Observing workspace. /takeover acquires control. "
                   "Send /help for usage."
                 : "Conversation attached. Send /help for usage.");
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
  else if (!f->ready)
    snprintf(message, sizeof(message), "Connecting to workspace server...");
  else if (f->state.transport.controller != f->state.transport.attachment)
    snprintf(message, sizeof(message),
             "Observing workspace. /takeover acquires control.");
  else if (cai_cli_turn_status_message(message, sizeof(message), NULL,
                                       &metrics) != 0)
    snprintf(message, sizeof(message), "Ready. /help for usage.");
  if (message[0] == '\0')
    snprintf(message, sizeof(message), "Ready. Send /help for usage.");
  return sl_set_status_busy(f->ui->sl, busy) == SL_OK &&
                 sl_set_status_spinner(f->ui->sl, busy) == SL_OK &&
                 sl_set_status_message(f->ui->sl, message) == SL_OK
             ? 0
             : -1;
}

static int observed(void *context, const cai_app_client_event *event,
                    cai_error *error) {
  frontend *f = context;
  f->state = *event->state;
  if (f->queue != NULL) {
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
  if (event->type == CAI_APP_CLIENT_STATUS && event->status->available &&
      f->pending_context && event->status->conversation != f->current->epoch) {
    snprintf(f->next_session, sizeof(f->next_session), "%s",
             event->status->session_id);
    f->switching = 1;
  }
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
    if (event->input->result.code != CAI_OK)
      (void)notice(f, event->input->result.reason);
    if (f->exit_save != 0U && event->request == f->exit_save) {
      if (event->input->result.code == CAI_OK && f->current == f->exit_view &&
          strcmp(sl_buffer(f->ui->sl), f->exit_draft) == 0)
        f->quit = 1;
      else if (event->input->result.code == CAI_OK)
        (void)notice(f, "Draft changed while saving; exit cancelled.");
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
    f->bound = 1;
    f->ready = 1;
    snprintf(f->current->session, sizeof(f->current->session), "%s",
             f->state.session);
    f->current->producer = f->state.producer;
    f->current->epoch = f->state.conversation;
    if (cai_cli_completions_open(&f->current->completions, f->ui->sl,
                                 f->target.workspace, completion_catalog,
                                 f) != SL_OK)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "bind completion catalog");
  } else if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
             event->input->type == CAI_INPUT_OBSERVATION_DISCONNECTED &&
             !f->state.bound) {
    if (f->queue != NULL) {
      (void)cai_cli_input_socket_disconnect(f->queue, NULL);
      f->queue = NULL;
    }
    f->pending_context = 1;
    f->ready = f->bound = f->awaiting_bind = f->subscribed = 0;
  }
  return CAI_OK;
}
static int transcript(void *context, const cai_transcript_event *event,
                      cai_error *error) {
  frontend *f = context;
  if (f->group->active != f)
    return CAI_OK;
  if (event->kind == CAI_TRANSCRIPT_EVENT_BEGIN && event->type != NULL) {
    int review = strcmp(event->type, "review_pending") == 0;
    if ((review || strcmp(event->type, "assistant_text_delta") == 0 ||
         strcmp(event->type, "input_queue_delivery") == 0 ||
         strcmp(event->type, "reasoning_summary_started") == 0) &&
        cai_cli_presenter_set_theme_scope(f->ui, review ? 2 : 0, error) !=
            CAI_OK)
      return CAI_ERR_TRANSPORT;
    /* /review's accepted command already carries its shared start/end
     * documents. Only the model-invoked tool needs local lifecycle headings. */
  }
  if (event->kind == CAI_TRANSCRIPT_REPLAY_BEGIN) {
    f->replaying = 1;
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
    f->resume_actions = 0;
    f->resume_collection[0] = '\0';
    f->resume_position = f->resume_sequence = 0U;
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
    }
    return notice(f, event->reason != NULL ? event->reason
                                           : "Transcript unavailable") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  return cai_cli_transcript_presenter_event(f->transcript, event, error);
}

/** Actions are bounded accepted input commands, not output documents.
 * Never run an action from replay or from another producer's transcript. */
static int action_chunk(frontend *f, const cai_execution_record *record,
                        int replay, cai_error *error) {
  char source[129], tail;
  unsigned long long producer, request, item;
  size_t length = strlen(record->data);
  if ((replay && !f->resume_actions) || record->call_id == NULL ||
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
  if (!event->replay && strcmp(r->kind, "command.completed") == 0 &&
      r->call_id != NULL && f->history_command[0] != '\0' &&
      strcmp(r->call_id, f->history_command) == 0) {
    f->history_requested = f->history_loaded = f->current->history_loaded = 0;
    (void)sl_history_set_auto_add(f->ui->sl, 0);
    f->history_command[0] = '\0';
  }
  f->ui->presentation_event = 1;
  f->ui->presentation_timestamp_ns = r->timestamp_ns;
  if (cai_cli_presenter_set_theme_scope(f->ui,
                                        r->subagent == NULL ? 0
                                        : strcmp(r->subagent, "review") == 0
                                            ? 2
                                            : 1,
                                        error) != CAI_OK)
    return CAI_ERR_TRANSPORT;
  if (strcmp(r->kind, "tool.started") == 0 && r->tool_name != NULL &&
      strcmp(r->tool_name, "run_review") == 0 && !f->review_open) {
    f->review_open = 1;
    if (markdown(f, "**Review started.**\n\n",
                 strlen("**Review started.**\n\n")) != 0 ||
        cai_cli_presenter_finish_response(f->ui) != 0)
      return CAI_ERR_TRANSPORT;
  }
  if (f->startup_request != 0U && f->startup_command && r->call_id != NULL &&
      (strcmp(r->kind, "command.completed") == 0 ||
       strcmp(r->kind, "command.failed") == 0)) {
    char source[129], tail;
    unsigned long long producer, request, item;
    if (sscanf(r->call_id, "command:%128[^:]:%llu:%llu:%llu%c", source,
               &producer, &request, &item, &tail) == 4 &&
        producer == f->startup_producer && request == f->startup_request &&
        strcmp(source, f->startup_session) == 0) {
      f->startup_request = 0U;
      if (strcmp(r->kind, "command.failed") == 0)
        f->startup.stage = CAI_CLI_STARTUP_DONE;
    }
  }
  if (strncmp(r->kind, "tool.", 5U) == 0 ||
      strcmp(r->kind, "hosted.tool") == 0 ||
      strcmp(r->kind, "reasoning.receipt") == 0) {
    int handled = 0;
    if (cai_cli_presenter_finish_response(f->ui) != 0 ||
        cai_cli_presenter_finish_reasoning(f->ui) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cai_cli_replay_tools_follow(&f->tools, r, f->ui->tool_display,
                                     f->full_output, 72U, tool_markdown, f,
                                     &handled, error);
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
      if (strncmp(f->command, "/history", 8U) == 0 &&
          (f->command[8] == ' ' || f->command[8] == '\t') &&
          r->call_id != NULL && strlen(r->call_id) < sizeof(f->history_command))
        snprintf(f->history_command, sizeof(f->history_command), "%s",
                 r->call_id);
      rc = cai_cli_presenter_input(f->ui, f->command, 1);
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
      rc = markdown(f, "**Command failed.**\n\n", 21U);
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
                    strcmp(r->parent_call_id, "/review") != 0))
      rc = markdown(f, "**Review finished.**\n",
                    strlen("**Review finished.**\n"));
    if (rc == 0)
      rc = cai_cli_presenter_finish_response(f->ui);
    f->review_open = 0;
  } else if (event->subagent && strcmp(r->kind, "assistant_text_delta") == 0 &&
             (r->subagent == NULL || strcmp(r->subagent, "review") != 0))
    rc = cai_cli_presenter_text(f->ui, r->data, length);
  else if (event->subagent && strcmp(r->kind, "assistant_text_end") == 0) {
    rc = cai_cli_presenter_finish_response(f->ui);
    if (rc == 0)
      rc = cai_cli_presenter_finish_reasoning(f->ui);
    cai_cli_reasoning_end(&f->current->child_reasoning);
  } else if (strcmp(r->kind, "turn.cancelled") == 0) {
    rc = cai_cli_presenter_finish_response(f->ui);
    if (rc == 0)
      rc = cai_cli_presenter_finish_reasoning(f->ui);
    if (rc == 0)
      rc =
          markdown(f, "**Turn cancelled.**\n", strlen("**Turn cancelled.**\n"));
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
  if (f->group->active != f)
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
  if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0)
    f->quit = 1;
  else if (strncmp(line, "/theme ", 7U) == 0 || strncmp(line, "/t ", 3U) == 0) {
    cai_cli_theme selected;
    const char *why = cai_cli_theme_parse(strchr(line, ' ') + 1U, &selected);
    if (why != NULL)
      rc = notice(f, why);
    else if (cai_cli_presenter_set_theme(f->ui, &selected, &f->error) != CAI_OK)
      return -1;
    else
      rc = markdown(f, "Theme changed.\n", 15U);
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
      if (errno != 0 || end == argument || *end != '\0')
        rc = notice(f, "Theme offset must be a signed integer.");
      else {
        f->theme_offsets[scope] = selected;
        f->ui->theme_offsets[scope] = selected;
        rc = cai_cli_presenter_set_theme(f->ui, &f->theme, &f->error);
      }
    }
    if (rc == 0) {
      snprintf(message, sizeof(message), "Theme offset: **%lld**.\n",
               f->theme_offsets[scope]);
      rc = markdown(f, message, strlen(message));
    }
  } else if (strncmp(line, "/output", 7U) == 0) {
    const char *argument = line + 7U;
    const char *message;
    argument += strspn(argument, " \t");
    if (strcmp(argument, "full") == 0)
      f->full_output = 1;
    else if (strcmp(argument, "compact") == 0)
      f->full_output = 0;
    else if (*argument != '\0')
      rc = notice(f, "Use /output compact or /output full.");
    f->ui->tool_output = f->full_output ? "full" : "compact";
    message = f->full_output ? "Tool output: **full**.\n"
                             : "Tool output: **compact**.\n";
    if (rc == 0)
      rc = markdown(f, message, strlen(message));
  } else if (strcmp(line, "/shell") == 0 || strcmp(line, "!sh") == 0)
    f->shell = 1;
  else
    rc = notice(f, "This frontend action is not connected yet.");
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
  if (f->quit || f->shell || f->switching || f->group->switching)
    return sl_suspend(f->ui->sl);
  return SL_OK;
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
  struct pollfd descriptor;
  struct itimerspec timer;
  uint64_t deadline = 0U, boot_deadline = 0U, now = clock_ms();
  const cai_app_view *view = NULL;
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
 * require their application record, including across a conversation switch. */
static int startup_input(frontend *f) {
  char *text = NULL;
  int rc, control;
  sl_queue_request_t request;
  sl_queue_request_id_t id;
  if (!f->ready || !f->subscribed || f->replaying || f->startup_request != 0U ||
      !f->state.transcript.active || f->state.transcript.subscribing ||
      f->state.transcript.replaying ||
      f->state.transport.controller != f->state.transport.attachment ||
      f->startup.stage >= CAI_CLI_STARTUP_DONE)
    return 0;
  rc = cai_cli_startup_next(&f->startup, f->options, f->state.session, &text,
                            &control, &f->error);
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
                             ? CAI_CLI_STARTUP_DONE
                             : CAI_CLI_STARTUP_SELECT;
      if (connect_target(f) != CAI_OK)
        return -1;
    }
  }
  if (f->client != NULL) {
    rc = cai_app_client_pump(f->client, clock_ms(), &f->error);
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
    if (f->pending_context)
      return status(f) == 0 ? interests(f) : -1;
    if (f->state.transport.phase == CAI_WORKSPACE_CONNECTED && !f->bound &&
        !f->awaiting_bind) {
      rc = cai_app_client_bind(
          f->client, ++f->bind_request, 0U, f->current->producer,
          f->current->session[0] != '\0' ? f->current->session : NULL,
          &f->error);
      if (rc != CAI_OK)
        return -1;
      f->awaiting_bind = 1;
    }
    if (f->bound && !f->subscribed) {
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
    if (actions(f) != 0)
      return -1;
    if (startup_input(f) != 0)
      return -1;
  }
  return status(f) == 0 ? interests(f) : -1;
}
static int quit_key(sl_t *sl, sl_key_t key, void *context,
                    sl_key_action_t *action) {
  frontend *f = context;
  (void)sl;
  (void)key;
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
  f->quit = 1;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}
static int cancel_key(sl_t *sl, sl_key_t key, void *context,
                      sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)context;
  /* Match Softline's chat adapter: CANCEL owns stopped-backlog and draft
   * semantics; the outer owner then cancels the remote foreground operation. */
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}
static int enter_key(sl_t *sl, sl_key_t key, void *context,
                     sl_key_action_t *action) {
  frontend *f = context;
  (void)key;
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
      f->control = 3;
      if (sl_set_buffer(sl, "") != SL_OK)
        return SL_ERROR_IO;
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

static int select_view(frontend *f, const char *session) {
  frontend_view *v;
  sl_config_t config;
  int fresh = 0;
  if (f->current != NULL) {
    park_view(f);
    cai_cli_transcript_presenter_reset(f->transcript);
    cai_cli_replay_tools_close(f->tools);
    f->current->tools = f->tools = NULL;
    if (sl_output_stream_end(f->ui->sl) != SL_OK)
      return -1;
  }
  for (v = f->views; v != NULL; v = v->next)
    if (session != NULL && strcmp(v->session, session) == 0)
      break;
  if (v == NULL) {
    v = calloc(1U, sizeof(*v));
    if (v == NULL)
      return cai_cli_error(&f->error, CAI_ERR_NOMEM,
                           "allocate conversation editor");
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
    config.line_max_len = CAI_CLI_INPUT_TEXT_MAX;
    cai_cli_input_adapter_configure(cai_cli_input_socket_adapter(f->queue),
                                    &config);
    f->ui->sl = sl_create_with_config(&config);
    if (f->ui->sl == NULL ||
        cai_cli_input_adapter_bind(cai_cli_input_socket_adapter(f->queue),
                                   f->ui->sl, &f->error) != CAI_OK ||
        sl_set_statusline(f->ui->sl, 1, 0) != SL_OK ||
        sl_history_set_max_len(f->ui->sl, f->options->prompt_history_limit) !=
            SL_OK ||
        sl_history_set_auto_add(f->ui->sl, 0) != SL_OK ||
        sl_set_prompt_queue_profile(
            f->ui->sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) != SL_OK ||
        sl_set_prompt_theme(f->ui->sl, f->theme.prompt) != SL_OK ||
        cai_cli_completions_open(&v->completions, f->ui->sl,
                                 f->target.workspace, completion_catalog,
                                 f) != SL_OK ||
        sl_watch_add(f->ui->sl, f->wake[0], SL_WATCH_READ, event_ready, f,
                     &f->wake_watch) != SL_OK ||
        sl_watch_add(f->ui->sl, f->timer, SL_WATCH_READ, event_ready, f,
                     &f->timer_watch) != SL_OK ||
        sl_bind_key(f->ui->sl, SL_KEY_ALT_BASE + 'q', quit_key, f) != SL_OK ||
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
  f->resume_actions = 0;
  f->history_requested = f->history_loaded = v->history_loaded;
  f->history_limit = v->history_loaded
                         ? v->history_limit
                         : (unsigned int)f->options->prompt_history_limit;
  f->history_length = 0U;
  f->action_field = f->action_length = f->command_length = f->review_open = 0U;
  park_view(f);
  return sl_output_stream_begin(f->ui->sl) == SL_OK ? 0 : -1;
}

static frontend *frontend_open(frontend_group *group,
                               const cai_app_config *application,
                               const cai_cli_options *options,
                               const cai_app_client_config *target,
                               const cai_cli_frontend_boot *boot,
                               cai_error *error) {
  frontend *f = calloc(1U, sizeof(*f));
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
  f->target = *target;
  f->target.workspace = f->workspace;
  f->target.runtime_directory = f->runtime_directory;
  if (target->expected_instance != NULL) {
    memcpy(f->instance, target->expected_instance, sizeof(f->instance));
    f->target.expected_instance = f->instance;
  }
  f->boot = boot;
  /* Discovery decides whether a new owner already received the original argv.
   * An existing owner (including the winner of a launch race) gets explicit
   * intents exactly once through admissions after its replay completes. */
  f->startup.stage = CAI_CLI_STARTUP_SELECT;
  f->theme_offsets[0] = options->theme_offset;
  f->theme_offsets[1] = options->subagent_theme_offset;
  f->theme_offsets[2] = options->review_theme_offset;
  f->full_output =
      options->tool_output != NULL && strcmp(options->tool_output, "full") == 0;
  f->wake[0] = f->wake[1] = f->timer = f->socket_fd = f->boot_fd = -1;
  f->parked_fd = -1;
  cai_error_init(&f->error);
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
  if (group->active != NULL && sl_output_stream_end(f->ui->sl) != SL_OK) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "park new workspace editor");
    return NULL;
  }
  cai_cli_status_init(&f->status, target->workspace, getenv("HOME"));
  (void)cai_cli_status_fields_parse(options->status_fields, &f->status.fields);
  if (group->active == NULL && application->banner != NULL &&
      cai_cli_presenter_sink(f->ui, application->banner,
                             strlen(application->banner)) != 0) {
    cai_cli_error(error, CAI_ERR_TRANSPORT, "write frontend banner");
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
            if (p != NULL)
              p->startup.stage = CAI_CLI_STARTUP_DONE;
            else
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
    snprintf(f->resume_collection, sizeof(f->resume_collection), "%s",
             f->state.transcript.collection);
    f->resume_position = f->state.transcript.position;
    f->resume_sequence = f->state.transcript.sequence;
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
    char *line = sl_next_prompt(f->ui->sl, "> ", &source);
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
      const char *command = control == 1   ? "/takeover"
                            : control == 2 ? "/shutdown"
                                           : "/quit";
      f->control = 0;
      if (cai_cli_presenter_input(f->ui, command, 1) != 0)
        f->failed = 1;
      else if (control == 3)
        f->quit = 1;
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
      int code;
      f->shell = 0;
      if (sl_terminal_local(f->ui->sl, NULL, &code) < 0)
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
      } else
        f->quit = 1;
    }
    if (!f->quit && service(f) != 0)
      f->failed = 1;
  }
  if (f->failed)
    rc = cai_cli_error(
        error, f->error.code != CAI_OK ? f->error.code : CAI_ERR_TRANSPORT,
        f->error.message != NULL ? f->error.message
                                 : "terminal attachment failed");
done:
  *exit_code = rc == CAI_OK ? 0 : 1;
  cai_workspace_discovery_close(group.discovery);
  free(group.selector);
  close_deadline = clock_ms() + 2000U;
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
  cai_cli_workspace_claims requirements;
  cai_config_context context;
  cai_config *configuration = NULL;
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
    if (configured != 1)
      rc = configured == 0 ? CAI_OK : CAI_ERR_INVALID;
    else if (strcmp(options.directory, target->workspace) != 0 ||
             options.server != NULL || options.headless || options.temporary ||
             options.list || options.list_all || options.resume_list ||
             options.delete_list || options.delete_id != NULL ||
             options.export_id != NULL || options.import_file != NULL ||
             options.login || options.chrome_setup || options.chrome_remove ||
             options.install_skill != NULL || options.message != NULL ||
             options.non_interactive || options.review)
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
        rc = cai_cli_frontend_run(application, &options, &checked, NULL,
                                  exit_code, error);
      }
    }
    if (configured == 0)
      *exit_code = 0;
  }
  cai_config_close(configuration);
  for (i = 0; i <= argc; ++i)
    free(arguments[i]);
  free(arguments);
  return rc;
}
