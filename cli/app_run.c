/** @file
 * Workspace owner: runtime/store lifetime, backend queue delivery and semantic
 * command documents. Terminal adapters consume its public socket channels;
 * this unit owns no editor, renderer, console replay or terminal descriptors.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "app_internal.h"
#include "app_startup.h"
#include "command_help.h"
#include "commands.h"
#include "config.h"
#include "documents.h"
#include "export_job.h"
#include "identity.h"
#include "instructions.h"
#include "log.h"
#include "mcp.h"
#include "messages.h"
#include "options.h"
#include "pouch.h"
#include "quota_refresh.h"
#include "review_report.h"
#include "runtime_options.h"
#include "session_commands.h"
#include "shell.h"
#include "skills.h"
#include "status.h"
#include "stores.h"
#include "temp.h"
#include "todo.h"
#include "workspace_host.h"
#include <cai/chrome.h>
#include <cai/plugins.h>
#ifdef CAI_APP_LUA_TOOLS
#include <cai/lua_tools.h>
#endif

#include "goal_input.h"
#include "workspace_claims.h"
#include <cai/agent_runtime.h>
#include <cai/auth.h>
#include <cai/execution.h>
#include <cai/quota.h>
#include <cai/session_store.h>
#include <cai/workspace_server.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef cai_cli_session cli_session;

typedef struct cli_state {
  const cai_app_config *application;
  cai_app_bridge *bridge;
  cai_cli_workspace_host *workspace_host;
  int host_stopped;
  size_t batch_pending_turns;
  int batch_frontend, settlement_pending;
  cai_cli_options options;
  cai_cli_options normal_options;
  cai_cli_temp temporary;
  int borrowed_configuration, borrowed_temporary;
  int review_block;
  char *normal_reasoning_override, *high_reasoning_override;
  char *initial_review_instructions;
  cai_config *configuration;
  cai_environment *environment;
  cai_agent_session_store store;
  cai_cli_pouch pouch;
  cai_cli_pouch auth_pouch, messaging_pouch;
  cai_chatgpt_auth *auth;
  cai_chatgpt_auth shared_auth;
  pthread_mutex_t auth_lock;
  int has_auth_lock;
  cai_client *client;
  cai_client *model_client;
  pslog_logger *model_log;
  cai_client *quota_client;
  cai_agent_runtime *runtime;
  cai_plugins *plugins;
  int owns_plugins;
#ifdef CAI_APP_LUA_TOOLS
  cai_lua_tools *lua_tools;
#endif
  cai_agent_runtime *review_runtime;
  char review_call[256];
  int foreground_subagent;
  cai_agent_runtime_metrics last_review_metrics;
  int has_review_metrics;
  cai_cli_mcp mcp;
  int mcp_initialized;
  int mcp_notice_pending;
  cai_cli_messages *messages;
  cai_cli_documents documents;
  cai_cli_export_job *export_job;
  cai_input_controller *input_controller;
  cai_input_queue *input;
  cai_input_queue_client *input_admin;
  unsigned long long input_request;
  cai_cli_log *log;
  int frontend_startup;
  char generated_goal[4096];
  cli_session *sessions;
  size_t session_count;
  int exit_requested;
  int was_busy;
  int awaiting_turn;
  int suppress_replay;
  int sandbox_notice_emitted;
  int automation_done;
  int automation_failed;
  size_t goal_starts_pending;
  int starting_goal;
  size_t startup_goal_pending;
  size_t next_instruction;
  /* Notification fields only. The core queue owns commands until delivery;
   * runtime owns accepted updates. No command or requested value is copied
   * here. */
  unsigned int settings_notice_fields;
  cai_chatgpt_quota quota;
  cai_cli_quota_refresh quota_refresh;
  int list_line_start;
  cai_cli_shell shell;
  unsigned long long shell_sequence, shell_offset;
  char shell_call[256];
  int shell_stream_ended, shell_stopping;
  char workspace[PATH_MAX];
  char output_path[PATH_MAX];
  char workspace_approval_root[PATH_MAX];
  int workspace_approval_pending;
  char message_queue_override[129];
  char clipboard_directory[PATH_MAX];
} cli_state;

static pslog_logger *cli_logger(const cli_state *state) {
  return state->log != NULL ? state->log->logger : NULL;
}
static int cli_session_log(cli_state *state, const char *id, cai_error *error) {
  int rc = cai_cli_log_session(state->log, id, error);
  if (rc == CAI_OK)
    rc = cai_app_bridge_log(
        state->bridge, state->log->fd >= 0 ? state->log->path : NULL, error);
  return rc;
}

static int cli_advance_automation(cli_state *state, cai_error *error);
static int cli_messages_process(cli_state *state, int at_boundary,
                                cai_error *error);
static int cli_document_progress(void *context, cai_error *error) {
  cli_state *state = context;
  return cai_cli_workspace_host_progress(state->workspace_host, error);
}
static int cli_dispatch_command(cli_state *state, const char *line,
                                const cai_input_claim *claim, cai_error *error,
                                int *handled);
static int cli_submit_automation(cli_state *state, const char *instruction,
                                 cai_error *error);
static int cli_continue_goal(cli_state *state, cai_error *error);
static int cli_host_process(cli_state *state, cai_error *error);
static int cli_pump(cli_state *state, long timeout, cai_error *error);
static int cli_export_pump(cli_state *, cai_error *);
static int cli_sync_queue(cli_state *state, int at_boundary, cai_error *error);
static int cli_review_pump(void *);
static int cli_sync_status(cli_state *state, cai_error *error);
static int cli_select_startup(void *, const char *, cai_error *);
static int cli_settings_notice(cli_state *state, cai_error *error);
static int cli_review_event(void *context, const cai_agent_runtime_event *event,
                            cai_error *error);

static int cli_load_model_metadata(const cai_cli_options *options, char **out,
                                   cai_error *error) {
  char path[PATH_MAX];
  char message[PATH_MAX + 128];
  const char *root;
  FILE *file;
  long length;
  size_t size;
  int written;

  *out = NULL;
  if (options->model_metadata != NULL) {
    written = snprintf(path, sizeof(path), "%s", options->model_metadata);
  } else {
    root = options->config_dir;
    if (root != NULL) {
      written = snprintf(path, sizeof(path), "%s/models.json", root);
    } else {
      return CAI_OK;
    }
  }
  if (written < 0 || (size_t)written >= sizeof(path)) {
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "model metadata path is too long");
  }
  file = fopen(path, "rb");
  if (file == NULL) {
    if (options->model_metadata == NULL && errno == ENOENT) {
      return CAI_OK;
    }
    snprintf(message, sizeof(message), "cannot open model metadata %s: %s",
             path, strerror(errno));
    return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
  }
  if (fseek(file, 0L, SEEK_END) != 0 || (length = ftell(file)) < 0L ||
      length > 1024L * 1024L || fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    snprintf(message, sizeof(message),
             "cannot read model metadata %s (file must be at most 1 MiB)",
             path);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  *out = (char *)malloc((size_t)length + 1U);
  if (*out == NULL) {
    fclose(file);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "cannot allocate model metadata file");
  }
  size = fread(*out, 1U, (size_t)length, file);
  if (fclose(file) != 0 || size != (size_t)length ||
      memchr(*out, '\0', size) != NULL) {
    free(*out);
    *out = NULL;
    snprintf(message, sizeof(message),
             "cannot read model metadata %s (read error or embedded NUL)",
             path);
    return cai_cli_error(error, CAI_ERR_INVALID, message);
  }
  (*out)[size] = '\0';
  return CAI_OK;
}

static void cli_print_error(cli_state *state, const char *operation,
                            const cai_error *error) {
  if (error != NULL && error->code == CAI_ERR_CANCELLED &&
      cai_app_bridge_stopping(state->bridge))
    return; /* Orderly startup/replay STOP is not an application failure. */
  cai_app_bridge_error(state->bridge, error);
  cai_cli_log_error(cli_logger(state), operation, error);
}

static void cli_diagnostic(cli_state *state, pslog_level level,
                           const char *text) {
  cai_cli_log_message(cli_logger(state), level, "diagnostic", "diagnostic",
                      text, strlen(text), NULL, 0U);
}

/* Both clients share credentials, while token refresh and token copies are
 * serialized. Network requests use their own client and copied bearer token. */
static int cli_auth_token(cai_chatgpt_auth *auth, char **out,
                          cai_error *error) {
  cli_state *state = auth->impl;
  int rc;
  pthread_mutex_lock(&state->auth_lock);
  rc = state->auth->access_token(state->auth, out, error);
  pthread_mutex_unlock(&state->auth_lock);
  return rc;
}

static int cli_auth_refresh(cai_chatgpt_auth *auth, cai_error *error) {
  cli_state *state = auth->impl;
  int rc;
  pthread_mutex_lock(&state->auth_lock);
  rc = state->auth->refresh(state->auth, error);
  pthread_mutex_unlock(&state->auth_lock);
  return rc;
}

static int cli_auth_account(cai_chatgpt_auth *auth, char **out,
                            cai_error *error) {
  cli_state *state = auth->impl;
  int rc;
  pthread_mutex_lock(&state->auth_lock);
  *out = NULL;
  rc = state->auth->account_id != NULL
           ? state->auth->account_id(state->auth, out, error)
           : CAI_OK;
  pthread_mutex_unlock(&state->auth_lock);
  return rc;
}

static int cli_fetch_quota(void *context, cai_chatgpt_quota *out,
                           cai_error *error) {
  return cai_client_chatgpt_quota((cai_client *)context, out, error);
}

static void cli_refresh_quota(cli_state *state, int force) {
  cai_cli_quota_request(&state->quota_refresh, force, force);
  (void)cai_cli_quota_take(&state->quota_refresh, &state->quota);
}

/** Command output is semantic Markdown journal data; only attachments render.
 */
static int cli_finish_response(cli_state *state) {
  return cai_cli_documents_finish(&state->documents);
}
static int cli_text(cli_state *state, const char *data, size_t length) {
  return cai_cli_documents_write(&state->documents, data, length);
}
static int cli_markdown(cli_state *state, const char *text) {
  return cli_finish_response(state) == 0 &&
                 cli_text(state, text, strlen(text)) == 0 &&
                 cli_finish_response(state) == 0
             ? 0
             : -1;
}
static int cli_review_boundary(cli_state *state, int start, cai_error *error) {
  int rc =
      cli_markdown(state, start ? CAI_CLI_REVIEW_START : CAI_CLI_REVIEW_END) ==
              0
          ? CAI_OK
          : cai_cli_error(error, CAI_ERR_TRANSPORT, "write review boundary");
  if (rc == CAI_OK)
    state->review_block = start;
  return rc;
}

static int cli_command_input(cli_state *state, const char *text) {
  pslog_field field =
      pslog_str("prompt_kind", text[0] == '!' ? "shell_command" : "command");
  cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO, "command_input",
                      "user", text, strlen(text), &field, 1U);
  return CAI_OK;
}

static int cli_shell_output(void *context, const char *bytes, size_t length,
                            int *accepted, cai_error *error) {
  cli_state *state = context;
  int rc;
  *accepted = 1;
  if (!state->shell_stopping) {
    if (length > ULLONG_MAX - state->shell_offset)
      return cai_cli_error(error, CAI_ERR_LIMIT, "live shell offset overflow");
    rc = cai_cli_workspace_host_shell(
        state->workspace_host, state->shell_sequence, state->shell_offset,
        bytes, length, 0, accepted, error);
    if (rc != CAI_OK || !*accepted)
      return rc;
  }
  rc = cai_agent_runtime_user_shell_write(state->runtime, bytes, length, error);
  if (rc != CAI_OK)
    return rc;
  state->shell_offset += length;
  return CAI_OK;
}

static int cli_shell_pump(cli_state *state, cai_error *error) {
  char message[160];
  int accepted = 1;
  int rc = cai_cli_shell_pump(&state->shell, cli_shell_output, state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->shell.exited && state->shell.eof) {
    rc = !state->shell_stopping && !state->shell_stream_ended
             ? cai_cli_workspace_host_shell(
                   state->workspace_host, state->shell_sequence,
                   state->shell_offset, NULL, 0U, 1, &accepted, error)
             : CAI_OK;
    if (rc != CAI_OK || !accepted)
      return rc;
    state->shell_stream_ended = 1;
    if (rc == CAI_OK)
      rc = cai_agent_runtime_user_shell_finish(
          state->runtime, state->shell.exit_code,
          cai_cli_shell_elapsed(&state->shell), error);
    if (rc != CAI_OK)
      return rc;
    if (state->shell_call[0] != '\0') {
      char status[32];
      snprintf(status, sizeof(status), "%d", state->shell.exit_code);
      rc = cai_cli_documents_operation(
          &state->documents, state->shell_call, "command.shell_settled",
          "exit_code", status,
          state->shell_stopping         ? CAI_ERR_CANCELLED
          : state->shell.exit_code == 0 ? CAI_OK
                                        : CAI_ERR_TRANSPORT,
          error);
      if (rc != CAI_OK)
        return rc;
      state->shell_call[0] = '\0';
    }
    snprintf(message, sizeof(message), "**Shell command exited %d** (%.3fs).",
             state->shell.exit_code,
             (double)cai_cli_shell_elapsed(&state->shell) / 1000.0);
    cai_cli_shell_close(&state->shell);
    if (cli_markdown(state, message) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "cannot display shell completion");
  }
  return CAI_OK;
}

static int cli_shell_start(cli_state *state, const char *command,
                           cai_error *error) {
  int rc;
  if (state->shell.pid > 0)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "a shell command is running; cancel it with Ctrl-C first");
  while (*command == ' ' || *command == '\t')
    command++;
  state->shell_stopping = state->shell_stream_ended = 0;
  state->shell_call[0] = '\0';
  if (state->documents.active) {
    snprintf(state->shell_call, sizeof(state->shell_call), "%s",
             state->documents.call);
    rc = cai_cli_documents_operation(&state->documents, state->shell_call,
                                     "command.shell_started", NULL, NULL,
                                     CAI_OK, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_agent_runtime_user_shell_begin(state->runtime, command,
                                          state->options.shell_context, error);
  if (rc != CAI_OK)
    return rc;
  state->shell_offset = 0U;
  {
    unsigned long long last;
    rc = cai_agent_runtime_user_shell_range(
        state->runtime, &state->shell_sequence, &last, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cai_cli_shell_start(&state->shell, command, state->workspace,
                           state->options.environment, error);
  if (rc != CAI_OK) {
    cai_error record_error;
    cai_error_init(&record_error);
    (void)cai_agent_runtime_user_shell_finish(state->runtime, 127, 0U,
                                              &record_error);
    cai_error_cleanup(&record_error);
    return rc;
  }
  return CAI_OK;
}

static int cli_command_at_feed_boundary(const char *line) {
  return cai_cli_command_delivery(line) == CLI_COMMAND_FEED;
}

static int cli_is_yolo(const char *line) {
  return strncmp(line, "/yolo", 5U) == 0 &&
         (line[5] == '\0' || line[5] == ' ' || line[5] == '\t');
}

static int cli_is_compact(const char *line) {
  return strncmp(line, "/compact", 8U) == 0 &&
         (line[8] == '\0' || line[8] == ' ' || line[8] == '\t');
}

/* Agent controls and renderer changes use a response/tool seam, not a new
 * user turn. Both submission keys retain an editable identity until then. */
static int cli_command_at_request_boundary(const char *line) {
  return cai_cli_command_delivery(line) == CLI_COMMAND_REQUEST;
}

/* Queued UI commands share admission; delivery follows their actual contract
 * rather than whether Enter or Alt-Enter happened to produce a Q/S label. */
static int cli_command_at_turn_boundary(const char *line) {
  return cai_cli_command_delivery(line) == CLI_COMMAND_TURN;
}

static cai_input_intent cli_prompt_mode(cli_state *state, const char *text,
                                        cai_input_intent mode) {
  if (mode != CAI_INPUT_STEERING)
    return mode;
  if (cli_command_at_turn_boundary(text))
    return CAI_INPUT_QUEUED;
  if (cai_cli_command_delivery(text) == CLI_COMMAND_PROMPT &&
      !state->options.subagent_steering &&
      (state->review_runtime != NULL || state->foreground_subagent))
    return CAI_INPUT_QUEUED;
  return mode;
}

static int cli_command_turn_ready(cli_state *state, int *ready,
                                  cai_error *error) {
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  int rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc == CAI_OK)
    *ready = !cai_cli_export_job_active(state->export_job) &&
             state->shell.pid <= 0 && state->review_runtime == NULL &&
             !metrics.turn_active && !state->awaiting_turn &&
             run_state != CAI_AGENT_SAMPLING &&
             run_state != CAI_AGENT_DISPATCHING_TOOL;
  return rc;
}

static int cli_yolo_value(const char *line, int *enabled) {
  const char *arg;
  size_t length;
  if (!cli_is_yolo(line))
    return 0;
  arg = line + 5U;
  arg += strspn(arg, " \t");
  length = strlen(arg);
  while (length > 0U && strchr(" \t", arg[length - 1U]) != NULL)
    length--;
  if (length == 2U && strncmp(arg, "on", length) == 0)
    *enabled = 1;
  else if (length == 3U && strncmp(arg, "off", length) == 0)
    *enabled = 0;
  else
    return 0;
  return 1;
}

static int cli_yolo_pending(cli_state *state, int *enabled) {
  cai_input_state pending;
  cai_input_entry entry;
  size_t i;
  if (state->input_admin == NULL ||
      cai_input_queue_get_state(state->input, &pending, NULL) != CAI_OK)
    return 0;
  for (i = 0U; i < pending.count; ++i)
    if (cai_input_queue_entry_at(state->input_admin, i, &entry, NULL) ==
            CAI_OK &&
        !entry.held && cli_yolo_value(entry.text, enabled))
      return 1;
  return 0;
}

/* The backend owns accepted work. Readiness follows runtime boundaries. */
static int cli_prompt_ready(cli_state *state, const char *text,
                            cai_input_intent mode, int at_boundary, int *ready,
                            cai_error *error) {
  int rc;
  *ready = 0;
  if (cli_command_at_feed_boundary(text)) {
    if (strcmp(text, "/tools") == 0 && state->mcp_initialized &&
        !cai_cli_mcp_ready(&state->mcp))
      return CAI_OK;
    *ready = 1;
    return CAI_OK;
  }
  if (state->runtime == NULL)
    return CAI_OK;
  rc = cli_command_turn_ready(state, ready, error);
  if (rc != CAI_OK)
    return rc;
  if (cli_is_compact(text)) {
    int compact_ready = 0;
    if (*ready)
      rc = cai_agent_runtime_can_compact(state->runtime, &compact_ready, error);
    *ready = *ready && compact_ready;
    return rc;
  }
  if (cli_command_at_turn_boundary(text) || state->shell.pid > 0) {
    if (*ready && cli_is_yolo(text)) {
      int requested, effective, change_ready;
      if (cli_yolo_value(text, &requested)) {
        rc = cai_agent_runtime_get_yolo(state->runtime, &effective,
                                        &change_ready, error);
        if (rc == CAI_OK && requested != effective && !change_ready)
          *ready = 0;
      }
    }
    return CAI_OK;
  }
  if (cli_command_at_request_boundary(text)) {
    if (!*ready && at_boundary &&
        (cai_cli_command_is_theme(text) ||
         (state->review_runtime == NULL && !state->foreground_subagent)))
      *ready = 1;
    return CAI_OK;
  }
  if (!*ready && mode == CAI_INPUT_STEERING && at_boundary &&
      (state->options.subagent_steering ||
       (state->review_runtime == NULL && !state->foreground_subagent)))
    *ready = 1;
  return rc;
}

static int cli_input_request(cli_state *state, cai_input_operation operation,
                             const char *text, cai_input_intent intent,
                             int enabled, const cai_input_entry *entry,
                             unsigned long long *id, cai_error *error) {
  cai_input_request request;
  cai_input_result result;
  cai_input_state pending;
  int rc;
  if (state->input_request == ~(unsigned long long)0)
    return cai_cli_error(error, CAI_ERR_LIMIT,
                         "input request identities exhausted");
  memset(&request, 0, sizeof(request));
  request.request_id = ++state->input_request;
  request.operation = operation;
  request.text = text;
  request.intent = intent;
  request.enabled = enabled;
  rc = cai_input_queue_get_state(state->input, &pending, error);
  if (rc != CAI_OK)
    return rc;
  request.queue_revision = pending.revision;
  if (entry != NULL) {
    request.item_id = entry->id;
    request.item_revision = entry->revision;
    request.edit_token = entry->edit_token;
  }
  rc = cai_input_queue_request(state->input_admin, &request, &result, error);
  if (rc == CAI_OK && result.outcome != CAI_INPUT_ACCEPTED)
    rc = cai_cli_error(error,
                       result.code != CAI_OK ? result.code : CAI_ERR_TRANSPORT,
                       result.reason);
  if (rc == CAI_OK && id != NULL)
    *id = result.item_id;
  return rc;
}
static int cli_prompt_classify(void *context, const char *text,
                               cai_input_policy *policy, cai_error *error) {
  cli_state *state = context;
  cai_input_intent mode = policy->intent == CAI_INPUT_STEERING
                              ? CAI_INPUT_STEERING
                              : CAI_INPUT_QUEUED;
  (void)error;
  mode = cli_prompt_mode(state, text, mode);
  policy->intent =
      mode == CAI_INPUT_STEERING ? CAI_INPUT_STEERING : CAI_INPUT_QUEUED;
  policy->action = text[0] == '/' || text[0] == '!';
  policy->independent = cli_command_at_feed_boundary(text) ||
                        cli_command_at_request_boundary(text);
  policy->resume_backlog = !policy->action;
  return CAI_OK;
}
static int cli_sync_queue(cli_state *state, int at_boundary, cai_error *error) {
  cai_input_entry entry;
  cai_input_state pending;
  size_t i;
  int ready, rc;
  if (state->input_admin == NULL)
    return CAI_OK;
  rc = cai_input_queue_get_state(state->input, &pending, error);
  for (i = 0U; rc == CAI_OK && i < pending.count; ++i) {
    cai_input_intent mode;
    rc = cai_input_queue_entry_at(state->input_admin, i, &entry, error);
    if (rc != CAI_OK)
      break;
    mode =
        cli_prompt_mode(state, entry.text,
                        entry.intent == CAI_INPUT_STEERING ? CAI_INPUT_STEERING
                                                           : CAI_INPUT_QUEUED);
    if ((mode == CAI_INPUT_STEERING) != (entry.intent == CAI_INPUT_STEERING)) {
      rc = cli_input_request(state, CAI_INPUT_SET_INTENT, NULL,
                             mode == CAI_INPUT_STEERING ? CAI_INPUT_STEERING
                                                        : CAI_INPUT_QUEUED,
                             0, &entry, NULL, error);
      if (rc == CAI_OK)
        rc = cai_input_queue_entry_at(state->input_admin, i, &entry, error);
    }
    if (rc == CAI_OK)
      rc =
          cli_prompt_ready(state, entry.text, mode, at_boundary, &ready, error);
    if (rc == CAI_OK && ready) {
      int settled = 0;
      rc = cai_input_controller_producer_ready(
          state->input_controller, entry.client_id, &settled, error);
      ready = ready && settled;
    }
    if (rc == CAI_OK)
      rc = cai_input_queue_set_ready(state->input, entry.id, ready, error);
  }
  return rc;
}

/** Socket controls run immediately after generation validation, without
 * entering the trusted host mailbox and outliving that authority. */
static int cli_remote_cancel(void *context, cai_error *error) {
  cli_state *state = context;
  if (cai_cli_export_job_active(state->export_job)) {
    cai_agent_goal_snapshot goal;
    int rc;
    cai_cli_export_job_cancel(state->export_job);
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc == CAI_OK && goal.has_goal && strcmp(goal.status, "active") == 0) {
      state->automation_done = 1;
      rc = cai_agent_runtime_pause_goal(state->runtime, error);
    }
    return rc;
  }
  cai_cli_messages_cancel_send(state->messages);
  if (state->shell.pid > 0) {
    cai_cli_shell_cancel(&state->shell);
    return CAI_OK;
  }
  return cai_agent_runtime_cancel_turn(
      state->review_runtime != NULL ? state->review_runtime : state->runtime,
      error);
}

static void cli_log_event(cli_state *state,
                          const cai_agent_runtime_event *event) {
  static const char *const names[] = {"unknown",
                                      "run_started",
                                      "run_state_changed",
                                      "assistant_text_delta",
                                      "tool_call_started",
                                      "tool_call_completed",
                                      "tool_call_failed",
                                      "steering_queued",
                                      "steering_delivered",
                                      "run_completed",
                                      "run_failed",
                                      "session_committed",
                                      "terminal_command_started",
                                      "terminal_output",
                                      "terminal_waiting",
                                      "terminal_command_completed",
                                      "terminal_command_cancelled",
                                      "turn_queued",
                                      "review_report",
                                      "review_started",
                                      "review_handed_off",
                                      "reasoning_summary",
                                      "response_completed",
                                      "goal_changed",
                                      "subagent_started",
                                      "subagent_handed_off",
                                      "compaction_started",
                                      "compaction_progress",
                                      "compaction_completed",
                                      "run_cancelled",
                                      "tool_patch",
                                      "tool_patch_locations",
                                      "policy_diagnostic",
                                      "reasoning_summary_started",
                                      "input_boundary",
                                      "execution_record",
                                      "request_retry",
                                      "request_retry_started",
                                      "reasoning_tool_result",
                                      "mcp_updated"};
  pslog_field fields[16];
  size_t count;
  const char *name;
  const char *role;
  const char *kind;
  const char *text;
  size_t length;
  pslog_level level;
  /* Execution history has its own durable journal and inspection callbacks.
   * Do not duplicate payload chunks into the ordinary diagnostic logger. */
  if (event->type == CAI_AGENT_EVENT_EXECUTION_RECORD)
    return;
  name =
      event->type > 0 && (size_t)event->type < sizeof(names) / sizeof(names[0])
          ? names[event->type]
          : "unknown";
  role = "runtime";
  kind = NULL;
  level = PSLOG_LEVEL_INFO;
  if (event->type == CAI_AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED) {
    role = "user";
    kind = "policy_diagnostic";
  } else if (event->type == CAI_AGENT_EVENT_RUN_STARTED &&
             event->data == NULL) {
    kind = "compaction";
  } else if (event->type == CAI_AGENT_EVENT_RUN_STARTED ||
             event->type == CAI_AGENT_EVENT_TURN_QUEUED ||
             event->type == CAI_AGENT_EVENT_STEERING_QUEUED ||
             event->type == CAI_AGENT_EVENT_STEERING_DELIVERED) {
    role = "user";
    kind = event->type == CAI_AGENT_EVENT_RUN_STARTED   ? "normal"
           : event->type == CAI_AGENT_EVENT_TURN_QUEUED ? "queued"
                                                        : "steering";
  } else if (event->type == CAI_AGENT_EVENT_TEXT_DELTA ||
             event->type == CAI_AGENT_EVENT_REASONING_SUMMARY ||
             event->type == CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED ||
             event->type == CAI_AGENT_EVENT_REVIEW_REPORT ||
             event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED) {
    role = "assistant";
    kind = event->type == CAI_AGENT_EVENT_REASONING_SUMMARY ||
                   event->type == CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED
               ? "reasoning_summary"
           : event->type == CAI_AGENT_EVENT_REVIEW_REPORT    ? "review_findings"
           : event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED ? "delegation"
                                                             : "normal";
  } else if (event->tool_name != NULL || event->terminal_id != NULL) {
    role = "tool";
  }
  if (event->type == CAI_AGENT_EVENT_RUN_FAILED)
    level = PSLOG_LEVEL_ERROR;
  else if (event->type == CAI_AGENT_EVENT_TOOL_CALL_FAILED ||
           event->type == CAI_AGENT_EVENT_RUN_CANCELLED)
    level = PSLOG_LEVEL_WARN;
  count = 0U;
  fields[count++] = pslog_i64("state", event->state);
  fields[count++] = pslog_u64("sequence", (pslog_uint64)event->sequence);
  if (kind != NULL)
    fields[count++] = pslog_str("prompt_kind", kind);
  if (event->runtime_session_id != NULL)
    fields[count++] = pslog_str("session_id", event->runtime_session_id);
  if (event->parent_tool_call_id != NULL)
    fields[count++] =
        pslog_str("parent_tool_call_id", event->parent_tool_call_id);
  if (event->subagent_name != NULL)
    fields[count++] = pslog_str("subagent", event->subagent_name);
  if (event->tool_name != NULL)
    fields[count++] = pslog_str("tool", event->tool_name);
  if (event->tool_call_id != NULL)
    fields[count++] = pslog_str("tool_call_id", event->tool_call_id);
  if (event->tool_path != NULL)
    fields[count++] = pslog_str("path", event->tool_path);
  if (event->terminal_id != NULL) {
    fields[count++] = pslog_str("terminal_id", event->terminal_id);
    fields[count++] =
        pslog_u64("command_id", (pslog_uint64)event->terminal_command_id);
    if (event->terminal_has_exit_code)
      fields[count++] =
          pslog_i64("exit_code", (pslog_int64)event->terminal_exit_code);
  }
  text = event->data != NULL ? event->data : name;
  length = event->data != NULL ? event->data_length : strlen(name);
  if (event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
    text = event->parent_tool_call_id == NULL ? "Turn stopped."
           : cai_cli_manual_review_call(event->parent_tool_call_id)
               ? "Review stopped."
               : "Subagent stopped.";
    length = strlen(text);
  }
  cai_cli_log_message(cli_logger(state), level, name, role, text, length,
                      fields, count);
  if (event->subagent_instruction != NULL) {
    if (kind != NULL)
      fields[2] = pslog_str("prompt_kind", "delegated");
    else
      fields[count++] = pslog_str("prompt_kind", "delegated");
    cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO,
                        "subagent_instruction", "user",
                        event->subagent_instruction,
                        strlen(event->subagent_instruction), fields, count);
  }
}

/* Backend lifecycle is independent of terminal presentation. */
static int cli_event_lifecycle(cli_state *state,
                               const cai_agent_runtime_event *event,
                               cai_error *error) {
  if (event->parent_tool_call_id == NULL) {
    if (event->type == CAI_AGENT_EVENT_GOAL_CHANGED && state->starting_goal &&
        state->startup_goal_pending > 0U)
      state->startup_goal_pending--;
    if ((state->batch_frontend) && state->batch_pending_turns > 0U &&
        (event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
         event->type == CAI_AGENT_EVENT_RUN_FAILED ||
         event->type == CAI_AGENT_EVENT_RUN_CANCELLED))
      state->batch_pending_turns--;
    if (event->type == CAI_AGENT_EVENT_GOAL_CHANGED ||
        event->type == CAI_AGENT_EVENT_RUN_COMPLETED ||
        event->type == CAI_AGENT_EVENT_RUN_FAILED ||
        event->type == CAI_AGENT_EVENT_RUN_CANCELLED)
      state->settlement_pending = 1;
    if (event->type == CAI_AGENT_EVENT_GOAL_CHANGED &&
        state->goal_starts_pending > 0U && event->data != NULL &&
        ((event->data_length == 8U &&
          memcmp(event->data, "replaced", 8U) == 0) ||
         (event->data_length == 7U && memcmp(event->data, "resumed", 7U) == 0)))
      state->goal_starts_pending--;
    if (event->type == CAI_AGENT_EVENT_RUN_COMPLETED)
      state->awaiting_turn = 0;

    if (event->type == CAI_AGENT_EVENT_RUN_FAILED ||
        event->type == CAI_AGENT_EVENT_RUN_CANCELLED) {
      state->awaiting_turn = 0;
      state->automation_failed = 1;
      state->goal_starts_pending = 0U;
      cai_app_bridge_outcome(state->bridge,
                             event->type == CAI_AGENT_EVENT_RUN_CANCELLED
                                 ? CAI_APP_CANCELLED
                                 : CAI_APP_FAILED);
      if (event->type == CAI_AGENT_EVENT_RUN_FAILED && event->data != NULL) {
        cai_error failed;
        char *message = malloc(event->data_length + 1U);
        cai_error_init(&failed);
        if (message == NULL)
          return cai_cli_error(error, CAI_ERR_NOMEM, "copy runtime failure");
        memcpy(message, event->data, event->data_length);
        message[event->data_length] = '\0';
        failed.code = CAI_ERR_TRANSPORT;
        failed.message = message;
        cai_app_bridge_error(state->bridge, &failed);
        cai_error_cleanup(&failed);
      }
    }
    if (event->type == CAI_AGENT_EVENT_RUN_STARTED) {
      state->settlement_pending = 1;
      state->has_review_metrics = 0;
      cai_app_bridge_outcome(state->bridge, CAI_APP_RUNNING);
      state->awaiting_turn = 1;
      state->automation_failed = 0;
    }
  }
  return CAI_OK;
}

static int cli_event_body(void *context, const cai_agent_runtime_event *event,
                          cai_error *error) {
  cli_state *state;
  state = (cli_state *)context;
  if (state->application != NULL &&
      state->application->event_callback != NULL) {
    int observed = state->application->event_callback(
        state->application->event_context, event, error);
    if (observed != CAI_OK) {
      if (error->code == CAI_OK)
        cai_cli_error(error, observed, "application event observer failed");
      cai_app_bridge_fail(state->bridge, error);
      return observed;
    }
  }
  cli_log_event(state, event);
  if (event->parent_tool_call_id == NULL) {
    int notified = cli_settings_notice(state, error);
    if (notified != CAI_OK)
      return notified;
  }
  if (event->type == CAI_AGENT_EVENT_INPUT_BOUNDARY) {
    int rc = cli_host_process(state, error);
    return rc != CAI_OK ? rc : cli_messages_process(state, 1, error);
  }
  if (event->type == CAI_AGENT_EVENT_MCP_UPDATED) {
    cai_cli_mcp_applied(&state->mcp);
    if (cai_cli_mcp_ready(&state->mcp) && state->mcp_notice_pending) {
      state->mcp_notice_pending = 0;
      return cli_markdown(state, event->data != NULL
                                     ? event->data
                                     : "MCP connections updated.") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    }
    return CAI_OK;
  }
  if (event->type == CAI_AGENT_EVENT_SUBAGENT_STARTED)
    state->foreground_subagent = 1;
  if (event->type == CAI_AGENT_EVENT_SUBAGENT_HANDED_OFF ||
      (event->parent_tool_call_id == NULL &&
       (event->type == CAI_AGENT_EVENT_RUN_FAILED ||
        event->type == CAI_AGENT_EVENT_RUN_CANCELLED)))
    state->foreground_subagent = 0;
  if (event->type == CAI_AGENT_EVENT_REVIEW_HANDED_OFF && state->review_block) {
    int rc = cli_review_boundary(state, 0, error);
    if (rc != CAI_OK)
      return rc;
  }
  return cli_event_lifecycle(state, event, error);
}

static int cli_event(void *context, const cai_agent_runtime_event *event,
                     cai_error *error) {
  return cli_event_body(context, event, error);
}

static int cli_review_event(void *context, const cai_agent_runtime_event *event,
                            cai_error *error) {
  cli_state *state = context;
  cai_agent_runtime_event forwarded = *event;
  forwarded.parent_tool_call_id =
      state->review_call[0] != '\0' ? state->review_call : "/review";
  forwarded.subagent_name = "review";
  return cli_event(context, &forwarded, error);
}

static int cli_publish_status(cli_state *state, cai_agent_runtime *runtime,
                              cai_error *error) {
  cai_agent_runtime_ids ids;
  cai_agent_runtime_metrics activity;
  cai_app_view view;
  unsigned long long conversation;
  const cai_app_status *status;
  int rc = cai_app_bridge_publish(state->bridge, runtime, error);
  if (rc != CAI_OK || state->workspace_host == NULL)
    return rc;
  memset(&activity, 0, sizeof(activity));
  if (state->review_runtime != NULL) {
    rc = cai_agent_runtime_get_metrics(state->review_runtime, &activity, error);
    if (rc != CAI_OK)
      return rc;
  } else if (state->has_review_metrics)
    activity = state->last_review_metrics;
  memset(&ids, 0, sizeof(ids));
  memset(&view, 0, sizeof(view));
  if (runtime != NULL) {
    rc = cai_agent_runtime_get_ids(runtime, &ids, error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_input_controller_conversation(state->input_controller,
                                           &conversation, error);
    if (rc != CAI_OK)
      return rc;
    view.conversation = conversation;
  }
  view.available = 1;
  view.provider = state->options.provider;
  view.last_response_id = ids.response_id;
  view.provider_conversation_id = ids.conversation_id;
  view.quota = state->quota;
  view.yolo = state->options.yolo;
  view.foreground = state->options.foreground;
  view.yield_time_ms = state->options.yield_time_ms;
  /* The cached native status owns these strings. The host compares/copies
   * bounded metadata while borrowed, with no I/O or callbacks under this lock.
   * Goal objectives and detailed errors never enter the wire projection. */
  pthread_mutex_lock(&state->bridge->lock);
  status = &state->bridge->status;
  view.running = status->running;
  view.runtime_ready = status->runtime_ready;
  view.outcome = status->outcome;
  view.exit_code = status->exit_code;
  view.runtime_state = status->state;
  view.session_id = status->session_id;
  view.log_path = status->log_path;
  view.effective = status->effective;
  view.pending = status->pending;
  view.metrics = status->metrics;
  /* Context, usage and identity belong to the conversation. Turn timing
   * belongs to its foreground work, including an isolated manual reviewer.
   * Publish final child timing too; frontends must not invent a local clock
   * or need a parent inference turn merely to show that a review finished. */
  if (state->review_runtime != NULL ||
      (state->has_review_metrics && !view.metrics.turn_active)) {
    view.metrics.turn_active = activity.turn_active;
    view.metrics.turn_elapsed_ms = activity.turn_elapsed_ms;
    view.metrics.has_last_turn = activity.has_last_turn;
    view.metrics.last_turn_duration_ms = activity.last_turn_duration_ms;
    view.metrics.last_turn_finished_unix_seconds =
        activity.last_turn_finished_unix_seconds;
  }
  view.goal.has_goal = status->goal.has_goal;
  view.goal.status = status->goal.status;
  view.goal.has_token_budget = status->goal.has_token_budget;
  view.goal.token_budget = status->goal.token_budget;
  view.goal.tokens_used = status->goal.tokens_used;
  view.goal.remaining_tokens = status->goal.remaining_tokens;
  view.goal.elapsed_seconds = status->goal.elapsed_seconds;
  view.goal.created_at = status->goal.created_at;
  view.goal.updated_at = status->goal.updated_at;
  rc = cai_cli_workspace_host_status(state->workspace_host, &view, error);
  pthread_mutex_unlock(&state->bridge->lock);
  if (state->was_busy != view.metrics.turn_active) {
    state->was_busy = view.metrics.turn_active;
    if (!state->was_busy)
      cli_refresh_quota(state, 0);
  }
  return rc;
}
static int cli_sync_status(cli_state *state, cai_error *error) {
  return cli_publish_status(state, state->runtime, error);
}

/* Deliver only backend-ready entries. Producer readiness preserves frontend
 * edits; request boundaries admit steers and feed commands without a turn. */
static int cli_messages_process(cli_state *state, int at_boundary,
                                cai_error *error) {
  cai_input_state pending;
  cai_input_entry entry;
  cai_input_claim claim;
  size_t index = 0U, applied = 0U;
  int rc;
  if (state->exit_requested || state->input_admin == NULL)
    return CAI_OK;
  rc = cai_input_controller_pump(state->input_controller, 32U, error);
  if (rc == CAI_OK)
    rc = cai_cli_messages_sync(state->messages, state->input, error);
  if (rc == CAI_OK)
    rc = cli_sync_queue(state, at_boundary, error);
  /* Producer readiness is published by the frontend after its native editor
   * has resolved buffered keys, edits and admission receipts. */
  while (rc == CAI_OK && applied < 32U) {
    rc = cai_input_queue_get_state(state->input, &pending, error);
    if (rc != CAI_OK || index >= pending.count)
      break;
    rc = cai_input_queue_entry_at(state->input_admin, index, &entry, error);
    if (rc != CAI_OK)
      break;
    if (!entry.ready) {
      ++index;
      continue;
    }
    rc = cai_input_queue_claim(state->input, entry.id, entry.revision, &claim,
                               error);
    if (rc == CAI_ERR_INVALID) {
      cai_error_cleanup(error);
      rc = CAI_OK;
      ++index;
      continue;
    }
    if (rc != CAI_OK)
      break;
    ++applied;
    rc = cai_cli_messages_consumed(state->messages, claim.entry.id, error);
    if (rc == CAI_OK && claim.entry.action) {
      int handled;
      rc = cli_dispatch_command(state, claim.text, &claim, error, &handled);
    } else if (rc == CAI_OK &&
               (!state->was_busy || entry.intent == CAI_INPUT_QUEUED))
      state->awaiting_turn = 1;
    cai_input_claim_cleanup(&claim);
    if (rc != CAI_OK) {
      cli_print_error(state, "input", error);
      if (state->documents.error.code != CAI_OK || rc == CAI_ERR_NOMEM ||
          state->runtime == NULL || state->exit_requested)
        return rc;
      cai_error_cleanup(error);
      rc = CAI_OK;
    }
    if (state->exit_requested)
      return CAI_OK;
    index = 0U;
    rc = cli_sync_queue(state, at_boundary, error);
  }
  return rc == CAI_OK ? cli_sync_queue(state, 0, error) : rc;
}
/** Replay diagnostics share the restoration pass; no renderer or second
 * journal reader is involved. Preserve source tool/child provenance in logs. */
static int cli_log_replay(cli_state *state,
                          const cai_agent_session_event *source,
                          cai_error *error) {
  cai_agent_session_event projected;
  const cai_agent_session_event *event = &projected;
  int rc = cai_agent_session_event_project_input(source, &projected, error);
  if (rc != CAI_OK)
    return rc;
  if (strcmp(event->type, "subagent") == 0) {
    cai_execution_record record;
    cai_error ignored;
    cai_execution_record_init(&record);
    cai_error_init(&ignored);
    if (cai_execution_record_parse(event->data, &record, &ignored) == CAI_OK &&
        (strcmp(record.kind, "assistant_text_delta") == 0 ||
         strcmp(record.kind, "reasoning_summary") == 0)) {
      pslog_field fields[6];
      fields[0] = pslog_bool("replay", 1);
      fields[1] =
          pslog_str("subagent", record.subagent != NULL ? record.subagent : "");
      fields[2] = pslog_str("session_id", record.session_id);
      fields[3] = pslog_str(
          "parent_session_id",
          record.parent_session_id != NULL ? record.parent_session_id : "");
      fields[4] =
          pslog_str("parent_tool_call_id",
                    record.parent_call_id != NULL ? record.parent_call_id : "");
      fields[5] = pslog_u64("sequence", record.source_sequence);
      cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO, record.kind,
                          "assistant", record.data, strlen(record.data), fields,
                          6U);
    }
    cai_execution_record_cleanup(&record);
    cai_error_cleanup(&ignored);
  }
  if (event->data != NULL &&
      (strcmp(event->type, "goal_continuation") == 0 ||
       strcmp(event->type, "policy_diagnostic") == 0 ||
       strcmp(event->type, "subagent_input") == 0 ||
       strcmp(event->type, "turn_submitted") == 0 ||
       strcmp(event->type, "turn_queued") == 0 ||
       strcmp(event->type, "steering_queued") == 0 ||
       strcmp(event->type, "reasoning_summary") == 0 ||
       strcmp(event->type, "assistant_text_delta") == 0)) {
    pslog_field fields[2];
    int assistant = strcmp(event->type, "assistant_text_delta") == 0 ||
                    strcmp(event->type, "reasoning_summary") == 0;
    fields[0] = pslog_bool("replay", 1);
    fields[1] = pslog_str(
        "prompt_kind",
        strcmp(event->type, "policy_diagnostic") == 0   ? "policy_diagnostic"
        : strcmp(event->type, "goal_continuation") == 0 ? "goal_continuation"
        : strcmp(event->type, "steering_queued") == 0 ||
                strcmp(event->type, "subagent_input") == 0
            ? "steering"
        : strcmp(event->type, "turn_queued") == 0 ? "queued"
                                                  : "normal");
    cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO, event->type,
                        assistant ? "assistant" : "user", event->data,
                        strlen(event->data), fields, 2U);
  }
  return CAI_OK;
}

/** Restoration invokes the host observer once. Console replay belongs solely
 * to an attachment's retained journal reader, never to the runtime owner. */
static int cli_replay_event(void *context, const cai_agent_session_event *event,
                            cai_error *error) {
  cli_state *state = context;
  if (cai_app_bridge_stopping(state->bridge))
    return cai_cli_error(error, CAI_ERR_CANCELLED, "startup was stopped");
  if (state->application != NULL &&
      state->application->replay_callback != NULL) {
    int rc = state->application->replay_callback(
        state->application->replay_context, event, error);
    if (rc != CAI_OK) {
      if (error->code == CAI_OK)
        cai_cli_error(error, rc, "application replay observer failed");
      cai_app_bridge_fail(state->bridge, error);
      return rc;
    }
  }
  return cli_log_replay(state, event, error);
}

static int cli_sandbox_notice(cli_state *state) {
  if (state->sandbox_notice_emitted)
    return 0;
  state->sandbox_notice_emitted = 1;
  cli_diagnostic(
      state, PSLOG_LEVEL_WARN,
      state->options.yolo
          ? "YOLO enabled: local sandbox restrictions are disabled; commands "
            "inherit the full environment and run with your user permissions."
          : "Sandbox restrictions enabled: local file tools are "
            "workspace-scoped and command environments are filtered. HOME and "
            "XDG_* are preserved. Managed terminal commands are not "
            "OS-sandboxed. Use --yolo to disable these restrictions.");
  return 0;
}

static int cli_prompt_configure(cli_state *state, cai_error *error) {
  int limit = state->options.prompt_history_limit;
  int rc = cai_cli_pouch_prompt_configure(
      &state->pouch, cai_agent_runtime_session_id(state->runtime),
      (unsigned int)limit, error);
  if (rc != CAI_OK)
    return rc;
  cai_cli_workspace_host_prompt_limit(state->workspace_host,
                                      (unsigned int)limit);
  return CAI_OK;
}

static int cli_replay_begin(void *context, const char *id, cai_error *error) {
  cli_state *state = context;
  int rc = cli_session_log(state, id, error);
  return rc;
}

static int cli_register_agent_tools(void *context, cai_agent *agent,
                                    cai_error *error) {
  cli_state *state = context;
  int rc = CAI_OK;
  if (state->messages != NULL)
    rc = cai_cli_messages_register(state->messages, agent, error);
  if (rc == CAI_OK && state->application != NULL &&
      state->application->register_agent_tools != NULL)
    rc = state->application->register_agent_tools(
        state->application->register_agent_tools_context, agent, error);
  return rc;
}

/* Handles belong to the app run, not a turn or reopened runtime. */
static cai_cli_pouch *cli_named_store(cli_state *state, const char *name,
                                      int authentication, cai_error *error) {
  cai_cli_options selected;
  cai_cli_pouch *store;
  /* Reject an empty/invalid selector before any cached-handle comparisons. */
  if (cai_cli_store_options(state->configuration, &state->normal_options, name,
                            &selected, error) != CAI_OK)
    return NULL;
  if (!state->options.temporary &&
      strcmp(name, state->normal_options.sessions_store) == 0)
    return &state->pouch;
  if (state->auth_pouch.sessions != NULL &&
      strcmp(name, state->normal_options.auth_store) == 0)
    return &state->auth_pouch;
  if (state->messaging_pouch.sessions != NULL &&
      strcmp(name, state->normal_options.messaging_store) == 0)
    return &state->messaging_pouch;
  store = authentication ? &state->auth_pouch : &state->messaging_pouch;
  if (cai_cli_pouch_open_options(store, &selected, cli_logger(state), error) !=
      CAI_OK)
    return NULL;
  return store;
}

static int cli_open_plugins(cli_state *state, cai_error *error) {
  cai_plugins_config config;
  const char *roots[2];
  char global[PATH_MAX], local[PATH_MAX];
  int rc;
  if (!state->options.plugins || state->options.review ||
      state->plugins != NULL)
    return CAI_OK;
  if (state->application != NULL && state->application->plugins != NULL) {
    state->plugins = state->application->plugins;
    return CAI_OK;
  }
  if (snprintf(global, sizeof(global), "%s/tools",
               state->normal_options.data_dir) >= (int)sizeof(global) ||
      snprintf(local, sizeof(local), "%s/.%s/tools", state->workspace,
               state->options.program_name) >= (int)sizeof(local))
    return cai_cli_error(error, CAI_ERR_INVALID, "tool root path too long");
  roots[0] =
      state->options.tools_dir != NULL ? state->options.tools_dir : global;
  roots[1] = state->options.workspace_tools_dir != NULL
                 ? state->options.workspace_tools_dir
                 : local;
  cai_plugins_config_init(&config);
  config.directories = roots;
  config.directory_count = 2U;
  rc = cai_plugins_open(&config, &state->plugins, error);
  if (rc != CAI_OK)
    return rc;
  state->owns_plugins = 1;
#ifdef CAI_APP_LUA_TOOLS
  {
    cai_lua_tools_config adapter;
    cai_lua_tools_config_init(&adapter);
    rc = cai_lua_tools_open(&adapter, &state->lua_tools, error);
  }
  if (rc == CAI_OK)
    rc = cai_lua_tools_attach(state->lua_tools, state->plugins, error);
#endif
  if (rc == CAI_OK)
    rc = cai_plugins_reload(state->plugins, error);
  return rc;
}

static int cli_open_runtime(cli_state *state, const char *resume_id,
                            int new_session, cai_error *error) {
  cai_agent_runtime_config config;
  cai_agent_preset preset;
  cai_skill_config skills;
  cai_cli_instruction_files instruction_files;
  cai_terminal_tool_config terminal;
  char selected_id[CAI_AGENT_SESSION_ID_MAX];
  int rc;

  if (resume_id != NULL) {
    rc = cai_cli_session_select(&state->pouch, state->workspace, resume_id,
                                selected_id, error);
    if (rc != CAI_OK)
      return rc;
    resume_id = selected_id;
  }

  if (!state->suppress_replay)
    state->has_review_metrics = 0;
  cai_agent_runtime_config_init(&config);
  if (state->configuration != NULL &&
      state->configuration->apply_runtime(state->configuration, &config,
                                          error) != CAI_OK)
    return error->code;
  memset(&terminal, 0, sizeof(terminal));
  if (state->configuration != NULL &&
      state->configuration->apply_terminal(state->configuration, &terminal,
                                           error) != CAI_OK)
    return error->code;
  config.terminal_tool_config = &terminal;
  if (state->options.temporary) {
    terminal.root_path = state->workspace;
    terminal.default_workdir = state->workspace;
    config.searxng.response_spool_dir = state->temporary.private_dir;
    config.revgeo.response_spool_dir = state->temporary.private_dir;
  }
  cai_cli_apply_runtime_options(&state->options, &config, &preset, &skills);
  config.workspace_approval_directory =
      state->workspace_approval_root[0] != '\0' ? state->workspace_approval_root
                                                : NULL;
  config.record_workspace_approval = state->workspace_approval_pending;
  rc = cli_open_plugins(state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->options.plugins && !state->options.review)
    config.plugins = state->plugins;
  terminal.default_yield_time_ms = state->options.yield_time_ms;
  terminal.foreground = state->options.foreground;
  if (state->suppress_replay) {
    /* A live catalog change must restore the current commit settings,
     * rather than reapply the invocation's original explicit overrides. */
    config.model = NULL;
    config.reasoning_effort = NULL;
    config.reasoning_summary = NULL;
    config.service_tier = NULL;
  }
  if (state->options.messaging && !state->options.review) {
    cai_cli_pouch *messaging =
        cli_named_store(state, state->normal_options.messaging_store, 0, error);
    if (messaging == NULL)
      return error->code;
    rc = cai_cli_messages_new(messaging, state->options.message_ack_timeout_ms,
                              &state->messages, error);
    if (rc != CAI_OK)
      return rc;
  }
  if (state->messages != NULL ||
      (state->application != NULL &&
       state->application->register_agent_tools != NULL)) {
    config.register_agent_tools = cli_register_agent_tools;
    config.register_agent_tools_context = state;
  }
  if (config.enable_todo) {
    config.todo.store = cai_cli_pouch_todo_callbacks();
    config.todo.store_context = &state->pouch;
  }
  config.logger = cli_logger(state);
  config.workspace_directory = state->workspace;
  config.image_directory =
      state->clipboard_directory[0] != '\0' ? state->clipboard_directory : NULL;
  config.session_store = &state->store;
  config.resume_latest =
      !state->options.review && !new_session && resume_id == NULL;
  config.resume_session_id = state->options.review ? NULL : resume_id;
  config.record_transcript = 1;
  config.event_callback = cli_event;
  config.review_event_callback = cli_review_event;
  config.review_event_context = state;
  config.synchronize_input_boundaries = 1;
  config.event_context = state;
  if (!state->options.review && !state->suppress_replay) {
    state->pouch.replay_begin = cli_replay_begin;
    config.replay_callback = cli_replay_event;
    config.replay_context = state;
    state->pouch.replay_context = state;
    state->pouch.replay_scope = state->workspace;
  }
  if (!state->mcp_initialized) {
    rc = cai_cli_mcp_open(&state->mcp, state->configuration,
                          state->options.program_name,
                          state->options.mcp && !state->options.review,
                          state->auth != NULL ? &state->shared_auth : NULL,
                          cli_logger(state), error);
    if (rc != CAI_OK)
      return rc;
    state->mcp_initialized = 1;
  }
  config.mcp_clients = state->mcp.clients;
  config.mcp_options = state->mcp.options;
  config.mcp_client_count =
      state->options.review ? 0U : state->mcp.client_count;
  cai_cli_pouch_begin_use(&state->pouch);
  rc = cai_cli_instruction_files_open(&state->options, &instruction_files,
                                      error);
  preset.developer_instructions = instruction_files.agent;
  preset.review_developer_instructions = instruction_files.reviewer;
  preset.developer_instructions_literal = instruction_files.agent != NULL;
  preset.review_developer_instructions_literal =
      instruction_files.reviewer != NULL;
  config.developer_instructions_override = instruction_files.initial;
  if (rc == CAI_OK && state->normal_options.review &&
      instruction_files.initial != NULL) {
    state->initial_review_instructions = instruction_files.initial;
    instruction_files.initial = NULL;
    config.developer_instructions_override = NULL;
    state->options.developer_instructions_override_file = NULL;
  }
  if (rc == CAI_OK)
    rc = cai_agent_runtime_open(state->client, &config, &state->runtime, error);
  cai_cli_instruction_files_close(&instruction_files);
  if (rc == CAI_OK)
    rc = cai_cli_documents_bind(&state->documents, state->runtime, error);
  if (rc == CAI_OK && !state->options.review) {
    cai_cli_mcp_start(&state->mcp);
    if (!cai_cli_mcp_ready(&state->mcp))
      rc = cai_agent_runtime_refresh_mcp(state->runtime, error);
  }
  state->pouch.capture_session = 0;
  state->pouch.replay_begin = NULL;
  state->pouch.replay_event = NULL;
  state->pouch.replay_context = NULL;
  state->pouch.replay_scope = NULL;
  if (rc != CAI_OK)
    return rc;
  {
    rc = cai_agent_runtime_pending_input(state->runtime, &state->input, error);
    if (rc == CAI_OK)
      rc = cai_input_queue_set_policy(state->input, cli_prompt_classify, state,
                                      error);
    if (rc == CAI_OK)
      rc =
          cai_input_queue_client_open(state->input, &state->input_admin, error);
    state->input_request = 0U;
    if (rc == CAI_OK)
      rc = cai_input_controller_select(state->input_controller, state->input,
                                       error);
    if (rc != CAI_OK)
      return rc;
    {
      cai_input_state pending;
      rc = cai_input_queue_get_state(state->input, &pending, error);
      if (rc == CAI_OK &&
          pending.auto_approval != state->options.enable_auto_enqueue)
        rc = cli_input_request(
            state, CAI_INPUT_AUTO_APPROVAL, NULL, CAI_INPUT_QUEUED,
            state->options.enable_auto_enqueue, NULL, NULL, error);
    }
    if (rc == CAI_OK)
      rc = cai_input_controller_pump(state->input_controller, 32U, error);
    if (rc != CAI_OK)
      return rc;
  }
  /* The runtime has durably recorded approval before its directory lookup is
   * published. A bypass must never become a saved authorization. */
  if (state->workspace_approval_pending) {
    rc = cai_cli_pouch_approve_workspace(
        &state->pouch, state->workspace,
        cai_agent_runtime_session_id(state->runtime), error);
    if (rc != CAI_OK)
      return rc;
    state->workspace_approval_pending = 0;
  }
  if (state->options.board_explicit && state->options.todo) {
    rc = cai_agent_runtime_select_board(state->runtime, state->options.board, 1,
                                        error);
    if (rc != CAI_OK)
      return rc;
    state->options.board_explicit = 0;
  }
  if (state->messages != NULL) {
    rc = cai_cli_messages_start(
        state->messages, cai_agent_runtime_session_id(state->runtime),
        state->workspace, state->options.message_queue, error);
    if (rc != CAI_OK)
      return rc;
  }
  rc = cli_session_log(state, cai_agent_runtime_session_id(state->runtime),
                       error);
  if (rc == CAI_OK)
    rc = cli_prompt_configure(state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->options.developer_instructions != NULL) {
    pslog_field kind = pslog_str("prompt_kind", "developer_instructions");
    cai_cli_log_message(
        cli_logger(state), PSLOG_LEVEL_INFO, "developer_instructions",
        "developer", state->options.developer_instructions,
        strlen(state->options.developer_instructions), &kind, 1U);
  }
  rc = CAI_OK;
  if (rc == CAI_OK)
    rc = cai_cli_workspace_host_input(
        state->workspace_host, state->input_controller,
        cai_agent_runtime_session_id(state->runtime), error);
  if (rc == CAI_OK)
    rc = cai_cli_workspace_host_history(state->workspace_host, &state->pouch,
                                        error);
  if (rc == CAI_OK)
    rc = cai_cli_workspace_host_catalog(state->workspace_host, state->runtime,
                                        error);
  if (rc == CAI_OK)
    cai_cli_workspace_host_cancel(state->workspace_host, cli_remote_cancel,
                                  state);
  if (rc == CAI_OK)
    cai_cli_workspace_host_selection(state->workspace_host, cli_select_startup,
                                     state);
  if (rc == CAI_OK)
    rc = cai_cli_workspace_host_ready(state->workspace_host, error);
  if (rc == CAI_OK) {
    cai_app_bridge_bind_input(state->bridge, state->input_admin != NULL
                                                 ? state->input_controller
                                                 : NULL);
    rc = cli_sync_status(state, error);
  }
  return rc;
}

static int cli_prepare_clipboard(cli_state *state, cai_error *error) {
  const char *path = state->options.clipboard_dir;
  int rc = cai_cli_private_directory(path, error);
  if (rc != CAI_OK)
    return rc;
  if (realpath(path, state->clipboard_directory) == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "cannot resolve application clipboard directory");
  return CAI_OK;
}

static int cli_close_runtime(cli_state *state, cai_error *error) {
  int rc, output = CAI_OK;
  output = cai_cli_documents_bind(&state->documents, NULL, error);
  cai_app_bridge_bind_input(state->bridge, NULL);
  rc = cai_cli_workspace_host_input(state->workspace_host, NULL, NULL, error);
  if (rc != CAI_OK)
    state->exit_requested = 1;
  if (state->input_controller != NULL)
    (void)cai_input_controller_select(state->input_controller, NULL, NULL);
  cai_input_queue_client_close(state->input_admin);
  state->input_admin = NULL;
  state->input = NULL;
  state->settings_notice_fields = 0U;
  if (state->review_runtime != NULL) {
    /* Abandoning the UI preserves the parent's durable review-pending marker
     * for recovery, just as an interrupted reviewer does. Close the owned
     * child before its parent; never start queued work during shutdown. */
    (void)cai_agent_runtime_cancel_turn(state->review_runtime, NULL);
    cai_agent_runtime_close(state->review_runtime);
    state->review_runtime = NULL;
  }
  state->review_call[0] = '\0';
  (void)cli_publish_status(state, NULL, NULL);
  cai_cli_messages_stop(state->messages);
  cai_agent_runtime_close(state->runtime);
  state->runtime = NULL;
  state->review_block = 0;
  cai_cli_messages_close(state->messages);
  state->messages = NULL;
  /* Close the conversation-owned recall collection before releasing its
   * session use guard. Frontends persist entries through the host channel. */
  cai_cli_pouch_prompt_close(&state->pouch);
  cai_cli_pouch_end_use(&state->pouch);
  state->goal_starts_pending = 0U;
  return output != CAI_OK ? output : rc;
}

static int cli_list_write(void *context, const void *bytes, size_t count,
                          cai_error *error) {
  cli_state *state = context;
  const char *text = bytes;
  size_t start = 0U, i;
  /* Indented Markdown code keeps streamed rows literal even when previews or
   * filenames contain backticks, links or Markdown fence terminators. */
  for (i = 0U; i < count; i++) {
    if (state->list_line_start) {
      if (cli_text(state, "    ", 4U) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "render command rows");
      state->list_line_start = 0;
    }
    if (text[i] == '\n') {
      if (cli_text(state, text + start, i - start + 1U) != 0)
        return cai_cli_error(error, CAI_ERR_TRANSPORT, "render command rows");
      start = i + 1U;
      state->list_line_start = 1;
    }
  }
  if (start < count && cli_text(state, text + start, count - start) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "render command rows");
  return CAI_OK;
}

/** Marshal a worker outcome back onto the original command's journal region.
 * Ordinary export failures are inline results; only persistence/output failure
 * can fail the owner. No runtime or presentation operation runs on the worker.
 */
static int cli_export_result(cli_state *state, cai_cli_export_result *result,
                             cai_error *error) {
  int outcome, written, rc;
  if (result == NULL)
    return CAI_OK;
  rc = cai_cli_documents_resume(&state->documents, result->call, error);
  if (rc != CAI_OK) {
    cai_cli_export_result_close(result);
    return rc;
  }
  outcome = result->status;
  if (outcome == CAI_OK) {
    state->list_line_start = 1;
    if (cli_text(state, "## Conversation export\n\n", 24U) != 0)
      outcome = CAI_ERR_TRANSPORT;
    if (outcome == CAI_OK)
      outcome =
          cli_list_write(state, result->paths, result->length, &result->error);
  }
  written =
      cai_cli_documents_complete(&state->documents, outcome, &result->error);
  rc = state->documents.error.code;
  if (rc == CAI_OK && written != outcome)
    rc = written;
  if (rc != CAI_OK)
    cai_cli_error(error, rc, "persist export completion");
  cai_cli_export_result_close(result);
  state->settlement_pending = 1;
  return rc;
}
static int cli_export_pump(cli_state *state, cai_error *error) {
  return cli_export_result(state, cai_cli_export_job_take(state->export_job),
                           error);
}

static int cli_load_sessions(cli_state *state, cai_error *error) {
  free(state->sessions);
  state->sessions = NULL;
  state->session_count = 0U;
  return cai_cli_pouch_list(&state->pouch, state->workspace, &state->sessions,
                            &state->session_count, error);
}

static int cli_list_sessions(cli_state *state, cai_error *error) {
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  int rc = cli_load_sessions(state, error);
  if (rc != CAI_OK)
    return rc;
  state->list_line_start = 1;
  if (cli_text(state, "## Conversations\n\n", 18U) != 0)
    return CAI_ERR_TRANSPORT;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = state;
  callbacks.write = cli_list_write;
  sink = NULL;
  rc = cai_sink_from_callbacks(&callbacks, &sink, error);
  if (rc == CAI_OK)
    rc = cai_cli_sessions_print(sink, state->sessions, state->session_count, 1,
                                78, error);
  cai_sink_close(sink);
  if (cli_finish_response(state) != 0 && rc == CAI_OK)
    rc = CAI_ERR_TRANSPORT;
  return rc;
}

/* Use a code-span delimiter longer than any run in a path or queue name.
 * Padding protects embedded backticks and leading/trailing spaces. */
static int cli_code_span(cli_state *state, const char *value) {
  const char *p;
  char *fence;
  size_t run = 0U, longest = 0U;
  int rc;
  for (p = value; *p != '\0'; p++) {
    run = *p == '`' ? run + 1U : 0U;
    if (run > longest)
      longest = run;
  }
  fence = malloc(longest + 2U);
  if (fence == NULL)
    return -1;
  memset(fence, '`', longest + 1U);
  fence[longest + 1U] = '\0';
  rc = cli_text(state, fence, longest + 1U) != 0 ||
               cli_text(state, " ", 1U) != 0 ||
               cli_text(state, value, strlen(value)) != 0 ||
               cli_text(state, " ", 1U) != 0 ||
               cli_text(state, fence, longest + 1U) != 0
           ? -1
           : 0;
  free(fence);
  return rc;
}

static int cli_settings_changed(cli_state *state,
                                const cai_agent_runtime_settings *settings,
                                unsigned int fields) {
  int model = (fields & CAI_AGENT_RUNTIME_SETTING_MODEL) != 0U &&
              settings->model != NULL;
  int effort = (fields & CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT) != 0U &&
               settings->reasoning_effort != NULL;
  if (cli_finish_response(state) != 0)
    return CAI_ERR_TRANSPORT;
  if (model || effort) {
    const char *prefix =
        model ? "Model changed to " : "Reasoning effort changed to ";
    if (cli_text(state, prefix, strlen(prefix)) != 0 ||
        cli_code_span(state, model ? settings->model
                                   : settings->reasoning_effort) != 0 ||
        (model && effort &&
         (cli_text(state, "; reasoning effort ", 19U) != 0 ||
          cli_code_span(state, settings->reasoning_effort) != 0)) ||
        cli_text(state, ".\n", 2U) != 0 || cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
  }
  if ((fields & CAI_AGENT_RUNTIME_SETTING_SERVICE_TIER) != 0U &&
      settings->service_tier != NULL) {
    if (cli_text(state, "Service tier selected: ", 23U) != 0 ||
        cli_code_span(state, settings->service_tier) != 0 ||
        cli_text(state, ".\n", 2U) != 0 || cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
  }
  if ((fields & CAI_AGENT_RUNTIME_SETTING_REASONING_SUMMARY) != 0U &&
      settings->reasoning_summary != NULL) {
    static const char prefix[] = "Reasoning summary changed to ";
    if (cli_text(state, prefix, sizeof(prefix) - 1U) != 0 ||
        cli_code_span(state, settings->reasoning_summary) != 0 ||
        cli_text(state, ".\n", 2U) != 0 || cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
  }
  return CAI_OK;
}

/* Only confirm real effective values, after the worker has applied the update.
 * Native runtime events drive this check; there is no polling or UI work queue.
 * Several commands delivered at one boundary can settle as one final update. */
static int cli_settings_notice(cli_state *state, cai_error *error) {
  cai_agent_runtime_settings effective, pending;
  cai_error local;
  unsigned int fields;
  int rc;
  if (state->settings_notice_fields == 0U || state->runtime == NULL ||
      state->exit_requested)
    return CAI_OK;
  cai_error_init(&local);
  rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                      &local);
  cai_error_cleanup(&local);
  if (rc == CAI_ERR_INVALID)
    return CAI_OK; /* A transient application/switch is still in progress. */
  if (rc != CAI_OK)
    return cai_cli_error(error, rc, "read applied runtime settings");
  if ((pending.present & state->settings_notice_fields) != 0U)
    return CAI_OK;
  fields = state->settings_notice_fields;
  state->settings_notice_fields = 0U;
  return cli_settings_changed(state, &effective, fields);
}

static int cli_session_field(cli_state *state, const char *label,
                             const char *value) {
  return cli_text(state, "- **", 4U) != 0 ||
                 cli_text(state, label, strlen(label)) != 0 ||
                 cli_text(state, ":** ", 4U) != 0 ||
                 cli_code_span(state, value) != 0 ||
                 cli_text(state, "\n", 1U) != 0
             ? -1
             : 0;
}

static int cli_set_messaging(cli_state *state, int enabled, cai_error *error) {
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  cai_input_entry info;
  cai_input_state pending;
  size_t i;
  char *id;
  int rc;
  if (enabled == state->options.messaging)
    return CAI_OK;
  rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (metrics.turn_active || state->awaiting_turn || state->shell.pid > 0 ||
      state->review_runtime != NULL || run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Change messaging between turns.");
  /* Reopening must not replace an edited, unconsumed incoming prompt with
   * its original durable delivery. Leave approval and editing under user
   * control before removing this runtime's messaging integration. */
  if (!enabled && state->input != NULL) {
    rc = cai_input_queue_get_state(state->input, &pending, error);
    for (i = 0U; rc == CAI_OK && i < pending.count; ++i) {
      rc = cai_input_queue_entry_at(state->input_admin, i, &info, error);
      if (rc == CAI_OK && cai_cli_messages_is_message(state->messages, info.id))
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "Approve or reject queued incoming messages "
                             "before disabling messaging.");
    }
    if (rc != CAI_OK)
      return rc;
  }
  id = strdup(cai_agent_runtime_session_id(state->runtime));
  if (id == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "copy messaging session ID");
  if (cli_finish_response(state) != 0) {
    free(id);
    return CAI_ERR_TRANSPORT;
  }
  /* Tool registration is immutable while a runtime exists. Reopen this same
   * durable session with the updated catalog, without replaying it to the UI
   * or host observers. No model request or new conversation is created. */
  rc = cai_cli_messages_flush_decisions(state->messages, state->input, error);
  if (rc != CAI_OK) {
    free(id);
    return rc;
  }
  state->suppress_replay = 1;
  rc = cli_close_runtime(state, error);
  if (rc != CAI_OK) {
    state->suppress_replay = 0;
    free(id);
    return rc;
  }
  state->options.messaging = enabled;
  rc = cli_open_runtime(state, id, 0, error);
  if (rc != CAI_OK) {
    cai_error recovery;
    cai_error_init(&recovery);
    if (cli_close_runtime(state, &recovery) != CAI_OK) {
      cai_error_cleanup(&recovery);
      free(id);
      state->suppress_replay = 0;
      return rc;
    }
    state->options.messaging = !enabled;
    if (cli_open_runtime(state, id, 0, &recovery) != CAI_OK) {
      state->exit_requested = 1;
      cli_print_error(state, "restore conversation after messaging change",
                      &recovery);
    }
    cai_error_cleanup(&recovery);
  }
  state->suppress_replay = 0;
  free(id);
  return rc;
}

static int cli_switch_session(cli_state *state, const char *id, int new_session,
                              cai_error *error) {
  cai_agent_run_state run_state;
  const char *created_id;
  int rc;
  /* Selecting the current context must not release its lease, disconnect its
   * producers or reopen SDK services merely to select that same context. */
  if (!new_session && id != NULL &&
      strcmp(id, cai_agent_runtime_session_id(state->runtime)) == 0)
    return cli_markdown(state, "Conversation already active.\n") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (state->shell.pid > 0 || state->awaiting_turn ||
      state->review_runtime != NULL ||
      cai_cli_export_job_active(state->export_job) ||
      run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL) {
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Wait for the current turn to finish.");
  }
  if (cli_finish_response(state) != 0)
    return CAI_ERR_TRANSPORT;
  rc = cai_cli_messages_flush_decisions(state->messages, state->input, error);
  if (rc != CAI_OK)
    return rc;
  rc = cli_close_runtime(state, error);
  if (rc != CAI_OK)
    return rc;
  state->awaiting_turn = 0;
  state->automation_done = 1;
  state->automation_failed = 0;
  /* -z selects only the startup parent, never a subsequently selected agent. */
  state->options.developer_instructions_override_file = NULL;
  free(state->initial_review_instructions);
  state->initial_review_instructions = NULL;
  rc = cli_open_runtime(state, id, new_session, error);
  if (rc != CAI_OK)
    return rc;
  if (!new_session)
    return cli_advance_automation(state, error);
  created_id = cai_agent_runtime_session_id(state->runtime);
  if (created_id == NULL)
    return cai_cli_error(error, CAI_ERR_PROTOCOL, "new session ID unavailable");
  if (cli_text(state, "## New session\n\n", 16U) != 0 ||
      cli_session_field(state, "ID", created_id) != 0 ||
      cli_session_field(state, "Workspace", state->workspace) != 0 ||
      (state->messages != NULL &&
       cli_session_field(state, "Message queue",
                         cai_cli_messages_name(state->messages)) != 0) ||
      cli_finish_response(state) != 0)
    return CAI_ERR_TRANSPORT;
  return CAI_OK;
}

/** Validate a destination before releasing the current context. Exact IDs
 * resolve syntactically first; their existence/workspace still need checking.
 * Keep the multi-KiB metadata off the owner's default worker stack. */
static int cli_session_destination(cli_state *state, const char *selector,
                                   char id[CAI_AGENT_SESSION_ID_MAX],
                                   cai_error *error) {
  cai_cli_session *destination;
  int rc = cai_cli_session_select(&state->pouch, state->workspace, selector, id,
                                  error);
  if (rc != CAI_OK ||
      strcmp(id, cai_agent_runtime_session_id(state->runtime)) == 0)
    return rc;
  destination = malloc(sizeof(*destination));
  if (destination == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "validate conversation selection");
  rc = cai_cli_pouch_session(&state->pouch, id, destination, error);
  if (rc == CAI_OK && strcmp(destination->workspace, state->workspace) != 0)
    rc = cai_cli_error(error, CAI_ERR_INVALID,
                       "session belongs to another directory");
  free(destination);
  return rc;
}
/** Resolve selectors before closing the current runtime. Startup selection is
 * connection setup, so selecting the current conversation emits no command
 * receipt or transcript text and does not reopen its store/context. */
static int cli_select_startup(void *context, const char *selector,
                              cai_error *error) {
  cli_state *state = context;
  char id[CAI_AGENT_SESSION_ID_MAX];
  int rc;
  if (selector == NULL)
    return cli_switch_session(state, NULL, 1, error);
  rc = cli_session_destination(state, selector, id, error);
  if (rc == CAI_OK &&
      strcmp(id, cai_agent_runtime_session_id(state->runtime)) != 0)
    rc = cli_switch_session(state, id, 0, error);
  return rc;
}
static int cli_reasoning_tool_command(cli_state *state, const char *arg,
                                      cai_error *error) {
  cai_reasoning_tool_config config;
  char buffer[256], message[512], *action, *level, *extra, *save = NULL;
  int rc = cai_agent_runtime_get_reasoning_tool(state->runtime, &config, error);
  if (rc != CAI_OK)
    return rc;
  if (strlen(arg) >= sizeof(buffer))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Reasoning tool command too long.");
  strcpy(buffer, arg);
  action = strtok_r(buffer, " \t\r\n", &save);
  level = strtok_r(NULL, " \t\r\n", &save);
  extra = strtok_r(NULL, " \t\r\n", &save);
  if (action != NULL) {
    char *normal, *high;
    if (extra != NULL)
      goto usage;
    if (strcmp(action, "on") == 0 && level == NULL)
      config.enabled = 1;
    else if (strcmp(action, "off") == 0 && level == NULL)
      config.enabled = 0;
    else if (strcmp(action, "normal") == 0 && level != NULL)
      config.normal_effort = level;
    else if (strcmp(action, "high") == 0 && level != NULL)
      config.high_effort = level;
    else
      goto usage;
    normal = strdup(config.normal_effort);
    high = strdup(config.high_effort);
    if (normal == NULL || high == NULL) {
      free(normal);
      free(high);
      return cai_cli_error(error, CAI_ERR_NOMEM, "copy reasoning mappings");
    }
    rc = cai_agent_runtime_set_reasoning_tool(state->runtime, &config, error);
    if (rc != CAI_OK) {
      free(normal);
      free(high);
      return rc;
    }
    free(state->normal_reasoning_override);
    free(state->high_reasoning_override);
    state->normal_reasoning_override = normal;
    state->high_reasoning_override = high;
    state->options.normal_reasoning_effort = normal;
    state->options.high_reasoning_effort = high;
    state->options.reasoning_tool = config.enabled;
    rc = cai_agent_runtime_get_reasoning_tool(state->runtime, &config, error);
    if (rc != CAI_OK)
      return rc;
  }
  snprintf(message, sizeof(message),
           "## Reasoning tool\n\n| Setting | Value |\n| --- | --- |\n"
           "| Enabled | %s |\n| Normal effort | %s |\n| High effort | %s |\n\n"
           "Changes apply to this app; config.ini is unchanged. Unsupported or "
           "unknown choices leave "
           "the current effort unchanged.\n",
           config.enabled ? "on" : "off", config.normal_effort,
           config.high_effort);
  return cli_text(state, message, strlen(message)) == 0 &&
                 cli_finish_response(state) == 0
             ? CAI_OK
             : CAI_ERR_TRANSPORT;
usage:
  return cai_cli_error(error, CAI_ERR_INVALID,
                       "Use /reasoning-tool [on|off|normal LEVEL|high LEVEL]. "
                       "Configure between turns.");
}

static int cli_goal_command(cli_state *state, const char *arg,
                            cai_error *error) {
  static const char usage[] =
      "Use /goal [status|help|pause|resume|clear] or /goal new TEXT. "
      "Prefer asking the agent to create and manage goals.";
  cai_agent_goal_snapshot goal;
  cai_agent_run_state run_state;
  pslog_field fields[2];
  char metrics[256];
  const char *objective = NULL;
  const char *initial;
  const char *action;
  int rc, starts_goal = 0;

  arg += strspn(arg, " \t\r\n");
  initial = cai_cli_goal_input_argument(arg);
  if (initial != NULL) {
    cai_cli_goal_input input;
    if (!state->options.goals)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "Goal pursuit requires goal tools; enable --goals and restart.");
    rc = cai_cli_goal_input_parse(initial, &input, error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_start_goal(
          state->runtime, input.objective, input.prompt,
          state->documents.active ? state->documents.call : NULL, error);
    cai_cli_goal_input_close(&input);
    if (rc == CAI_OK) {
      state->awaiting_turn = 1;
      state->automation_failed = 0;
      state->automation_done = 0;
    }
    return rc;
  }
  if (strcmp(arg, "help") == 0) {
    static const char *const help[] = {
        "## Goal commands\n\n"
        "Prefer asking the agent to create and manage goals. These explicit "
        "controls apply at the next safe model/tool boundary.\n\n"
        "| Command | Action |\n| --- | --- |\n"
        "| `/goal`, `/goal status` | Show the current durable goal. |\n"
        "| `/goal new TEXT` | Start a fresh, unbudgeted goal, replacing the "
        "current goal without confirmation, and pursue it. |\n",
        "| `/goal start {\"objective\":\"...\",\"prompt\":\"...\"}` | Start a "
        "fresh goal and its initial prompt as one queued turn. |\n",
        "| `/goal pause` | Pause pursuit after the current request/tool round. "
        "|\n"
        "| `/goal resume` | Continue any unfinished goal, including an active "
        "goal whose pursuit stopped. |\n"
        "| `/goal clear` | Remove goal tracking; the current user turn "
        "continues. |\n"
        "| `/goal help` | Show this help. |\n"};
    size_t i;
    for (i = 0U; i < sizeof(help) / sizeof(help[0]); i++)
      if (cli_text(state, help[i], strlen(help[i])) != 0)
        return CAI_ERR_TRANSPORT;
    return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (*arg == '\0' || strcmp(arg, "status") == 0) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal)
      return cli_markdown(state,
                          "**No goal.** Use `/goal new TEXT`, or ask the agent "
                          "to create and manage one.\n") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    snprintf(metrics, sizeof(metrics), "## Goal\n\nStatus: **%s**\n\n",
             goal.status);
    if (cli_text(state, metrics, strlen(metrics)) != 0 ||
        cli_text(state, goal.objective, strlen(goal.objective)) != 0)
      return CAI_ERR_TRANSPORT;
    snprintf(metrics, sizeof(metrics),
             "\n\nTokens used: %lld. Active time: %lld seconds.\n\n",
             goal.tokens_used, goal.elapsed_seconds);
    if (cli_text(state, metrics, strlen(metrics)) != 0)
      return CAI_ERR_TRANSPORT;
    if (goal.has_token_budget)
      snprintf(metrics, sizeof(metrics),
               "Token budget: %lld; remaining: %lld.\n", goal.token_budget,
               goal.remaining_tokens);
    else
      snprintf(metrics, sizeof(metrics), "Token budget: unbounded.\n");
    return cli_text(state, metrics, strlen(metrics)) == 0 &&
                   cli_finish_response(state) == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strncmp(arg, "new", 3U) == 0 &&
      (arg[3] == '\0' || strchr(" \t\r\n", arg[3]) != NULL)) {
    objective = arg + 3U;
    objective += strspn(objective, " \t\r\n");
    if (*objective == '\0')
      return cai_cli_error(error, CAI_ERR_INVALID, usage);
    if (!state->options.goals)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Goal pursuit requires goal tools; enable --goals "
                           "or [agent] goals=true and restart.");
    action = "new";
    starts_goal = 1;
    rc = cai_agent_runtime_replace_goal(state->runtime, objective, error);
  } else if (strcmp(arg, "pause") == 0) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal || strcmp(goal.status, "active") != 0)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "only an active goal can pause");
    action = "pause";
    rc = cai_agent_runtime_pause_goal(state->runtime, error);
  } else if (strcmp(arg, "resume") == 0) {
    if (!state->options.goals)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Goal pursuit requires goal tools; enable --goals "
                           "or [agent] goals=true and restart.");
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (!goal.has_goal || strcmp(goal.status, "complete") == 0)
      return cai_cli_error(error, CAI_ERR_INVALID, "goal is not resumable");
    action = "resume";
    starts_goal = 1;
    rc = cai_agent_runtime_resume_goal(state->runtime, error);
  } else if (strcmp(arg, "clear") == 0) {
    action = "clear";
    rc = cai_agent_runtime_clear_goal(state->runtime, error);
  } else {
    return cai_cli_error(error, CAI_ERR_INVALID, usage);
  }
  if (rc != CAI_OK)
    return rc;
  if (state->starting_goal)
    state->startup_goal_pending++;
  if (starts_goal)
    state->goal_starts_pending++;
  fields[0] = pslog_str("command", action);
  fields[1] = pslog_str("prompt_kind", "goal_control");
  cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO, "goal_control",
                      "user", arg, strlen(arg), fields, 2U);
  if (cli_markdown(state,
                   "**Goal control queued** for the next safe model/tool "
                   "boundary.\n") != 0)
    return CAI_ERR_TRANSPORT;
  if (!starts_goal) {
    state->automation_done = 1;
    return CAI_OK;
  }
  state->automation_done = 0;
  state->automation_failed = 0;
  /* Settle all startup goal controls before choosing the first prompt. A
   * replacement here must not submit work alongside the startup prompt. */
  if (state->starting_goal)
    return CAI_OK;
  /* Accepted host controls retain FIFO application, but a pending STOP must
   * prevent a goal control from starting inference before that STOP applies. */
  if (cai_app_bridge_stopping(state->bridge))
    return CAI_OK;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL) {
    state->awaiting_turn = 1;
    return CAI_OK;
  }
  return cli_advance_automation(state, error);
}

static int cli_stop(cli_state *state, cai_error *error) {
  int rc = CAI_OK;
  state->host_stopped = state->exit_requested = 1;
  state->automation_done = 1;
  cai_cli_export_job_cancel(state->export_job);
  /* Cancel before acknowledgement. Closing joins workers without draining
   * events that could otherwise start another accepted queued turn. */
  if (state->runtime != NULL) {
    rc = cai_agent_runtime_cancel_turn(state->runtime, error);
    if (rc == CAI_ERR_INVALID) {
      cai_error_cleanup(error);
      cai_error_init(error);
      rc = CAI_OK;
    }
  }
  cai_cli_messages_cancel_send(state->messages);
  if (state->review_runtime != NULL)
    (void)cai_agent_runtime_cancel_turn(state->review_runtime, NULL);
  return rc;
}
static int cli_stop_owned(cli_state *state, cai_error *error) {
  int stopped = state->host_stopped;
  int rc;
  rc = cli_stop(state, error);
  state->host_stopped = stopped;
  return rc;
}
static int cli_host_process(cli_state *state, cai_error *error) {
  cai_app_command *command;
  size_t count;
  int rc;
  if (cai_app_bridge_frontend_ended(state->bridge)) {
    rc = cli_stop_owned(state, error);
    /* Detaching this run's frontend ends its owned controller; it is not an
     * explicit host STOP and must preserve failure/cancellation semantics. */
    if (rc != CAI_OK)
      return rc;
  }
  for (count = 0U;
       count < 64U && (command = cai_app_bridge_take(state->bridge)) != NULL;
       count++) {
    cai_error applied;
    cai_error_init(&applied);
    rc = CAI_OK;
    if (state->exit_requested) {
      rc = cai_cli_error(&applied, CAI_ERR_INVALID, "application is stopping");
    } else if (command->type == CAI_APP_STOP) {
      rc = cli_stop(state, &applied);
    } else if (state->runtime == NULL) {
      rc = cai_cli_error(&applied, CAI_ERR_INVALID,
                         "agent runtime is not ready");
    } else if ((command->type == CAI_APP_QUEUE_PROMPT ||
                command->type == CAI_APP_STEER) &&
               (command->text[0] == '/' || command->text[0] == '!')) {
      rc = cai_cli_error(&applied, CAI_ERR_INVALID,
                         "terminal commands require a frontend attachment");
    } else if (command->type == CAI_APP_QUEUE_PROMPT) {
      if (state->batch_frontend) {
        rc = cai_agent_runtime_submit_queued(state->runtime, command->text,
                                             &applied);
        if (rc == CAI_OK) {
          state->batch_pending_turns++;
          state->automation_done = 0;
        }
      } else {
        rc = cli_input_request(
            state, CAI_INPUT_OFFER, command->text, CAI_INPUT_QUEUED,
            (state->bridge != NULL && state->bridge->frontend_fd >= 0), NULL,
            NULL, &applied);
      }
    } else if (command->type == CAI_APP_STEER) {
      rc = !state->batch_frontend
               ? cli_input_request(state, CAI_INPUT_OFFER, command->text,
                                   CAI_INPUT_STEERING, 0, NULL, NULL, &applied)
               : cai_agent_runtime_submit_steering(state->runtime,
                                                   command->text, &applied);
    } else if (command->type == CAI_APP_CANCEL_TURN) {
      rc = cai_cli_export_job_active(state->export_job)
               ? cli_remote_cancel(state, &applied)
               : cai_agent_runtime_cancel_turn(state->review_runtime != NULL
                                                   ? state->review_runtime
                                                   : state->runtime,
                                               &applied);
      cai_cli_messages_cancel_send(state->messages);
    } else {
      char *goal_command = NULL;
      const char *action = command->type == CAI_APP_PAUSE_GOAL    ? "pause"
                           : command->type == CAI_APP_RESUME_GOAL ? "resume"
                           : command->type == CAI_APP_CLEAR_GOAL  ? "clear"
                                                                  : NULL;
      if (action == NULL) {
        size_t length = strlen(command->text);
        goal_command = malloc(length + 5U);
        if (goal_command == NULL)
          rc = cai_cli_error(&applied, CAI_ERR_NOMEM, "copy goal control");
        else {
          memcpy(goal_command, "new ", 4U);
          memcpy(goal_command + 4U, command->text, length + 1U);
          action = goal_command;
        }
      }
      if (rc == CAI_OK)
        rc = cli_goal_command(state, action, &applied);
      free(goal_command);
    }
    {
      pslog_field fields[3];
      int published = cli_publish_status(state, state->runtime, error);
      const char *message = rc == CAI_OK              ? "Host control accepted"
                            : applied.message != NULL ? applied.message
                                                      : cai_status_string(rc);
      fields[0] = pslog_u64("control_id", (pslog_uint64)command->id);
      fields[1] = pslog_i64("control", command->type);
      fields[2] = pslog_i64("code", rc);
      cai_cli_log_message(
          cli_logger(state), rc == CAI_OK ? PSLOG_LEVEL_INFO : PSLOG_LEVEL_WARN,
          "host_control", "user", message, strlen(message), fields, 3U);
      cai_app_bridge_ack(state->bridge, command, rc, &applied);
      if (published != CAI_OK) {
        cai_error_cleanup(&applied);
        return published;
      }
    }
    cai_error_cleanup(&applied);
  }
  return cli_publish_status(state, state->runtime, error);
}

/* Level-triggered runtime, input, store and socket readiness share one owner
 * loop. Idle servers wait indefinitely except for an actual host deadline. */
static int cli_pump(cli_state *state, long timeout, cai_error *error) {
  struct pollfd descriptors[11U + CAI_CLI_WORKSPACE_HOST_FDS];
  size_t server_count = 0U;
  int fd, rc, stopping;
  rc = cai_cli_pouch_ownership_check(&state->pouch, error);
  if (rc != CAI_OK) {
    (void)cai_agent_runtime_cancel_turn(state->runtime, NULL);
    return rc;
  }
  if (timeout != 0L) {
    rc = cai_agent_runtime_wakeup_fd(state->runtime, &fd, error);
    if (rc != CAI_OK)
      return rc;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd = fd;
    descriptors[1].fd = state->bridge != NULL ? state->bridge->fd : -1;
    descriptors[2].fd = cai_cli_pouch_ownership_fd(&state->pouch);
    descriptors[3].fd = cai_input_controller_fd(state->input_controller);
    descriptors[4].fd =
        state->messages != NULL ? cai_cli_messages_fd(state->messages) : -1;
    descriptors[5].fd =
        state->quota_refresh.started ? state->quota_refresh.fd : -1;
    descriptors[6].fd = -1;
    if (state->review_runtime != NULL) {
      rc = cai_agent_runtime_wakeup_fd(state->review_runtime,
                                       &descriptors[6].fd, error);
      if (rc != CAI_OK)
        return rc;
    }
    descriptors[7].fd = !state->shell.eof && !state->shell.pending_length
                            ? state->shell.output_fd
                            : -1;
    descriptors[8].fd = !state->shell.exited ? state->shell.exit_fd : -1;
    descriptors[9].fd = state->shell.timer_fd;
    descriptors[10].fd = cai_cli_export_job_fd(state->export_job);
    descriptors[0].events = descriptors[1].events = descriptors[2].events =
        descriptors[3].events = descriptors[4].events = descriptors[5].events =
            descriptors[6].events = descriptors[7].events =
                descriptors[8].events = descriptors[9].events =
                    descriptors[10].events = POLLIN;
    rc = cai_cli_workspace_host_prepare(
        state->workspace_host, descriptors + 11U, CAI_CLI_WORKSPACE_HOST_FDS,
        &server_count, &timeout, error);
    if (rc != CAI_OK)
      return rc;
    do {
      rc = poll(descriptors, 11U + server_count, (int)timeout);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "wait for app events");
    timeout = 0L;
  }
  rc = cai_cli_workspace_host_pump(state->workspace_host, &stopping, error);
  if (rc != CAI_OK)
    return rc;
  if (stopping) {
    int detached;
    pthread_mutex_lock(&state->bridge->lock);
    detached = state->bridge->status.server.stop_reason ==
               CAI_ATTACHMENT_LAST_DETACHED;
    pthread_mutex_unlock(&state->bridge->lock);
    /* A composed native run preserves its foreground outcome. An independent
     * server ending its configured lifetime reports an orderly server STOP. */
    return detached && state->bridge->frontend_fd >= 0
               ? cli_stop_owned(state, error)
               : cli_stop(state, error);
  }
  rc = cai_cli_pouch_ownership_check(&state->pouch, error);
  if (rc != CAI_OK) {
    (void)cai_agent_runtime_cancel_turn(state->runtime, NULL);
    return rc;
  }
  rc = cai_agent_runtime_pump_bounded(state->runtime, timeout, 4U, error);
  if (rc == CAI_OK)
    rc = cli_host_process(state, error);
  if (rc == CAI_OK && state->review_runtime != NULL &&
      cli_review_pump(state) != CAI_OK)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "pump isolated reviewer");
  if (rc == CAI_OK && state->shell.pid > 0)
    rc = cli_shell_pump(state, error);
  if (rc == CAI_OK)
    rc = cli_export_pump(state, error);
  if (rc == CAI_OK)
    rc = cli_messages_process(state, 0, error);
  if (rc == CAI_OK && cai_cli_quota_take(&state->quota_refresh, &state->quota))
    rc = cli_sync_status(state, error);
  return rc;
}
static const char *const cli_reasoning_efforts[] = {
    "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};

static int cli_effort_known(const char *value) {
  size_t i;
  for (i = 0U;
       i < sizeof(cli_reasoning_efforts) / sizeof(cli_reasoning_efforts[0]);
       i++)
    if (strcmp(value, cli_reasoning_efforts[i]) == 0)
      return 1;
  return 0;
}

static int cli_tier_command(cli_state *state, const char *arguments, int fast,
                            cai_error *error) {
  cai_agent_runtime_settings effective, pending, requested;
  cai_agent_runtime_control_result result;
  cai_model_catalog *catalog = NULL;
  const cai_model_catalog_entry *entry;
  const char *choices[66], *current, *selected, *fast_tier = NULL;
  char buffer[128], message[512], *argument, *extra, *save = NULL;
  size_t count = 1U, i, j;
  int known = 0, listing, rc;
  pslog_field fields[2];
  if (strlen(arguments) >= sizeof(buffer))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "service-tier input is too long");
  strcpy(buffer, arguments);
  argument = strtok_r(buffer, " \t\r\n", &save);
  extra = strtok_r(NULL, " \t\r\n", &save);
  if (extra != NULL || (fast && argument != NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Usage: /tier [list | NAME] or /fast");
  rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                      error);
  if (rc != CAI_OK)
    return rc;
  current = pending.service_tier != NULL ? pending.service_tier
                                         : effective.service_tier;
  listing = argument != NULL && strcmp(argument, "list") == 0;
  choices[0] = "default";
  rc = cai_client_list_models(state->model_client,
                              CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED,
                              &catalog, error);
  if (rc != CAI_OK) {
    cai_error_cleanup(error);
    cai_error_init(error);
  }
  entry = cai_model_catalog_find(
      catalog, pending.model != NULL ? pending.model : effective.model);
  if (entry != NULL && entry->service_tiers_known) {
    known = 1;
    for (i = 0U; i < entry->service_tier_count && count < 66U; i++) {
      for (j = 0U; j < count; j++)
        if (strcmp(choices[j], entry->service_tiers[i]) == 0)
          break;
      if (j == count)
        choices[count++] = entry->service_tiers[i];
    }
  } else if (strcmp(state->options.provider, "openai") == 0) {
    choices[count++] = "auto";
    choices[count++] = "priority";
    choices[count++] = "flex";
  }
  for (i = 0U; i < count; i++)
    if (strcmp(choices[i], "priority") == 0 ||
        strcmp(choices[i], "fast") == 0) {
      fast_tier = choices[i];
      break;
    }
  if (listing) {
    snprintf(message, sizeof(message),
             "## Service tiers\n\nSelected: `%s`\n\nPending: `%s`\n\n"
             "| Tier | Routing |\n| --- | --- |\n",
             effective.service_tier != NULL ? effective.service_tier
                                            : "unset (provider routing)",
             pending.service_tier != NULL ? pending.service_tier : "none");
    rc = cli_text(state, message, strlen(message)) == 0 ? CAI_OK
                                                        : CAI_ERR_TRANSPORT;
    for (i = 0U; rc == CAI_OK && i < count; i++) {
      snprintf(message, sizeof(message), "| `%s` | %s |\n", choices[i],
               i == 0U ? "Explicit normal routing"
               : known ? "Advertised by provider"
                       : "Request option; provider validates account/model "
                         "availability");
      if (cli_text(state, message, strlen(message)) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
    if (rc == CAI_OK && !known && count == 1U) {
      static const char note[] =
          "\nTier metadata is unavailable. Explicit `/tier NAME` is "
          "forwarded to the provider; cycling and `/fast` require known "
          "choices.\n";
      if (cli_text(state, note, sizeof(note) - 1U) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    cai_model_catalog_close(catalog);
    return rc;
  }
  selected = argument;
  if (fast) {
    if (current != NULL &&
        (strcmp(current, "priority") == 0 || strcmp(current, "fast") == 0))
      selected = "default";
    else
      selected = fast_tier;
  } else if (selected == NULL && count > 1U) {
    for (i = 0U; i < count; i++)
      if (current != NULL && strcmp(current, choices[i]) == 0)
        break;
    selected = choices[i < count ? (i + 1U) % count : 1U];
  }
  if (selected != NULL && strcmp(selected, "fast") == 0 && fast_tier != NULL)
    selected = fast_tier;
  if (selected == NULL) {
    cai_model_catalog_close(catalog);
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "no available tier choices; use /tier list or explicit /tier NAME");
  }
  if (known) {
    for (i = 0U; i < count; i++)
      if (strcmp(selected, choices[i]) == 0)
        break;
    if (i == count) {
      cai_model_catalog_close(catalog);
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "tier is not advertised for this model; use /tier list");
    }
  }
  memset(&requested, 0, sizeof(requested));
  requested.present = CAI_AGENT_RUNTIME_SETTING_SERVICE_TIER;
  requested.service_tier = selected;
  rc = cai_agent_runtime_update_settings(state->runtime, &requested, &result,
                                         error);
  if (rc == CAI_OK) {
    fields[0] = pslog_str("service_tier", selected);
    fields[1] = pslog_bool("applied", result.applied);
    cli_logger(state)->info(cli_logger(state), "cai.service_tier.selected",
                            fields, 2U);
    if (!result.applied)
      state->settings_notice_fields |= requested.present;
    else {
      unsigned int announced =
          requested.present | state->settings_notice_fields;
      state->settings_notice_fields = 0U;
      rc = cli_settings_changed(state, &result.effective, announced);
    }
  }
  cai_model_catalog_close(catalog);
  return rc;
}

static int cli_settings_command(cli_state *state, const char *arguments,
                                int reasoning_only, cai_error *error) {
  cai_agent_runtime_settings effective, pending, requested;
  cai_agent_runtime_control_result result;
  cai_model_catalog *catalog = NULL;
  const cai_model_catalog_entry *entry;
  pslog_field fields[3];
  char buffer[512], message[1024], target_model[257];
  char *first, *second, *extra, *save = NULL;
  const char *model, *effort;
  size_t i;
  int rc, listing;
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  if (strlen(arguments) >= sizeof(buffer))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "model settings input is too long");
  strcpy(buffer, arguments);
  first = strtok_r(buffer, " \t\r\n", &save);
  second = strtok_r(NULL, " \t\r\n", &save);
  extra = strtok_r(NULL, " \t\r\n", &save);
  if (extra != NULL || (reasoning_only && second != NULL) ||
      (second != NULL && !cli_effort_known(second)))
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        reasoning_only ? "use /reasoning EFFORT (see /reasoning for choices)"
                       : "use /model MODEL [EFFORT] or /model EFFORT (see "
                         "/reasoning for choices)");
  memset(&requested, 0, sizeof(requested));
  if (first != NULL) {
    if (reasoning_only || (second == NULL && cli_effort_known(first))) {
      if (!cli_effort_known(first))
        return cai_cli_error(
            error, CAI_ERR_INVALID,
            "unknown reasoning effort; use /reasoning for choices");
      requested.present = CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
      requested.reasoning_effort = first;
    } else {
      if (strlen(first) > 256U)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "model ID must contain at most 256 bytes");
      requested.present = CAI_AGENT_RUNTIME_SETTING_MODEL;
      requested.model = first;
      if (second != NULL) {
        requested.present |= CAI_AGENT_RUNTIME_SETTING_REASONING_EFFORT;
        requested.reasoning_effort = second;
      }
    }
  }
  rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                      error);
  if (rc != CAI_OK)
    return rc;
  listing = first == NULL;
  model = requested.model != NULL ? requested.model
          : pending.model != NULL ? pending.model
                                  : effective.model;
  if (snprintf(target_model, sizeof(target_model), "%s", model) >=
      (int)sizeof(target_model))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "current model ID is too long");
  if (listing) {
    snprintf(message, sizeof(message),
             "## %s\n\nCurrent: `%s`\n\nReasoning effort: `%s`\n\n%s%s%s%s%s%s",
             reasoning_only ? "Reasoning" : "Model", effective.model,
             effective.reasoning_effort != NULL ? effective.reasoning_effort
                                                : "provider default",
             pending.model != NULL ? "Pending: `" : "",
             pending.model != NULL ? pending.model : "",
             pending.model != NULL ? "`\n\n" : "",
             pending.reasoning_effort != NULL ? "Pending reasoning: `" : "",
             pending.reasoning_effort != NULL ? pending.reasoning_effort : "",
             pending.reasoning_effort != NULL ? "`\n\n" : "");
    if (!reasoning_only && strcmp(state->options.provider, "openrouter") == 0) {
      static const char note[] =
          "OpenRouter's large catalog is not listed here. "
          "Use `/model MODEL [EFFORT]` or `/reasoning EFFORT`.\n";
      if (cli_text(state, message, strlen(message)) != 0 ||
          cli_text(state, note, strlen(note)) != 0)
        return CAI_ERR_TRANSPORT;
      return cli_finish_response(state) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    }
  }
  /* A separate discovery client avoids touching the agent worker's transport.
   */
  if (listing || requested.reasoning_effort != NULL) {
    rc = cai_client_list_models(
        state->model_client,
        listing ? CAI_MODEL_CATALOG_REFRESH_ONLINE
                : CAI_MODEL_CATALOG_REFRESH_ONLINE_IF_UNCACHED,
        &catalog, error);
    if (rc != CAI_OK) {
      cai_error_cleanup(error);
      cai_error_init(error);
      /* An explicit retry may fail while a previous successful catalog is
       * still useful. Offline lookup performs no further network request. */
      (void)cai_client_list_models(state->model_client,
                                   CAI_MODEL_CATALOG_REFRESH_OFFLINE, &catalog,
                                   error);
      cai_error_cleanup(error);
      cai_error_init(error);
    }
  }
  if (listing && cli_text(state, message, strlen(message)) != 0) {
    cai_model_catalog_close(catalog);
    return CAI_ERR_TRANSPORT;
  }
  entry = cai_model_catalog_find(catalog, target_model);
  if (!listing && requested.reasoning_effort != NULL && entry != NULL &&
      entry->reasoning_efforts_known) {
    for (i = 0U; i < entry->reasoning_effort_count; i++)
      if (strcmp(requested.reasoning_effort, entry->reasoning_efforts[i]) == 0)
        break;
    if (i == entry->reasoning_effort_count) {
      snprintf(message, sizeof(message),
               "Model %s does not advertise reasoning effort %s; use "
               "/reasoning for choices.",
               target_model, requested.reasoning_effort);
      cai_model_catalog_close(catalog);
      return cai_cli_error(error, CAI_ERR_INVALID, message);
    }
  }
  if (listing && reasoning_only) {
    int known = entry != NULL && entry->reasoning_efforts_known;
    size_t count = known ? entry->reasoning_effort_count
                         : sizeof(cli_reasoning_efforts) /
                               sizeof(cli_reasoning_efforts[0]);
    const char *const *choices =
        known ? (const char *const *)entry->reasoning_efforts
              : cli_reasoning_efforts;
    const char *note =
        known ? "Provider-advertised choices:\n\n"
              : "Model-specific reasoning choices are unavailable. The "
                "application accepts "
                "the "
                "following values; support depends on the provider/model.\n\n";
    snprintf(message, sizeof(message), "Choices for `%s`:\n\n", target_model);
    rc = cli_text(state, message, strlen(message)) == 0 ? CAI_OK
                                                        : CAI_ERR_TRANSPORT;
    if (rc == CAI_OK)
      rc =
          cli_text(state, note, strlen(note)) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    if (rc == CAI_OK && count == 0U)
      rc =
          cli_text(
              state, "This model advertises no reasoning-effort choices.\n",
              strlen("This model advertises no reasoning-effort choices.\n")) ==
                  0
              ? CAI_OK
              : CAI_ERR_TRANSPORT;
    for (i = 0U; rc == CAI_OK && i < count; i++) {
      snprintf(message, sizeof(message), "- `%s`%s\n", choices[i],
               entry != NULL && entry->default_reasoning_effort != NULL &&
                       strcmp(choices[i], entry->default_reasoning_effort) == 0
                   ? " (provider default)"
                   : "");
      if (cli_text(state, message, strlen(message)) != 0)
        rc = CAI_ERR_TRANSPORT;
    }
  } else if (listing) {
    if (catalog == NULL) {
      const char *note = "Model list unavailable. Use `/model` to retry, or "
                         "`/model MODEL [EFFORT]` to select directly.\n";
      rc =
          cli_text(state, note, strlen(note)) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    } else {
      size_t limit = catalog->count;
      if (strcmp(state->options.provider, "chatgpt") != 0 && limit > 50U)
        limit = 50U;
      rc =
          catalog->is_stale
              ? (cli_text(state,
                          "Using cached model catalog; `/model` retries "
                          "discovery.\n\n",
                          strlen("Using cached model catalog; `/model` retries "
                                 "discovery.\n\n")) == 0
                     ? CAI_OK
                     : CAI_ERR_TRANSPORT)
              : CAI_OK;
      for (i = 0U; rc == CAI_OK && i < limit; i++) {
        snprintf(message, sizeof(message), "- `%s`%s%s\n",
                 catalog->entries[i].slug,
                 catalog->entries[i].display_name != NULL ? " — " : "",
                 catalog->entries[i].display_name != NULL
                     ? catalog->entries[i].display_name
                     : "");
        if (cli_text(state, message, strlen(message)) != 0)
          rc = CAI_ERR_TRANSPORT;
      }
      if (rc == CAI_OK && catalog->count > limit) {
        snprintf(message, sizeof(message),
                 "\nShowing %lu of %lu models. Enter any model ID directly.\n",
                 (unsigned long)limit, (unsigned long)catalog->count);
        if (cli_text(state, message, strlen(message)) != 0)
          rc = CAI_ERR_TRANSPORT;
      }
    }
  }
  cai_model_catalog_close(catalog);
  if (listing)
    return rc == CAI_OK && cli_finish_response(state) == 0 ? CAI_OK
                                                           : CAI_ERR_TRANSPORT;
  rc = cai_agent_runtime_update_settings(state->runtime, &requested, &result,
                                         error);
  if (rc != CAI_OK)
    return rc;
  effort = requested.reasoning_effort;
  if (requested.model != NULL)
    snprintf(
        message, sizeof(message),
        result.applied
            ? "Model changed to %s%s%s.\n"
            : "Model %s%s%s queued for the next safe model/tool boundary.\n",
        requested.model, effort != NULL ? "; reasoning effort " : "",
        effort != NULL ? effort : "");
  else
    snprintf(message, sizeof(message),
             result.applied ? "Reasoning effort changed to %s.\n"
                            : "Reasoning effort %s queued for the next safe "
                              "model/tool boundary.\n",
             effort);
  fields[0] = pslog_str("model", target_model);
  fields[1] =
      pslog_str("reasoning_effort",
                effort != NULL ? effort : result.effective.reasoning_effort);
  fields[2] = pslog_bool("applied", result.applied);
  cai_cli_log_message(cli_logger(state), PSLOG_LEVEL_INFO,
                      requested.model != NULL ? "model_selection"
                                              : "reasoning_selection",
                      "user", message, strlen(message), fields, 3U);
  if (!result.applied) {
    state->settings_notice_fields |= requested.present;
    return CAI_OK;
  }
  requested.present |= state->settings_notice_fields;
  state->settings_notice_fields = 0U;
  return cli_settings_changed(state, &result.effective, requested.present);
}

/** Summary selection uses the same request-boundary update as model/effort.
 * Acknowledgement describes pending intent; only effective values announce a
 * completed change, including when a worker applies it later. */
static int cli_summary_command(cli_state *state, const char *argument,
                               cai_error *error) {
  cai_agent_runtime_settings effective, pending, requested;
  cai_agent_runtime_control_result result;
  char message[256], value[16];
  size_t length;
  int rc;
  argument += strspn(argument, " \t");
  length = strlen(argument);
  while (length != 0U && strchr(" \t", argument[length - 1U]) != NULL)
    --length;
  if (length == 0U) {
    rc = cai_agent_runtime_get_settings(state->runtime, &effective, &pending,
                                        error);
    if (rc != CAI_OK)
      return rc;
    snprintf(message, sizeof(message),
             "Reasoning summary: `%s`. Pending: `%s`. "
             "Use `/reasoning-summary none|auto|concise|detailed`.",
             effective.reasoning_summary != NULL ? effective.reasoning_summary
                                                 : "unset",
             pending.reasoning_summary != NULL ? pending.reasoning_summary
                                               : "none");
    return cli_markdown(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (length >= sizeof(value))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "use /reasoning-summary none|auto|concise|detailed");
  memcpy(value, argument, length);
  value[length] = '\0';
  if (strcmp(value, "none") != 0 && strcmp(value, "auto") != 0 &&
      strcmp(value, "concise") != 0 && strcmp(value, "detailed") != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "use /reasoning-summary none|auto|concise|detailed");
  memset(&requested, 0, sizeof(requested));
  requested.present = CAI_AGENT_RUNTIME_SETTING_REASONING_SUMMARY;
  requested.reasoning_summary = value;
  rc = cai_agent_runtime_update_settings(state->runtime, &requested, &result,
                                         error);
  if (rc != CAI_OK)
    return rc;
  if (!result.applied) {
    state->settings_notice_fields |= requested.present;
    return CAI_OK;
  }
  requested.present |= state->settings_notice_fields;
  state->settings_notice_fields = 0U;
  return cli_settings_changed(state, &result.effective, requested.present);
}

static int cli_tools_write(void *context, const char *bytes, size_t count) {
  return cli_text(context, bytes, count);
}

static int cli_trim_row(void *context, const cai_agent_history_entry *entry,
                        cai_error *error) {
  cli_state *state = context;
  char row[1200];
  cai_cli_history_row(row, sizeof(row), entry, 78);
  return cli_text(state, row, strlen(row)) == 0
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT, "display trim history");
}

static int cli_trim_command(cli_state *state, const char *arg,
                            cai_error *error) {
  unsigned long long value;
  size_t count;
  char *end;
  char message[128];
  int list, rc;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  list = *arg == '\0' || (strncmp(arg, "list", 4U) == 0 &&
                          (arg[4] == '\0' || arg[4] == ' ' || arg[4] == '\t'));
  if (*arg != '\0' && list)
    arg += 4U;
  while (*arg == ' ' || *arg == '\t')
    arg++;
  count = 20U;
  if (*arg != '\0') {
    if (*arg < '0' || *arg > '9')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Usage: /trim [COUNT | list [LIMIT]]");
    errno = 0;
    value = strtoull(arg, &end, 10);
    while (*end == ' ' || *end == '\t')
      end++;
    if (errno != 0 || *end != '\0' || value > (unsigned long long)(size_t)-1)
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "trim count must be a nonnegative integer");
    count = (size_t)value;
  }
  if (list) {
    /* Use a literal Markdown block so summaries cannot become headings,
     * nested lists, links or terminal escapes. libmdf owns all output. */
    if (cli_text(
            state, "## Trim history\n\n```text\n0. Keep current history\n",
            strlen("## Trim history\n\n```text\n0. Keep current history\n")) !=
        0)
      return CAI_ERR_TRANSPORT;
    rc = cai_agent_runtime_visit_history(state->runtime, count, cli_trim_row,
                                         state, error);
    if (cli_text(state, "```\n", 4U) != 0 && rc == CAI_OK)
      rc = CAI_ERR_TRANSPORT;
    if (cli_finish_response(state) != 0 && rc == CAI_OK)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  rc = cai_agent_runtime_trim_history(state->runtime, count, error);
  if (rc == CAI_OK) {
    snprintf(message, sizeof(message),
             "**Trimmed %lu newest history %s.** External changes remain.",
             (unsigned long)count, count == 1U ? "entry" : "entries");
    if (cli_markdown(state, message) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cli_sync_status(state, error);
  }
  return rc;
}

static int cli_review_pump(void *context) {
  cli_state *state = context;
  cai_agent_runtime_metrics metrics;
  cai_agent_run_state run_state;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_agent_runtime_pump_bounded(state->review_runtime, 0L, 4U, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_state(state->review_runtime, &run_state, &error);
  if (rc == CAI_OK)
    rc = cai_agent_runtime_get_metrics(state->review_runtime, &metrics, &error);
  if (rc == CAI_OK && !metrics.turn_active &&
      (run_state == CAI_AGENT_COMPLETED || run_state == CAI_AGENT_FAILED ||
       run_state == CAI_AGENT_CANCELLED)) {
    /* The worker can finish while the previous pump is delivering its last
     * snapshot. Drain terminal observations before closing the child, or its
     * final error/report can disappear despite being durable in the journal. */
    rc = cai_agent_runtime_pump(state->review_runtime, 0L, &error);
    if (rc == CAI_OK)
      rc = cai_agent_runtime_finish_review(state->runtime,
                                           state->review_runtime, &error);
    if (rc == CAI_OK && state->review_call[0] != '\0')
      rc = cai_cli_documents_operation(&state->documents, state->review_call,
                                       "command.review_settled", NULL, NULL,
                                       run_state == CAI_AGENT_COMPLETED ? CAI_OK
                                       : run_state == CAI_AGENT_CANCELLED
                                           ? CAI_ERR_CANCELLED
                                           : CAI_ERR_PROTOCOL,
                                       &error);
    if (rc == CAI_OK) {
      state->last_review_metrics = metrics;
      state->has_review_metrics = 1;
      cai_agent_runtime_close(state->review_runtime);
      state->review_runtime = NULL;
      state->review_call[0] = '\0';
    }
  }
  if (rc == CAI_OK)
    rc = cli_sync_status(state, &error);
  if (rc != CAI_OK)
    cli_print_error(state, "review", &error);
  cai_error_cleanup(&error);
  return rc;
}

static int cli_review_fix_goal(cli_state *state, int uncommitted,
                               const char *base, char *out, size_t capacity,
                               cai_error *error) {
  cai_agent_review_request request;
  cai_agent_review_request_init(&request);
  request.target =
      uncommitted ? CAI_AGENT_REVIEW_UNCOMMITTED : CAI_AGENT_REVIEW_DEFAULT;
  request.base_branch = base;
  return cai_agent_review_and_fix_objective(state->workspace, &request, out,
                                            capacity, error);
}

static int cli_review_fix_command(cli_state *state, const char *arguments,
                                  cai_error *error) {
  char command[sizeof(state->generated_goal) + 4U], base[512];
  const char *revision = NULL;
  size_t length;
  int uncommitted = 0, rc;
  arguments += strspn(arguments, " \t\r\n");
  if (*arguments != '\0') {
    length = strcspn(arguments, " \t\r\n");
    if (length == 11U && memcmp(arguments, "uncommitted", 11U) == 0 &&
        arguments[length + strspn(arguments + length, " \t\r\n")] == '\0')
      uncommitted = 1;
    else if (length == 4U && memcmp(arguments, "base", 4U) == 0) {
      arguments += length;
      arguments += strspn(arguments, " \t\r\n");
      length = strcspn(arguments, " \t\r\n");
      if (length == 0U || length >= sizeof(base) ||
          arguments[length + strspn(arguments + length, " \t\r\n")] != '\0')
        goto usage;
      memcpy(base, arguments, length);
      base[length] = '\0';
      revision = base;
    } else
      goto usage;
  }
  if (!state->options.review_subagent)
    return cai_cli_error(error, CAI_ERR_INVALID, "review subagent is disabled");
  if (!state->options.goals)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Goal pursuit requires goal tools; enable --goals "
                         "or [agent] goals=true and restart.");
  if (state->review_runtime != NULL || state->shell.pid > 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "finish the active review or shell before reviewing");
  memcpy(command, "new ", 4U);
  rc = cli_review_fix_goal(state, uncommitted, revision, command + 4U,
                           sizeof(command) - 4U, error);
  return rc == CAI_OK ? cli_goal_command(state, command, error) : rc;
usage:
  return cai_cli_error(error, CAI_ERR_INVALID,
                       "Use /review-and-fix [uncommitted|base REF].");
}

static int cli_review_command(cli_state *state, const char *arguments,
                              cai_error *error) {
  cai_agent_review_request request;
  int rc;
  while (*arguments == ' ' || *arguments == '\t')
    arguments++;
  cai_agent_review_request_init(&request);
  if (strcmp(arguments, "uncommitted") == 0)
    request.target = CAI_AGENT_REVIEW_UNCOMMITTED;
  else if (strncmp(arguments, "base ", 5U) == 0) {
    request.target = CAI_AGENT_REVIEW_BASE_BRANCH;
    request.base_branch = arguments + 5;
  } else if (strncmp(arguments, "custom ", 7U) == 0) {
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = arguments + 7;
  } else if (strncmp(arguments, "commit ", 7U) == 0) {
    request.target = CAI_AGENT_REVIEW_COMMIT;
    request.commit = arguments + 7;
  } else if (*arguments != '\0') {
    request.target = CAI_AGENT_REVIEW_CUSTOM;
    request.instructions = arguments;
  }
  if (!state->options.review_subagent)
    return cai_cli_error(error, CAI_ERR_INVALID, "review subagent is disabled");
  if (state->review_runtime != NULL || state->shell.pid > 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "finish the active review or shell before reviewing");
  rc = cli_review_boundary(state, 1, error);
  if (rc != CAI_OK)
    return rc;
  if (state->documents.active) {
    snprintf(state->review_call, sizeof(state->review_call), "%s",
             state->documents.call);
    request.parent_call_id = state->review_call;
  }
  request.developer_instructions_override = state->initial_review_instructions;
  rc = cai_agent_runtime_start_review(state->runtime, &request,
                                      &state->review_runtime, error);
  if (state->review_runtime != NULL) {
    free(state->initial_review_instructions);
    state->initial_review_instructions = NULL;
  }
  if (state->review_runtime == NULL) {
    int finished;
    state->review_call[0] = '\0';
    finished = cli_review_boundary(state, 0, NULL);
    if (finished != CAI_OK)
      return finished;
    return rc;
  }
  return rc;
}

static int cli_yield_command(cli_state *state, const char *arg, int mode,
                             cai_error *error) {
  char message[320], *end;
  long value;
  int rc =
      cai_agent_runtime_get_terminal_yield_time(state->runtime, &value, error);
  if (rc != CAI_OK)
    return rc;
  arg += strspn(arg, " \t");
  if (mode != 0 && *arg != '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Use /foreground or /background.");
  if (mode > 0)
    value = 0L;
  else if (mode < 0 && value == 0L)
    value = CAI_TERMINAL_DEFAULT_YIELD_TIME_MS;
  else if (*arg != '\0') {
    errno = 0;
    value = strtol(arg, &end, 10);
    end += strspn(end, " \t");
    if (errno != 0 || end == arg || *end != '\0' || value < 0L ||
        value > 30000L)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "Use /yield_time_ms 0..30000; 0 selects foreground mode.");
  }
  if (mode != 0 || *arg != '\0') {
    rc =
        cai_agent_runtime_set_terminal_yield_time(state->runtime, value, error);
    if (rc != CAI_OK)
      return rc;
    state->options.foreground = value == 0L;
    state->options.yield_time_ms =
        value != 0L ? value : CAI_TERMINAL_DEFAULT_YIELD_TIME_MS;
  }
  if (value == 0L)
    snprintf(message, sizeof(message),
             "**Foreground commands:** wait for completion or cancellation "
             "(`yield_time_ms = 0`). Use `/background` to restore yielding.");
  else
    snprintf(message, sizeof(message),
             "**Background commands:** default yield interval `%ld ms`. "
             "Use `/foreground` to wait for completion or cancellation.",
             value);
  return cli_markdown(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}

static int cli_retention_command(cli_state *state, const char *arg, int history,
                                 cai_error *error) {
  char *end, message[512], empty[1] = {0};
  long value;
  int rc;
  arg += strspn(arg, " \t");
  if (*arg != '\0') {
    errno = 0;
    if (strncmp(arg, "off", 3U) == 0 &&
        arg[3U + strspn(arg + 3U, " \t")] == '\0') {
      value = 0L;
      end = empty;
    } else
      value = strtol(arg, &end, 10);
    end += strspn(end, " \t");
    if (errno != 0 || end == arg || *end != '\0' || value < 0L ||
        value > (history ? INT_MAX : LONG_MAX / 86400L))
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          history
              ? "use /history COUNT (nonnegative entry count)"
              : "use /retention DAYS; 0 or off keeps the conversation forever");
    if (history) {
      int previous = state->options.prompt_history_limit;
      state->options.prompt_history_limit = (int)value;
      rc = cli_prompt_configure(state, error);
      if (rc != CAI_OK)
        state->options.prompt_history_limit = previous;
    } else
      rc = cai_cli_pouch_set_retention(
          &state->pouch, cai_agent_runtime_session_id(state->runtime), value,
          error);
    if (rc != CAI_OK)
      return rc;
  }
  if (history) {
    snprintf(
        message, sizeof(message),
        "Prompt history limit: **%d entries**.\n\n"
        "Controls recall and native stored-entry retention. Oldest "
        "entries are pruned asynchronously by storage maintenance. "
        "Zero disables recall and recording without deleting saved prompts.",
        state->options.prompt_history_limit);
  } else {
    rc = cai_cli_pouch_retention(&state->pouch,
                                 cai_agent_runtime_session_id(state->runtime),
                                 &value, error);
    if (rc != CAI_OK)
      return rc;
    if (value == 0L)
      snprintf(message, sizeof(message),
               "Conversation retention: **forever**.");
    else
      snprintf(
          message, sizeof(message),
          "Conversation retention: **%ld days from last update**.\n\n"
          "Liblockdc maintenance removes the whole collection after expiry.",
          value);
  }
  return cli_markdown(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
}

static int cli_handle_command(cli_state *state, const char *line,
                              cai_error *error, int *handled) {
  const char *arg;
  int rc;
  *handled = 1;
  if (strncmp(line, "/history", 8U) == 0 &&
      (line[8] == '\0' || line[8] == ' ' || line[8] == '\t'))
    return cli_retention_command(state, line + 8U, 1, error);
  if (strncmp(line, "/retention", 10U) == 0 &&
      (line[10] == '\0' || line[10] == ' ' || line[10] == '\t'))
    return cli_retention_command(state, line + 10U, 0, error);
  if (cli_is_compact(line)) {
    arg = line + 8U;
    arg += strspn(arg, " \t");
    if (*arg != '\0')
      return cli_markdown(state, "Usage: `/compact`") == 0 ? CAI_OK
                                                           : CAI_ERR_TRANSPORT;
    if (state->shell.pid > 0 || state->review_runtime != NULL)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "Compaction waits for the current shell or review to finish");
    rc = cai_agent_runtime_compact(state->runtime, error);
    if (rc == CAI_OK) {
      state->awaiting_turn = 1;
    }
    return rc;
  }
  if (cli_is_yolo(line)) {
    int enabled, requested, applied, pending;
    char message[512];
    rc = cai_agent_runtime_get_yolo(state->runtime, &enabled, NULL, error);
    if (rc != CAI_OK)
      return rc;
    if (cli_yolo_value(line, &requested)) {
      rc = cai_agent_runtime_try_set_yolo(state->runtime, requested, &applied,
                                          error);
      if (rc != CAI_OK)
        return rc;
      if (!applied)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "YOLO change waits for an idle runtime and tools");
      state->options.yolo = enabled = requested;
      snprintf(
          message, sizeof(message),
          "**YOLO %s.** %s\n\nThis runtime only; configuration is unchanged.\n",
          enabled ? "enabled" : "disabled",
          enabled ? "Local tools use ordinary host filesystem and environment "
                    "access."
                  : "Local tool confinement and environment filtering are "
                    "restored.");
    } else {
      arg = line + 5U;
      arg += strspn(arg, " \t");
      if (*arg != '\0')
        return cai_cli_error(error, CAI_ERR_INVALID, "Use /yolo [on|off]");
      pending = cli_yolo_pending(state, &requested);
      snprintf(
          message, sizeof(message),
          "## Execution policy\n\nYOLO: **%s**.\n\n%s%s%s\n"
          "Use `/yolo on` or `/yolo off`. Changes wait until active turns, "
          "tools and subagents finish; pending entries stay editable.\n",
          enabled ? "on (unrestricted)" : "off (restricted)",
          pending ? "Pending: **" : "",
          pending ? (requested ? "on" : "off") : "", pending ? "**.\n\n" : "");
    }
    return cli_markdown(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/review-and-fix") == 0 ||
      strncmp(line, "/review-and-fix ", 16U) == 0 ||
      strncmp(line, "/review-and-fix\t", 16U) == 0)
    return cli_review_fix_command(state, line + 15U, error);
  if (strcmp(line, "/review") == 0 || strncmp(line, "/review ", 8U) == 0 ||
      strncmp(line, "/review\t", 8U) == 0)
    return cli_review_command(state, line + 7U, error);
  if (strcmp(line, "/todo") == 0 || strncmp(line, "/todo ", 6U) == 0 ||
      strncmp(line, "/todo\t", 6U) == 0) {
    rc = cai_cli_todo_command(state->runtime, line + 5U,
                              &state->options.todo_layout, cli_tools_write,
                              state, error);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/queue") == 0 || strncmp(line, "/queue ", 7U) == 0 ||
      strncmp(line, "/queue\t", 7U) == 0) {
    cai_agent_runtime_metrics metrics;
    cai_agent_run_state run_state;
    char name[129], message[256];
    size_t length;
    arg = line + 6U;
    while (*arg == ' ' || *arg == '\t')
      arg++;
    length = strlen(arg);
    while (length > 0U && (arg[length - 1U] == ' ' || arg[length - 1U] == '\t'))
      length--;
    if ((length == 2U && strncmp(arg, "on", length) == 0) ||
        (length == 3U && strncmp(arg, "off", length) == 0)) {
      rc = cli_set_messaging(state, length == 2U, error);
      if (rc != CAI_OK)
        return rc;
      length = 0U;
    }
    if (state->messages == NULL) {
      const char *hint = "Messaging is disabled. Use `/queue on` to enable "
                         "the message tool and incoming queue.\n";
      return cli_markdown(state, hint) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
    }
    if (length > 0U) {
      if (length >= sizeof(name))
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "Queue names must be at most 128 characters.");
      memcpy(name, arg, length);
      name[length] = '\0';
      rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
      if (rc == CAI_OK)
        rc = cai_agent_runtime_state(state->runtime, &run_state, error);
      if (rc != CAI_OK)
        return rc;
      if (metrics.turn_active || state->awaiting_turn ||
          run_state == CAI_AGENT_SAMPLING ||
          run_state == CAI_AGENT_DISPATCHING_TOOL)
        return cai_cli_error(error, CAI_ERR_INVALID,
                             "Change the message queue between turns.");
      strcpy(message, cai_cli_messages_name(state->messages));
      rc = cai_cli_messages_set_queue(state->messages, name, error);
      /* A selected name survives /new and /resume in this app, never in
       * config.ini or the conversation commit. */
      if (rc == CAI_OK ||
          strcmp(cai_cli_messages_name(state->messages), message) != 0) {
        strcpy(state->message_queue_override,
               cai_cli_messages_name(state->messages));
        state->options.message_queue = state->message_queue_override;
      }
      if (rc != CAI_OK)
        return rc;
    }
    snprintf(message, sizeof(message), "Message queue: `%s`\n",
             cai_cli_messages_name(state->messages));
    return cli_markdown(state, message) == 0 ? CAI_OK : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/autoqueue") == 0 ||
      strncmp(line, "/autoqueue ", 11U) == 0) {
    int enabled;
    /* Admission policy belongs to the input queue, including while a mailbox
     * is disabled; enabling it later uses the same explicit selection. */
    {
      cai_input_state pending;
      rc = cai_input_queue_get_state(state->input, &pending, error);
      if (rc != CAI_OK)
        return rc;
      enabled = pending.auto_approval;
    }
    arg = line + 10U;
    while (*arg == ' ')
      arg++;
    if (*arg == '\0')
      enabled = !enabled;
    else if (strcmp(arg, "on") == 0)
      enabled = 1;
    else if (strcmp(arg, "off") == 0)
      enabled = 0;
    else
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "Usage: /autoqueue [on|off]");
    {
      rc = cli_input_request(state, CAI_INPUT_AUTO_APPROVAL, NULL,
                             CAI_INPUT_QUEUED, enabled, NULL, NULL, error);
      if (rc != CAI_OK)
        return rc;
    }
    state->options.enable_auto_enqueue = enabled;
    return cli_markdown(
               state,
               enabled
                   ? "**Autoqueue on:** waiting and future incoming prompts "
                     "are "
                     "automatically approved."
                   : "**Autoqueue off:** new incoming prompts require approval "
                     "(`Alt-A`, then `Alt-Y`; `Alt-N` rejects, `Alt-E` "
                     "edits).") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/trim") == 0 || strncmp(line, "/trim ", 6U) == 0 ||
      strncmp(line, "/trim\t", 6U) == 0)
    return cli_trim_command(state, line + 5U, error);
  if (strcmp(line, "/tier") == 0 || strncmp(line, "/tier ", 6U) == 0 ||
      strncmp(line, "/tier\t", 6U) == 0)
    return cli_tier_command(state, line + 5U, 0, error);
  if (strcmp(line, "/foreground") == 0 ||
      strncmp(line, "/foreground ", 12U) == 0 ||
      strncmp(line, "/foreground\t", 12U) == 0)
    return cli_yield_command(state, line + 11U, 1, error);
  if (strcmp(line, "/fg") == 0 || strncmp(line, "/fg ", 4U) == 0 ||
      strncmp(line, "/fg\t", 4U) == 0)
    return cli_yield_command(state, line + 3U, 1, error);
  if (strcmp(line, "/background") == 0 ||
      strncmp(line, "/background ", 12U) == 0 ||
      strncmp(line, "/background\t", 12U) == 0)
    return cli_yield_command(state, line + 11U, -1, error);
  if (strcmp(line, "/bg") == 0 || strncmp(line, "/bg ", 4U) == 0 ||
      strncmp(line, "/bg\t", 4U) == 0)
    return cli_yield_command(state, line + 3U, -1, error);
  if (strcmp(line, "/yield_time_ms") == 0 ||
      strncmp(line, "/yield_time_ms ", 15U) == 0 ||
      strncmp(line, "/yield_time_ms\t", 15U) == 0)
    return cli_yield_command(state, line + 14U, 0, error);
  if (strcmp(line, "/fast") == 0 || strncmp(line, "/fast ", 6U) == 0 ||
      strncmp(line, "/fast\t", 6U) == 0)
    return cli_tier_command(state, line + 5U, 1, error);
  if (line[0] == '!')
    return cli_shell_start(state, line + 1U, error);
  if (strcmp(line, "/shell-context") == 0 ||
      strncmp(line, "/shell-context ", 15U) == 0) {
    arg = line + 14U;
    while (*arg == ' ')
      arg++;
    if (strcmp(arg, "on") == 0)
      state->options.shell_context = 1;
    else if (strcmp(arg, "off") == 0)
      state->options.shell_context = 0;
    else if (*arg != '\0')
      return cli_markdown(state, "Usage: `/shell-context [on|off]`") == 0
                 ? CAI_OK
                 : CAI_ERR_TRANSPORT;
    return cli_markdown(state,
                        state->options.shell_context
                            ? "**Shell results will be shared with the model** "
                              "for future commands."
                            : "**Shell results will stay out of model "
                              "context** for future commands.") == 0
               ? CAI_OK
               : CAI_ERR_TRANSPORT;
  }
  if (strcmp(line, "/goal") == 0 || strncmp(line, "/goal ", 6U) == 0 ||
      strncmp(line, "/goal\t", 6U) == 0)
    return cli_goal_command(state, line + 5U, error);
  if (strcmp(line, "/reasoning-tool") == 0 ||
      strncmp(line, "/reasoning-tool ", 16U) == 0 ||
      strncmp(line, "/reasoning-tool\t", 16U) == 0)
    return cli_reasoning_tool_command(state, line + 15U, error);
  if (strcmp(line, "/model") == 0 || strncmp(line, "/model ", 7U) == 0 ||
      strncmp(line, "/model\t", 7U) == 0)
    return cli_settings_command(state, line + 6U, 0, error);
  if (strcmp(line, "/reasoning") == 0 ||
      strncmp(line, "/reasoning ", 11U) == 0 ||
      strncmp(line, "/reasoning\t", 11U) == 0)
    return cli_settings_command(state, line + 10U, 1, error);
  if (strcmp(line, "/reasoning-summary") == 0 ||
      strncmp(line, "/reasoning-summary ", 19U) == 0 ||
      strncmp(line, "/reasoning-summary\t", 19U) == 0)
    return cli_summary_command(state, line + 18U, error);
  if (strcmp(line, "/help") == 0) {
    if (cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cai_cli_command_help(state->options.invocation_name,
                              state->workspace_host != NULL, cli_tools_write,
                              state);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strncmp(line, "/help ", 6U) == 0) {
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Use /help for inline help or /help -p (alias /help "
                         "less) in a terminal.");
  }
  if (strcmp(line, "/new") == 0) {
    return cli_switch_session(state, NULL, 1, error);
  }
  if (strncmp(line, "/tools ", 7U) == 0) {
    const char *action = line + 7U;
    const char *message = "Tool catalog updated for the next model request.\n";
    if (state->plugins == NULL)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "Plugins are disabled; enable --plugins or [plugins] enabled.");
    if (strcmp(action, "reload") == 0)
      rc = cai_plugins_reload(state->plugins, error);
    else if (strncmp(action, "reload ", 7U) == 0)
      rc = cai_plugins_load(state->plugins, action + 7U, error);
    else if (strncmp(action, "unload ", 7U) == 0)
      rc = cai_plugins_unload(state->plugins, action + 7U, error);
    else
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "Use /tools, /tools reload [PATH], or /tools unload NAME.");
    if (rc == CAI_OK && (cli_text(state, message, strlen(message)) != 0 ||
                         cli_finish_response(state) != 0))
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/mcp") == 0 || strncmp(line, "/mcp ", 5U) == 0) {
    if (cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    arg = line + 4U;
    arg += strspn(arg, " \t");
    rc = cai_cli_mcp_command(&state->mcp, arg, state->runtime, cli_tools_write,
                             state, error);
    if (rc == CAI_OK &&
        (strncmp(arg, "enable", 6U) == 0 || strncmp(arg, "disable", 7U) == 0 ||
         strncmp(arg, "refresh", 7U) == 0) &&
        !cai_cli_mcp_ready(&state->mcp))
      state->mcp_notice_pending = 1;
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/tools") == 0) {
    if (cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    /* Full tables retain their layout; libmdf switches oversized tables to
     * its bounded row renderer automatically. Use the normal feed throughout.
     */
    rc = cai_cli_tools_markdown(&state->mcp, state->runtime, 1, cli_tools_write,
                                state, error);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/skills") == 0) {
    if (cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cai_cli_skills_markdown(state->runtime, state->options.skills,
                                 cli_tools_write, state, error);
    if (rc == CAI_OK && cli_finish_response(state) != 0)
      rc = CAI_ERR_TRANSPORT;
    return rc;
  }
  if (strcmp(line, "/log") == 0) {
    if (cli_finish_response(state) != 0 ||
        cai_cli_status_log_markdown(
            state->log != NULL && state->log->fd >= 0 ? state->log->path : NULL,
            0, cli_tools_write, state) != 0 ||
        cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    return CAI_OK;
  }
  if (strcmp(line, "/status") == 0) {
    cai_agent_runtime_ids ids;
    cai_agent_instruction_status instructions;
    cai_agent_runtime_settings settings;
    cai_agent_runtime_metrics metrics;
    cai_error optional_error;
    char markdown[2048], mailbox_row[320], policy_row[256];
    int yolo, pending_yolo, has_pending_yolo;
    const char *model = state->options.model;
    const char *effort = state->options.reasoning_effort;
    memset(&settings, 0, sizeof(settings));
    cai_error_init(&optional_error);
    if (cai_agent_runtime_get_settings(state->runtime, &settings, NULL,
                                       &optional_error) == CAI_OK) {
      if (settings.model != NULL)
        model = settings.model;
      if (settings.reasoning_effort != NULL)
        effort = settings.reasoning_effort;
    }
    cai_error_cleanup(&optional_error);
    rc = cai_agent_runtime_get_metrics(state->runtime, &metrics, error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_agent_runtime_get_ids(state->runtime, &ids, error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_agent_runtime_get_instruction_status(state->runtime, &instructions,
                                                  error);
    if (rc != CAI_OK)
      return rc;
    rc = cai_agent_runtime_get_yolo(state->runtime, &yolo, NULL, error);
    if (rc != CAI_OK)
      return rc;
    has_pending_yolo = cli_yolo_pending(state, &pending_yolo);
    snprintf(policy_row, sizeof(policy_row), "| Execution mode | %s |\n%s%s%s",
             yolo ? "YOLO (unrestricted)"
                  : "Restricted (terminal has no OS sandbox)",
             has_pending_yolo ? "| Pending execution mode | " : "",
             has_pending_yolo ? (pending_yolo ? "YOLO" : "Restricted") : "",
             has_pending_yolo ? " (queued) |\n" : "");
    cli_refresh_quota(state, 1);
    snprintf(mailbox_row, sizeof(mailbox_row),
             "| Message queue | `%s` |\n| Autoqueue | %s |\n",
             state->messages != NULL ? cai_cli_messages_name(state->messages)
                                     : "disabled",
             state->messages == NULL              ? "disabled"
             : state->options.enable_auto_enqueue ? "on (automatic approval)"
                                                  : "off (manual approval)");
    if (cai_cli_status_markdown(
            markdown, sizeof(markdown), state->options.program_name, model,
            effort, state->options.provider, &metrics,
            state->auth != NULL ? &state->quota : NULL, &instructions) != 0 ||
        cli_text(state, markdown, strlen(markdown)) != 0 ||
        cli_tools_write(state, policy_row, strlen(policy_row)) != 0 ||
        cai_cli_status_ids_markdown(&ids, cli_tools_write, state) != 0 ||
        cai_cli_status_log_markdown(
            state->log != NULL && state->log->fd >= 0 ? state->log->path : NULL,
            1, cli_tools_write, state) != 0 ||
        cli_text(state, mailbox_row, strlen(mailbox_row)) != 0 ||
        cai_cli_todo_status(state->runtime, cli_tools_write, state, error) !=
            CAI_OK ||
        cai_cli_tools_markdown(&state->mcp, state->runtime, 0, cli_tools_write,
                               state, error) != CAI_OK ||
        cli_finish_response(state) != 0) {
      return CAI_ERR_TRANSPORT;
    }
    return CAI_OK;
  }
  if (strcmp(line, "/export") == 0 || strncmp(line, "/export ", 8U) == 0 ||
      strncmp(line, "/export\t", 8U) == 0) {
    cai_cli_export *source = NULL;
    cai_agent_handover *capture = NULL;
    const char *id = cai_agent_runtime_session_id(state->runtime);
    char basename[PATH_MAX];
    size_t length;
    arg = line + 7U;
    arg += strspn(arg, " \t");
    length = strlen(arg);
    while (length > 0U && strchr(" \t", arg[length - 1U]) != NULL)
      length--;
    if (length >= sizeof(basename))
      return cai_cli_error(error, CAI_ERR_INVALID, "export file path too long");
    memcpy(basename, arg, length);
    basename[length] = '\0';
    state->list_line_start = 1;
    if (cli_text(state, "## Exporting conversation\n\n", 27U) != 0 ||
        cli_finish_response(state) != 0)
      return CAI_ERR_TRANSPORT;
    rc = cai_agent_runtime_prepare_handover(state->runtime, &capture, error);
    if (rc == CAI_OK)
      rc = cai_cli_export_prepare(
          &state->pouch, id, state->workspace, state->options.export_dir,
          length != 0U ? basename : NULL, &source, error);
    if (rc == CAI_OK && state->export_job == NULL)
      rc = cai_cli_export_job_open(&state->export_job, error);
    if (rc == CAI_OK)
      rc = cai_cli_export_job_start(state->export_job, source, capture,
                                    state->documents.call, error);
    if (rc == CAI_OK) {
      rc = cai_cli_documents_suspend(&state->documents, error);
      if (rc != CAI_OK)
        cai_cli_export_job_cancel(state->export_job);
    } else {
      cai_cli_export_close(source);
      cai_agent_handover_close(capture);
    }
    return rc;
  }
  if (strcmp(line, "/resume") == 0) {
    return cli_list_sessions(state, error);
  }
  if (strncmp(line, "/resume ", 8U) == 0) {
    char selected[129];
    arg = line + 8U;
    arg += strspn(arg, " \t");
    rc = cli_session_destination(state, arg, selected, error);
    return rc == CAI_OK ? cli_switch_session(state, selected, 0, error) : rc;
  }
  if (line[0] == '/')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Unknown command. Send /help for usage.");
  *handled = 0;
  return CAI_OK;
}

static int cli_dispatch_command(cli_state *state, const char *line,
                                const cai_input_claim *claim, cai_error *error,
                                int *handled) {
  int rc;
  char *normalized = NULL;
  const char *command = line;
  /* Classification and execution share horizontal-whitespace semantics.
   * Preserve argument contents (especially goal text and file paths), and
   * leave ! shell commands entirely untouched. The feed echoes the input. */
  if (line[0] == '/') {
    size_t name = strcspn(line, " \t"), length;
    const char *arg = line + name;
    const char *canonical = cai_cli_command_name(line, &name);
    arg += strspn(arg, " \t");
    length = strlen(arg);
    while (length > 0U && strchr(" \t", arg[length - 1U]) != NULL)
      length--;
    normalized = malloc(name + length + 2U);
    if (normalized == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "normalize command input");
    memcpy(normalized, canonical, name);
    if (length != 0U) {
      normalized[name++] = ' ';
      memcpy(normalized + name, arg, length);
      name += length;
    }
    normalized[name] = '\0';
    command = normalized;
  }
  *handled = 1;
  state->documents.progress = cli_document_progress;
  state->documents.progress_context = state;
  rc = cai_cli_documents_begin(&state->documents, claim, error);
  if (rc == CAI_OK)
    rc = cli_command_input(state, line);
  if (rc == CAI_OK)
    rc = cai_cli_command_frontend(command)
             ? cai_cli_documents_delegate(&state->documents, line, error)
             : cli_handle_command(state, command, error, handled);
  if (state->documents.active)
    rc = cai_cli_documents_complete(&state->documents, rc, error);
  state->settlement_pending = 1;
  free(normalized);
  return rc;
}

static int cli_submit_automation(cli_state *state, const char *instruction,
                                 cai_error *error) {
  int rc;
  if (instruction == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "instruction is required");
  state->awaiting_turn = 1;
  /* Preserve ordinary prompt journal entries. If another batch turn is
   * pending, this instruction must remain a new turn rather than steering it.
   */
  rc = cai_agent_runtime_submit_interactive(state->runtime, instruction, error);
  if (rc != CAI_OK)
    state->awaiting_turn = 0;
  return rc;
}

static int cli_continue_goal(cli_state *state, cai_error *error) {
  int started = 0;
  int rc = cai_agent_runtime_continue_goal(state->runtime, &started, error);
  if (rc == CAI_OK && started) {
    state->awaiting_turn = 1;
    state->automation_done = 0;
  }
  return rc;
}

static int cli_start_goal(cli_state *state, const char *objective,
                          cai_error *error) {
  int rc;

  state->starting_goal = 1;
  state->startup_goal_pending = 1U;
  rc = cai_agent_runtime_replace_goal(state->runtime, objective, error);
  while (rc == CAI_OK && state->startup_goal_pending > 0U) {
    rc = cli_pump(state, 100L, error);
    if (rc != CAI_OK)
      break;
    if (state->host_stopped)
      break;
    if (state->automation_failed) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "goal could not be started; see the preceding error");
      break;
    }
  }
  state->starting_goal = 0;
  state->startup_goal_pending = 0U;
  return rc;
}

static int cli_start_automation(cli_state *state, cai_error *error) {
  const char *objective;
  const char *instruction;
  int rc;

  if (state->frontend_startup) {
    state->next_instruction = state->options.instruction_count;
    state->automation_done = 1;
    return CAI_OK;
  }
  objective = state->options.goal;
  if (state->options.review_and_fix) {
    rc = cli_review_fix_goal(state, state->options.review_uncommitted,
                             state->options.base, state->generated_goal,
                             sizeof(state->generated_goal), error);
    if (rc != CAI_OK)
      return rc;
    objective = state->generated_goal;
  }
  instruction = cai_cli_instruction_at(&state->options, 0U);
  if (objective != NULL && instruction != NULL) {
    /* Terminal-free startup uses the same core action as the queued command.
     * The goal is not made active ahead of its initial user turn. */
    state->next_instruction = 1U;
    state->awaiting_turn = 1;
    rc = cai_agent_runtime_start_goal(state->runtime, objective, instruction,
                                      NULL, error);
    if (rc != CAI_OK)
      state->awaiting_turn = 0;
    return rc;
  }
  if (objective != NULL) {
    rc = cli_start_goal(state, objective, error);
    if (rc != CAI_OK)
      return rc;
    if (state->exit_requested)
      return CAI_OK;
  }
  if (state->options.instruction_count > 0U) {
    instruction = cai_cli_instruction_at(&state->options, 0U);
    state->next_instruction = 1U;
  } else {
    instruction = NULL;
  }
  if (instruction == NULL) {
    state->automation_done = 1;
    return cli_advance_automation(state, error);
  }
  return cli_submit_automation(state, instruction, error);
}

static int cli_advance_automation(cli_state *state, cai_error *error) {
  cai_agent_goal_snapshot goal;
  cai_agent_run_state run_state;
  const char *next;
  int rc;

  if (cai_cli_export_job_active(state->export_job) || state->awaiting_turn ||
      state->batch_pending_turns > 0U || state->exit_requested ||
      state->review_runtime != NULL || state->shell.pid > 0)
    return CAI_OK;
  if (state->automation_failed) {
    state->automation_done = 1;
    return CAI_OK;
  }
  /* A completed turn may be delivered before a new/resumed host goal is
   * applied. Do not consume that older goal projection as the new outcome. */
  if (state->goal_starts_pending > 0U)
    return CAI_OK;
  rc = cai_agent_runtime_state(state->runtime, &run_state, error);
  if (rc != CAI_OK)
    return rc;
  if (run_state == CAI_AGENT_SAMPLING ||
      run_state == CAI_AGENT_DISPATCHING_TOOL)
    return CAI_OK;
  if (state->input_admin != NULL) {
    size_t index;
    cai_input_entry entry;
    cai_input_state pending;
    rc = cai_input_queue_get_state(state->input, &pending, error);
    if (rc != CAI_OK)
      return rc;
    for (index = 0U; index < pending.count; ++index) {
      rc = cai_input_queue_entry_at(state->input_admin, index, &entry, error);
      if (rc != CAI_OK)
        return rc;
      if (!entry.held && !entry.stopped)
        return CAI_OK;
    }
  }
  /* Pursuit belongs to the durable goal, not to the UI path that created it.
   * This also covers model-created goals and resumed active commits.
   * Cancellation/failure above requires a new user turn or explicit resume.
   */
  if (state->options.goals) {
    rc = cai_agent_runtime_get_goal(state->runtime, &goal, error);
    if (rc != CAI_OK)
      return rc;
    if (goal.has_goal && goal.status != NULL &&
        strcmp(goal.status, "active") == 0) {
      state->automation_done = 0;
      return cli_continue_goal(state, error);
    }
    if (!state->automation_done && goal.has_goal && goal.status != NULL &&
        strcmp(goal.status, "complete") != 0) {
      state->automation_failed = 1;
      state->automation_done = 1;
      cli_diagnostic(state, PSLOG_LEVEL_WARN, goal.status);
      return CAI_OK;
    }
  }
  if (state->automation_done)
    return CAI_OK;
  next = cai_cli_instruction_at(&state->options, state->next_instruction);
  if (next != NULL) {
    state->next_instruction++;
    return cli_submit_automation(state, next, error);
  }
  state->automation_done = 1;
  return CAI_OK;
}

/** Publish only after owner callbacks, host controls and automatic pursuit
 * have drained. A terminal model record alone does not settle a synchronous
 * application invocation that already accepted host follow-up work. */
static int cli_publish_settlement(cli_state *state, cai_error *error) {
  cai_execution_record *record;
  cai_agent_execution *writer = NULL;
  cai_input_state queue;
  cai_input_entry entry;
  size_t i;
  int quiet, pending, rc;
  if (cai_cli_export_job_active(state->export_job) ||
      !state->settlement_pending || state->awaiting_turn ||
      state->batch_pending_turns > 0U || state->review_runtime != NULL ||
      state->shell.pid > 0 || state->goal_starts_pending > 0U)
    return CAI_OK;
  rc = cai_agent_runtime_is_quiescent(state->runtime, &quiet, error);
  if (rc != CAI_OK || !quiet)
    return rc;
  rc = cai_input_queue_get_state(state->input, &queue, error);
  for (i = 0U; rc == CAI_OK && i < queue.count; ++i) {
    rc = cai_input_queue_entry_at(state->input_admin, i, &entry, error);
    if (rc == CAI_OK && entry.ready && !entry.held && !entry.stopped)
      return CAI_OK;
  }
  if (rc != CAI_OK)
    return rc;
  pthread_mutex_lock(&state->bridge->lock);
  pending = state->bridge->head != NULL || state->bridge->stop_requested;
  pthread_mutex_unlock(&state->bridge->lock);
  if (pending)
    return CAI_OK;
  record = calloc(1U, sizeof(*record));
  if (record == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "allocate application settlement");
  record->kind = "application.settled";
  rc = cai_agent_runtime_execution_open(state->runtime, record, &writer, error);
  free(record);
  if (rc == CAI_OK)
    rc = cai_agent_execution_finish(writer, error);
  cai_agent_execution_close(writer);
  if (rc == CAI_OK)
    state->settlement_pending = 0;
  return rc;
}

static int cli_temp_stop(void *context) {
  cli_state *state = context;
  if (cai_app_bridge_cleanup_stop(state->bridge))
    state->host_stopped = 1;
  return state->host_stopped;
}

static int cli_app_run_state(cli_state *state, int argc, char **argv,
                             const cai_app_config *application,
                             cai_app_bridge *bridge,
                             const cai_config_context *startup,
                             const cai_attachment_config *server,
                             int lifetime_explicit, int frontend_startup,
                             const cai_cli_prepared *prepared) {
  cai_client_config client_config;
  cai_chatgpt_auth_config auth_config;
  cai_cli_pouch *auth_store = NULL;
  cai_error error;
  char auth_path[PATH_MAX];
  const char *auth_file;
  char *metadata_json;
  int parsed;
  int rc;
  int result;

  memset(state, 0, sizeof(*state));
  cai_cli_temp_init(&state->temporary);
  state->application = application;
  state->bridge = bridge;
  state->frontend_startup = frontend_startup;
  cai_cli_shell_init(&state->shell);
  result = 1;
  metadata_json = NULL;
  cai_error_init(&error);
  {
    cai_cli_options early;
    int valid = cai_cli_read_options(argc, argv, NULL, &early);
    if (valid < 0) {
      cai_cli_error(&error, CAI_ERR_INVALID, early.diagnostic);
      result = 2;
      goto cleanup;
    }
    if (valid <= 0 || cai_cli_options_utility(&early) ||
        (!frontend_startup && (early.review || early.non_interactive))) {
      cai_cli_error(&error, CAI_ERR_INVALID,
                    "controller startup accepts agent options, not utility or "
                    "terminal modes");
      result = 2;
      goto cleanup;
    }
    if (cai_app_bridge_stopping(bridge)) {
      cai_cli_error(&error, CAI_ERR_CANCELLED, "startup was stopped");
      goto cleanup;
    }
  }
  if (prepared != NULL) {
    state->configuration = prepared->configuration;
    state->options = prepared->options;
    state->normal_options = prepared->normal_options;
    state->borrowed_configuration = 1;
    if (prepared->temporary != NULL) {
      state->temporary = *prepared->temporary;
      state->borrowed_temporary = 1;
    }
    parsed = 1;
  } else
    parsed =
        cai_cli_configure_at(argc, argv, &state->options, &state->configuration,
                             application, startup, &error);
  if (parsed > 0 && state->options.headless && server == NULL) {
    cai_cli_error(&error, CAI_ERR_INVALID,
                  "--headless is an executable process mode; native hosts "
                  "must use cai_app_start_server");
    parsed = -1;
  }
  if (parsed > 0 &&
      cai_config_environment(state->configuration, &state->environment,
                             &error) != CAI_OK)
    parsed = -1;
  if (parsed > 0)
    state->options.environment = state->environment;
  if (parsed > 0 && prepared == NULL) {
    state->normal_options = state->options;
    if (state->options.temporary &&
        cai_cli_temp_open(&state->temporary, &state->options, &error) != CAI_OK)
      parsed = -1;
  }
  if (parsed <= 0) {
    if (parsed < 0 &&
        cai_cli_log_new(&state->log, PSLOG_LEVEL_WARN, 0,
                        application != NULL ? application->logger : NULL,
                        &error) == CAI_OK)
      cli_diagnostic(state, PSLOG_LEVEL_ERROR,
                     error.message != NULL ? error.message
                                           : state->options.diagnostic);
    result = parsed == 0 ? 0 : 2;
    if (parsed < 0 && error.code == CAI_OK)
      cai_cli_error(&error, CAI_ERR_INVALID, state->options.diagnostic);
    goto cleanup;
  }
  if (cai_app_bridge_stopping(bridge)) {
    cai_cli_error(&error, CAI_ERR_CANCELLED, "startup was stopped");
    goto cleanup;
  }
  if (frontend_startup) {
    state->batch_frontend = frontend_startup == CAI_CLI_OWNER_BATCH_FRONTEND;
    state->options.review = state->options.review_and_fix = 0;
    state->options.non_interactive = 0;
    state->options.out = NULL;
  }
  if (cai_cli_log_new_at(&state->log, PSLOG_LEVEL_TRACE, server != NULL,
                         application != NULL ? application->logger : NULL,
                         state->environment, state->options.directory,
                         &error) != CAI_OK)
    goto cleanup;
  result = 1;
  if (realpath(state->options.directory, state->workspace) == NULL) {
    cli_diagnostic(state, PSLOG_LEVEL_ERROR, "cannot resolve workspace");
    result = 2;
    goto cleanup;
  }
  if (server != NULL) {
    cai_attachment_config selected = *server;
    cai_cli_options identity = state->options;
    cai_cli_workspace_claims claims = {0};
    if (!lifetime_explicit)
      selected.persist = state->options.server_persist;
    identity.server_persist = selected.persist;
    identity.review = state->normal_options.review;
    rc = cai_cli_workspace_claims_build(&identity, state->configuration, 0,
                                        &claims, &error);
    if (rc == CAI_OK)
      rc = cai_cli_workspace_host_open(
          state->options.runtime_dir, state->workspace,
          state->options.workspace_name, claims.values, claims.count, &selected,
          application != NULL ? &application->input : NULL, state->bridge,
          cli_logger(state), &state->workspace_host, &error);
    cai_cli_workspace_claims_cleanup(&claims);
    if (rc != CAI_OK) {
      cli_print_error(state, "open workspace server", &error);
      goto cleanup;
    }
  }
  if (state->options.out != NULL) {
    if (cai_cli_workspace_path(state->output_path, PATH_MAX, state->workspace,
                               state->options.out, &error) != CAI_OK)
      goto cleanup;
    state->options.out = state->output_path;
  }
  if (server != NULL) {
    rc = cai_cli_log_file_at(state->log, state->options.log_dir, 0, &error);
    if (rc == CAI_OK)
      rc = cai_app_bridge_log(
          state->bridge, state->log->fd >= 0 ? state->log->path : NULL, &error);
    if (rc != CAI_OK) {
      cli_print_error(state, "open session log", &error);
      goto cleanup;
    }
  }
  if (state->options.temporary) {
    pslog_field field = pslog_str("workspace", state->temporary.workspace);
    cai_cli_log_message(state->log->logger, PSLOG_LEVEL_INFO,
                        "temp_workspace_opened", "diagnostic",
                        "Temporary workspace created", 27U, &field, 1U);
  }
  {
    cai_cli_options selected;
    const char *name = state->normal_options.sessions_store;
    rc = state->options.temporary
             ? (selected = state->options, CAI_OK)
             : cai_cli_store_options(state->configuration,
                                     &state->normal_options, name, &selected,
                                     &error);
    if (rc == CAI_OK)
      rc = cai_cli_pouch_open_options(&state->pouch, &selected,
                                      state->log->logger, &error);
  }
  if (rc != CAI_OK) {
    cli_print_error(state, "open lockd store", &error);
    goto cleanup;
  }
  state->store = state->pouch.store;
  rc = cli_host_process(state, &error);
  if (rc != CAI_OK || state->host_stopped) {
    result = rc == CAI_OK ? 0 : 1;
    goto cleanup;
  }
  if (!state->options.insecure) {
    if (state->options.force) {
      strcpy(state->workspace_approval_root, state->workspace);
      state->workspace_approval_pending = 1;
      rc = CAI_OK;
    } else
      rc = cai_cli_pouch_workspace_approval(
          &state->pouch, state->workspace, state->workspace_approval_root,
          sizeof(state->workspace_approval_root), &error);
    if (rc != CAI_OK) {
      cli_print_error(state, "read workspace approval", &error);
      goto cleanup;
    }
    if (state->workspace_approval_root[0] == '\0') {
      cai_cli_error(
          &error, CAI_ERR_INVALID,
          "Workspace is not approved. Use -y/--yes (or --force) to approve "
          "this directory and its subdirectories, or -k/--insecure to bypass "
          "approval for this invocation without saving it.");
      cli_print_error(state, "workspace approval", &error);
      goto cleanup;
    }
  }
  auth_file = state->options.auth_json;
  if (strcmp(state->options.provider, "chatgpt") == 0 ||
      (!state->options.review && state->options.mcp)) {
    auth_store =
        cli_named_store(state, state->normal_options.auth_store, 1, &error);
    if (auth_store == NULL) {
      cli_print_error(state, "open authentication store", &error);
      goto cleanup;
    }
    if (auth_file == NULL &&
        cai_environment_get(state->environment, "HOME") != NULL) {
      rc = snprintf(auth_path, sizeof(auth_path), "%s/.codex/auth.json",
                    cai_environment_get(state->environment, "HOME"));
      if (rc > 0 && (size_t)rc < sizeof(auth_path) &&
          access(auth_path, F_OK) == 0)
        auth_file = auth_path;
    }
    if (auth_file != NULL) {
      rc = cai_cli_pouch_seed_auth(auth_store, auth_file, &error);
      if (rc != CAI_OK) {
        if (strcmp(state->options.provider, "chatgpt") == 0) {
          cli_print_error(state, "import ChatGPT auth", &error);
          goto cleanup;
        }
        cai_cli_log_error(cli_logger(state), "ChatGPT apps auth import",
                          &error);
        cai_error_cleanup(&error);
        cai_error_init(&error);
      }
    }
  }
  rc = cli_prepare_clipboard(state, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "initialize clipboard", &error);
    goto cleanup;
  }
  {
    cai_input_controller_config admission = {0};
    if (state->workspace_host != NULL)
      cai_cli_workspace_host_admission(state->workspace_host, &admission);
    else if (application != NULL)
      admission = application->input;
    rc =
        cai_input_controller_open(&admission, &state->input_controller, &error);
    if (rc != CAI_OK) {
      cli_print_error(state, "initialize input controller", &error);
      goto cleanup;
    }
  }
  cai_client_config_init(&client_config);
  client_config.environment = state->environment;
  client_config.logger = state->log->logger;
  rc = cli_load_model_metadata(&state->options, &metadata_json, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "load model metadata", &error);
    goto cleanup;
  }
  client_config.model_metadata_json = metadata_json;
  if (auth_store != NULL) {
    cai_chatgpt_auth_config_init(&auth_config);
    auth_config.logger = state->log->logger;
    auth_config.storage = &auth_store->credentials;
    rc = cai_chatgpt_auth_open(&auth_config, &state->auth, &error);
    if (rc != CAI_OK) {
      if (strcmp(state->options.provider, "chatgpt") == 0) {
        char hint[640];
        cli_print_error(state, "open ChatGPT auth", &error);
        snprintf(hint, sizeof(hint), "run %s --login to authenticate",
                 state->options.invocation_name);
        cli_diagnostic(state, PSLOG_LEVEL_ERROR, hint);
        goto cleanup;
      }
      cai_error_cleanup(&error);
      cai_error_init(&error);
    }
  }
  if (state->auth != NULL) {
    if (pthread_mutex_init(&state->auth_lock, NULL) != 0) {
      cai_cli_error(&error, CAI_ERR_TRANSPORT, "initialize shared auth lock");
      cli_print_error(state, "open ChatGPT auth", &error);
      goto cleanup;
    }
    state->has_auth_lock = 1;
    state->shared_auth.access_token = cli_auth_token;
    state->shared_auth.refresh = cli_auth_refresh;
    state->shared_auth.account_id = cli_auth_account;
    state->shared_auth.impl = state;
  }
  if (strcmp(state->options.provider, "chatgpt") == 0) {
    client_config.chatgpt_auth = &state->shared_auth;
  } else if (strcmp(state->options.provider, "openrouter") == 0) {
    cai_client_config_use_openrouter(&client_config);
  } else if (strcmp(state->options.provider, "berget") == 0) {
    cai_client_config_use_berget(&client_config);
  } else if (strcmp(state->options.provider, "custom") == 0) {
    client_config.base_url = state->options.endpoint;
    client_config.api_key_env = state->options.api_key_env;
  }
  client_config.metadata_provider = state->options.provider;
  if (state->configuration != NULL &&
      state->configuration->apply_client(state->configuration, &client_config,
                                         &error) != CAI_OK) {
    cli_print_error(state, "apply client configuration", &error);
    goto cleanup;
  }
  if (state->options.endpoint != NULL)
    client_config.base_url = state->options.endpoint;
  if (state->options.api_key_env != NULL)
    client_config.api_key_env = state->options.api_key_env;
  if (metadata_json != NULL)
    client_config.model_metadata_json = metadata_json;
  rc = cai_client_open(&client_config, &state->client, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "open client", &error);
    goto cleanup;
  }
  /* Explicit catalog commands inherit the configured request deadline. They
   * are independent of the short, opportunistic quota-refresh timeout. */
  {
    pslog_field operation = pslog_str("operation", "model catalog");
    state->model_log =
        state->log->logger->with(state->log->logger, &operation, 1U);
    if (state->model_log == NULL) {
      cai_cli_error(&error, CAI_ERR_NOMEM, "create model catalog logger");
      cli_print_error(state, "open model catalog client", &error);
      goto cleanup;
    }
    client_config.logger = state->model_log;
  }
  rc = cai_client_open(&client_config, &state->model_client, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "open model catalog client", &error);
    goto cleanup;
  }
  if (state->auth != NULL && strcmp(state->options.provider, "chatgpt") == 0) {
    cai_error quota_error;
    cai_client_config quota_config;
    cai_error_init(&quota_error);
    cai_client_config_init(&quota_config);
    quota_config.environment = state->environment;
    quota_config.logger = state->log->logger;
    quota_config.chatgpt_auth = &state->shared_auth;
    quota_config.ca_bundle_path = client_config.ca_bundle_path;
    quota_config.ca_path = client_config.ca_path;
    quota_config.insecure_skip_verify = client_config.insecure_skip_verify;
    quota_config.http_2_disabled = client_config.http_2_disabled;
    quota_config.timeout_ms =
        client_config.timeout_ms > 0L ? client_config.timeout_ms : 10000L;
    if (cai_client_open(&quota_config, &state->quota_client, &quota_error) ==
        CAI_OK) {
      if (cai_cli_quota_open(&state->quota_refresh, cli_fetch_quota,
                             state->quota_client, &quota_error) == CAI_OK) {
      }
    }
    if (quota_error.code != CAI_OK)
      cai_cli_log_error(state->log->logger, "initialize quota updates",
                        &quota_error);
    cai_error_cleanup(&quota_error);
  }
  rc = cli_open_runtime(state, state->options.resume_id,
                        state->options.new_session, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "open agent session", &error);
    goto cleanup;
  }
  rc = cli_publish_status(state, state->runtime, &error);
  if (rc != CAI_OK)
    goto cleanup;
  if (cli_sandbox_notice(state) != 0) {
    cai_cli_error(&error, CAI_ERR_TRANSPORT, "render startup policy notice");
    goto cleanup;
  }
  rc = cli_host_process(state, &error);
  if (rc != CAI_OK)
    goto cleanup;
  if (state->exit_requested) {
    result = 0;
    goto cleanup;
  }
  cli_refresh_quota(state, 0);
  rc = cli_start_automation(state, &error);
  if (rc != CAI_OK) {
    cli_print_error(state, "start work", &error);
    goto cleanup;
  }
  while (!state->exit_requested) {
    rc = cli_pump(state, 0L, &error);
    if (rc == CAI_OK)
      rc = cli_advance_automation(state, &error);
    if (rc == CAI_OK)
      rc = cli_sync_status(state, &error);
    if (rc == CAI_OK)
      rc = cli_publish_settlement(state, &error);
    if (rc == CAI_OK && !state->exit_requested)
      rc = cli_pump(state, -1L, &error);
    if (rc != CAI_OK) {
      cli_print_error(state, "controller event", &error);
      goto cleanup;
    }
  }
  result = 0;
cleanup:
  cai_cli_workspace_host_stop(state->workspace_host);
  if (error.code == CAI_ERR_CANCELLED && cai_app_bridge_stopping(bridge)) {
    cai_error_cleanup(&error);
    cai_error_init(&error);
    (void)cli_host_process(state, &error);
    result = error.code == CAI_OK ? 0 : 1;
  }
  {
    cai_error decisions;
    cai_error_init(&decisions);
    /* Settle handed-off admission/edit decisions, including Enter followed by
     * Alt-Q in one terminal batch. Never claim or start work during shutdown.
     */
    if (state->input != NULL && state->input_controller != NULL) {
      if (cai_input_controller_pump(state->input_controller, 32U, &decisions) !=
          CAI_OK) {
        cli_print_error(state, "save pending input decisions", &decisions);
        result = 1;
      }
      cai_error_cleanup(&decisions);
      cai_error_init(&decisions);
    }
    /* A discard and Alt-Q/EOF/host STOP can share one input batch, without an
     * idle callback. Flush only decisions already made, before detaching IDs;
     * still-queued or actively edited inbox messages remain recoverable. */
    if (cai_cli_messages_flush_decisions(state->messages, state->input,
                                         &decisions) != CAI_OK) {
      cli_print_error(state, "save incoming message decisions", &decisions);
      cai_app_bridge_fail(bridge, &decisions);
      result = 1;
    }
    cai_error_cleanup(&decisions);
  }
  if (state->host_stopped) {
    cai_app_bridge_outcome(bridge, CAI_APP_STOPPED);
  }
  if (bridge != NULL) {
    cai_error snapshot;
    cai_error_init(&snapshot);
    if (cli_publish_status(state, state->runtime, &snapshot) != CAI_OK) {
      cai_app_bridge_error(bridge, &snapshot);
      result = 1;
    }
    cai_error_cleanup(&snapshot);
    cai_app_bridge_error(bridge, &error);
  }
  {
    cai_cli_export_result *pending = NULL;
    cai_cli_export_job_close(state->export_job, &pending);
    state->export_job = NULL;
    if (pending != NULL && cli_export_result(state, pending, &error) != CAI_OK)
      result = 1;
  }
  cai_cli_shell_close(&state->shell);
  if (state->runtime != NULL) {
    cai_error close_error;
    cai_error_init(&close_error);
    if (cli_close_runtime(state, &close_error) != CAI_OK) {
      cli_print_error(state, "close conversation", &close_error);
      result = 1;
    }
    cai_error_cleanup(&close_error);
  } else {
    cai_cli_messages_close(state->messages);
    state->messages = NULL;
  }
  cai_cli_mcp_close(&state->mcp);
  if (state->owns_plugins)
    cai_plugins_close(state->plugins);
#ifdef CAI_APP_LUA_TOOLS
  cai_lua_tools_close(state->lua_tools);
#endif
  cai_cli_quota_close(&state->quota_refresh);
  if (state->client != NULL)
    state->client->close(state->client);
  if (state->model_client != NULL)
    state->model_client->close(state->model_client);
  if (state->model_log != NULL)
    state->model_log->destroy(state->model_log);
  if (state->quota_client != NULL)
    state->quota_client->close(state->quota_client);
  if (state->auth != NULL)
    state->auth->close(state->auth);
  if (state->has_auth_lock)
    pthread_mutex_destroy(&state->auth_lock);
  cai_cli_pouch_close(&state->pouch);
  cai_cli_pouch_close(&state->auth_pouch);
  cai_cli_pouch_close(&state->messaging_pouch);
  cai_cli_documents_close(&state->documents);
  cai_app_bridge_bind_input(state->bridge, NULL);
  cai_input_controller_close(state->input_controller);
  if (!state->borrowed_temporary &&
      (state->temporary.root[0] != '\0' || state->temporary.parent_fd >= 0)) {
    cai_error cleanup_error;
    int kept = 0;
    cai_error_init(&cleanup_error);
    rc = cai_cli_temp_close(&state->temporary, state->options.force, 0,
                            bridge != NULL ? bridge->fd : -1, cli_temp_stop,
                            state, &kept, &cleanup_error);
    if (kept) {
      pslog_field field = pslog_str("workspace", state->temporary.workspace);
      cai_cli_log_message(state->log != NULL ? state->log->logger : NULL,
                          PSLOG_LEVEL_INFO, "temp_workspace_kept", "diagnostic",
                          "Temporary workspace kept", 24U, &field, 1U);
    }
    if (rc != CAI_OK) {
      cai_cli_log_error(state->log != NULL ? state->log->logger : NULL,
                        "clean temporary workspace", &cleanup_error);
      cai_app_bridge_error(bridge, &cleanup_error);
      result = 1;
    }
    cai_error_cleanup(&cleanup_error);
  }
  free(state->sessions);
  free(state->normal_reasoning_override);
  free(state->high_reasoning_override);
  free(state->initial_review_instructions);
  free(metadata_json);
  if (!state->borrowed_configuration)
    cai_config_close(state->configuration);
  cai_environment_close(state->environment);
  cai_cli_workspace_host_close(state->workspace_host);
  cai_error_cleanup(&error);
  if (cai_cli_log_release(state->log) != 0)
    result = 1;
  return result;
}

/* Path/configuration aggregates live on the heap. This owner outlives every
 * runtime, lease service and host callback, without owning frontend state. */
static int
cli_app_receive(int argc, char **argv, const cai_app_config *application,
                cai_app_bridge *bridge, const cai_config_context *startup,
                const cai_attachment_config *server, int lifetime_explicit,
                int frontend_startup, const cai_cli_prepared *prepared) {
  cli_state *state = calloc(1U, sizeof(*state));
  int code;
  if (state == NULL) {
    if (application != NULL && application->logger != NULL)
      application->logger->error(application->logger,
                                 "cannot allocate application state", NULL, 0U);
    else
      fputs("cannot allocate application state\n", stderr);
    return 1;
  }
  code =
      cli_app_run_state(state, argc, argv, application, bridge, startup, server,
                        lifetime_explicit, frontend_startup, prepared);
  free(state);
  return code;
}
int cai_cli_app_controller_run(int argc, char **argv,
                               const cai_app_config *application,
                               cai_app_bridge *bridge,
                               const cai_config_context *startup,
                               const cai_attachment_config *server,
                               int lifetime_explicit, int frontend_startup,
                               const cai_cli_prepared *prepared) {
  if (startup == NULL) {
    cai_error error;
    cai_error_init(&error);
    cai_cli_error(&error, CAI_ERR_INVALID,
                  "controller startup context is required");
    cai_app_bridge_error(bridge, &error);
    cai_error_cleanup(&error);
    return 2;
  }
  return cli_app_receive(argc, argv, application, bridge, startup, server,
                         lifetime_explicit, frontend_startup, prepared);
}
