/** @file
 * Closed-session handover adapter. Reads final control metadata and
 * streams goals, retained provider history and execution chunks from one
 * native journal view. Instruction preparation shares the live Smith policy;
 * no provider client, tool runtime or model-context restoration is involved.
 */
#define LONEJSON_WITH_CURL
#include "markdown_export.h"
#include "identity.h"
#include "instructions.h"
#include <cai/execution.h>
#include <cai/handover.h>
#include <cai/smith.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Model/settings are control values. Goal text is deliberately not captured:
 * it flows from the native metadata producer into the handover's text sink. */
typedef struct export_text {
  char bytes[8192];
  size_t length;
  int present;
} export_text;
typedef struct export_value {
  lonejson_json_value value;
} export_value;
static const lonejson_field value_fields[] = {
    LONEJSON_FIELD_JSON_VALUE(export_value, value, "value")};
LONEJSON_MAP_DEFINE(value_map, export_value, value_fields);
enum field {
  NONE,
  MODEL,
  REQUEST,
  EFFORT,
  PRESET,
  VERSION,
  STATUS,
  OBJECTIVE,
  BUDGET,
  USED,
  ELAPSED,
  STARTED,
  BASELINE,
  METADATA_VERSION
};
typedef struct markdown {
  cai_cli_journal_view *view;
  cai_sink *sink, *goal_sink;
  cai_error *error;
  cai_handover_config handover;
  export_text text[6];
  long long number[6];
  int have_number[6], has_goal, field, rc;
  char digits[32];
  size_t digit_count;
  lonejson *json;
  lonejson_curl_parse parser;
  lonejson_path_value_visitor visitor;
  export_value value;
  cai_agent_instructions *instructions;
  cai_cli_instruction_files files;
  char approval_root[PATH_MAX];
  cai_execution_decoder *execution;
  cai_execution_record record;
  int execution_started, selected, execution_error, include_subagents;
} markdown;

static int fail(markdown *m, int code, const char *message) {
  if (m->rc == CAI_OK)
    m->rc = cai_cli_error(m->error, code, message);
  return m->rc;
}
static int field_at(const lonejson_value_path *path) {
  static const char *const names[] = {"",
                                      "model",
                                      "request_model",
                                      "reasoning_effort",
                                      "preset_name",
                                      "preset_prompt_version",
                                      "goal_status",
                                      "goal_objective",
                                      "goal_token_budget",
                                      "goal_tokens_used",
                                      "goal_elapsed_seconds",
                                      "goal_active_started_at",
                                      "goal_token_usage_baseline",
                                      "version"};
  size_t i;
  if (path->segment_count != 1U)
    return NONE;
  for (i = 1U; i < sizeof(names) / sizeof(names[0]); i++)
    if (path->segments[0].len == strlen(names[i]) &&
        memcmp(path->segments[0].data, names[i], strlen(names[i])) == 0)
      return (int)i;
  return NONE;
}
static lonejson_status string_begin(void *context,
                                    const lonejson_value_path *path,
                                    lonejson_error *error) {
  markdown *m = context;
  (void)error;
  m->field = field_at(path);
  if (m->field == OBJECTIVE)
    m->has_goal = 1;
  else if (m->field >= MODEL && m->field <= STATUS) {
    export_text *text = &m->text[m->field - MODEL];
    text->present = 1;
    text->length = 0U;
    text->bytes[0] = '\0';
  } else if (m->field >= BUDGET)
    fail(m, CAI_ERR_PROTOCOL, "numeric handover metadata must not be a string");
  return m->rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status string_chunk(void *context,
                                    const lonejson_value_path *path,
                                    const char *bytes, size_t length,
                                    lonejson_error *error) {
  markdown *m = context;
  int field = m->field;
  (void)path;
  (void)error;
  if (field == OBJECTIVE && m->goal_sink != NULL) {
    m->rc = cai_sink_write(m->goal_sink, bytes, length, m->error);
  } else if (field >= MODEL && field <= STATUS) {
    export_text *text = &m->text[field - MODEL];
    if (memchr(bytes, '\0', length) != NULL)
      fail(m, CAI_ERR_PROTOCOL, "handover setting contains NUL");
    else if (length >= sizeof(text->bytes) - text->length)
      fail(m, CAI_ERR_LIMIT,
           "handover setting exceeds its control-value limit");
    else {
      memcpy(text->bytes + text->length, bytes, length);
      text->length += length;
      text->bytes[text->length] = '\0';
    }
  }
  return m->rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status number_begin(void *context,
                                    const lonejson_value_path *path,
                                    lonejson_error *error) {
  markdown *m = context;
  (void)error;
  m->field = field_at(path);
  m->digit_count = 0U;
  return LONEJSON_STATUS_OK;
}
static lonejson_status number_chunk(void *context,
                                    const lonejson_value_path *path,
                                    const char *bytes, size_t length,
                                    lonejson_error *error) {
  markdown *m = context;
  int field = m->field;
  (void)path;
  (void)error;
  if (field == NONE)
    return LONEJSON_STATUS_OK;
  if (field < BUDGET || length >= sizeof(m->digits) - m->digit_count)
    fail(m, CAI_ERR_PROTOCOL, "invalid numeric handover metadata");
  else {
    memcpy(m->digits + m->digit_count, bytes, length);
    m->digit_count += length;
  }
  return m->rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status number_end(void *context,
                                  const lonejson_value_path *path,
                                  lonejson_error *error) {
  markdown *m = context;
  int field = m->field;
  unsigned long long n = 0U;
  size_t i = 0U;
  int negative;
  (void)path;
  (void)error;
  if (field < BUDGET)
    return LONEJSON_STATUS_OK;
  negative = m->digit_count != 0U && m->digits[0] == '-';
  if (negative) {
    if (field != ELAPSED && field != STARTED) {
      fail(m, CAI_ERR_PROTOCOL, "negative handover counter");
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    i = 1U;
  }
  for (; i < m->digit_count; i++) {
    unsigned char ch = (unsigned char)m->digits[i];
    if (ch < '0' || ch > '9' ||
        n > ((unsigned long long)LLONG_MAX + (negative ? 1U : 0U) -
             (ch - '0')) /
                10U) {
      fail(m, CAI_ERR_PROTOCOL, "invalid handover counter");
      return LONEJSON_STATUS_CALLBACK_FAILED;
    }
    n = n * 10U + ch - '0';
  }
  m->number[field - BUDGET] = negative ? 0LL : (long long)n;
  m->have_number[field - BUDGET] = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status structural(void *context,
                                  const lonejson_value_path *path,
                                  lonejson_error *error) {
  markdown *m = context;
  (void)error;
  if (field_at(path) != NONE)
    fail(m, CAI_ERR_PROTOCOL, "invalid handover metadata value type");
  return m->rc == CAI_OK ? LONEJSON_STATUS_OK : LONEJSON_STATUS_CALLBACK_FAILED;
}
static lonejson_status boolean(void *context, const lonejson_value_path *path,
                               int value, lonejson_error *error) {
  (void)value;
  return structural(context, path, error);
}
static int parser_bytes(void *context, const void *bytes, size_t length,
                        cai_error *error) {
  markdown *m = context;
  const char *at = bytes;
  (void)error;
  while (length != 0U && m->rc == CAI_OK) {
    char chunk[4096];
    size_t take = length < sizeof(chunk) ? length : sizeof(chunk);
    memcpy(chunk, at, take);
    if (lonejson_curl_write_callback(chunk, 1U, take, &m->parser) != take)
      fail(m, CAI_ERR_PROTOCOL, "parse handover metadata");
    length -= take;
    at += take;
  }
  return m->rc;
}
/** Inspect scalar settings or forward the goal directly into a text producer.
 * Repeated passes retain the view's captured head, not a materialized object.
 */
static int metadata(markdown *m, cai_sink *goal_sink) {
  lonejson_error error;
  cai_sink sink;
  m->goal_sink = goal_sink;
  memset(&sink, 0, sizeof(sink));
  sink.callbacks.write = parser_bytes;
  sink.callbacks.context = m;
  lonejson_json_value_init(m->json, &m->value.value);
  if (lonejson_json_value_set_parse_path_visitor(
          &m->value.value, &m->visitor, m, &error) != LONEJSON_STATUS_OK ||
      lonejson_curl_parse_init(&m->parser, m->json, &value_map, &m->value) !=
          LONEJSON_STATUS_OK)
    fail(m, CAI_ERR_NOMEM, "initialize handover metadata parser");
  if (m->rc == CAI_OK)
    parser_bytes(m, "{\"value\":", 9U, m->error);
  if (m->rc == CAI_OK)
    m->rc = cai_cli_journal_view_metadata(m->view, &sink, m->error);
  if (m->rc == CAI_OK)
    parser_bytes(m, "}", 1U, m->error);
  if (m->rc == CAI_OK &&
      lonejson_curl_parse_finish(&m->parser) != LONEJSON_STATUS_OK)
    fail(m, CAI_ERR_PROTOCOL, "incomplete handover metadata");
  lonejson_curl_parse_cleanup(&m->parser);
  lonejson_cleanup(&value_map, &m->value);
  memset(&m->parser, 0, sizeof(m->parser));
  memset(&m->value, 0, sizeof(m->value));
  m->goal_sink = NULL;
  return m->rc;
}
static int objective(void *context, cai_sink *sink, cai_error *error) {
  markdown *m = context;
  (void)error;
  return metadata(m, sink);
}
static int instructions(void *context, cai_sink *sink, cai_error *error) {
  markdown *m = context;
  return cai_agent_instructions_write(m->instructions, m->handover.active_model,
                                      sink, error);
}
/** Decode one authenticated bounded execution record at a time. A malformed
 * record has the same omission notice as the existing runtime exporter. */
static int execution(void *context, const cai_cli_transcript_event *event,
                     cai_error *error) {
  markdown *m = context;
  int rc;
  if (event->kind == CAI_CLI_TRANSCRIPT_BEGIN) {
    m->selected =
        strcmp(event->type, "execution") == 0 ||
        (m->include_subagents && strcmp(event->type, "subagent") == 0);
    m->execution_error = 0;
    if (m->selected)
      return cai_execution_decoder_reset(m->execution, error);
  } else if (m->selected && event->kind == CAI_CLI_TRANSCRIPT_DATA) {
    if (!m->execution_error)
      m->execution_error = cai_execution_decoder_feed(m->execution, event->data,
                                                      event->length, error);
  } else if (m->selected && event->kind == CAI_CLI_TRANSCRIPT_END) {
    if (!m->execution_started) {
      static const char heading[] =
          "\n\n## Execution history\n\n*Recorded "
          "activity; never replayed as model input.*\n";
      rc = cai_sink_write(m->sink, heading, sizeof(heading) - 1U, error);
      if (rc != CAI_OK)
        return rc;
      m->execution_started = 1;
    }
    rc = m->execution_error != 0
             ? m->execution_error
             : cai_execution_decoder_finish(m->execution, &m->record, error);
    if (rc != CAI_OK) {
      char notice[192];
      int count = snprintf(notice, sizeof(notice),
                           "\n\n> Execution record %llu could not be decoded; "
                           "its original data remains in the JSONL export.\n",
                           event->sequence);
      cai_error_cleanup(error);
      cai_error_init(error);
      return cai_sink_write(m->sink, notice, (size_t)count, error);
    }
    rc = cai_execution_record_write_markdown(&m->record, 0, m->sink, error);
    cai_execution_record_cleanup(&m->record);
    return rc;
  }
  return CAI_OK;
}

int cai_cli_markdown_export(cai_cli_journal_view *view, cai_cli_pouch *pouch,
                            const cai_cli_session *session,
                            const char *workspace,
                            const cai_handover_config *active,
                            const cai_cli_options *options, cai_sink *sink,
                            cai_error *error) {
  markdown *m;
  cai_agent_preset preset;
  cai_agent_preset_config config;
  cai_skill_config skills;
  lonejson_config json_config = lonejson_default_config();
  lonejson_error local;
  struct stat info;
  long long now = (long long)time(NULL), delta;
  int rc;
  if (view == NULL || pouch == NULL || session == NULL || workspace == NULL ||
      sink == NULL || sink->callbacks.write == NULL)
    return cai_cli_error(
        error, CAI_ERR_INVALID,
        "journal view, store, session, workspace and sink required");
  m = calloc(1U, sizeof(*m));
  if (m == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate Markdown projection");
  m->view = view;
  m->sink = sink;
  m->error = error;
  cai_execution_record_init(&m->record);
  if (active != NULL) {
    m->handover = *active;
    m->include_subagents = 1;
    goto write;
  }
  json_config.clear_destination_by_default = 0;
  json_config.json_value_max_total_bytes =
      json_config.json_value_max_string_bytes = 0U;
  m->json = lonejson_new(&json_config, &local);
  if (m->json == NULL)
    fail(m, CAI_ERR_NOMEM, "allocate handover metadata parser");
  m->visitor = lonejson_default_path_value_visitor();
  m->visitor.string_begin = string_begin;
  m->visitor.string_chunk = string_chunk;
  m->visitor.number_begin = number_begin;
  m->visitor.number_chunk = number_chunk;
  m->visitor.number_end = number_end;
  m->visitor.object_begin = m->visitor.array_begin = structural;
  m->visitor.boolean_value = boolean;
  if (m->rc == CAI_OK)
    metadata(m, NULL);
  cai_agent_preset_from_smith(&preset);
  if (m->rc == CAI_OK &&
      ((m->text[PRESET - MODEL].present || m->text[VERSION - MODEL].present) &&
       (strcmp(m->text[PRESET - MODEL].bytes, preset.name) != 0 ||
        strcmp(m->text[VERSION - MODEL].bytes, preset.prompt_version) != 0)))
    fail(m, CAI_ERR_INVALID,
         "session commit preset does not match runtime preset");
  if (m->rc == CAI_OK && m->number[METADATA_VERSION - BUDGET] != 1)
    fail(m, CAI_ERR_PROTOCOL, "invalid handover metadata version");
  if (m->rc == CAI_OK &&
      ((!m->has_goal &&
        (m->have_number[BUDGET - BUDGET] || m->number[USED - BUDGET] != 0 ||
         m->number[BASELINE - BUDGET] != 0)) ||
       (m->has_goal && m->have_number[BUDGET - BUDGET] &&
        m->number[BUDGET - BUDGET] <= 0)))
    fail(m, CAI_ERR_INVALID, "session state has invalid goal token accounting");
  if (m->rc == CAI_OK && m->text[STATUS - MODEL].present) {
    const char *status = m->text[STATUS - MODEL].bytes;
    if (strcmp(status, "active") && strcmp(status, "complete") &&
        strcmp(status, "paused") && strcmp(status, "blocked") &&
        strcmp(status, "usage_limited") && strcmp(status, "budget_limited"))
      fail(m, CAI_ERR_INVALID, "session state has invalid goal status");
  }
  cai_agent_preset_config_init(&config);
  config.workspace_directory =
      stat(session->workspace, &info) == 0 && S_ISDIR(info.st_mode)
          ? session->workspace
          : workspace;
  config.agent_config_directory = pouch->config_directory;
  config.agent_identity = pouch->program_name;
  if (options != NULL) {
    config.environment = options->environment;
    config.agent_config_directory = options->config_dir;
    config.global_agents_md_path = options->agents_md;
    config.agent_identity =
        options->identity != NULL ? options->identity : pouch->program_name;
    config.developer_instructions_extension = options->developer_instructions;
    config.codex_compat_agents_md = options->codex_agents_md;
    config.disable_terminal = !options->terminal;
    cai_skill_config_init(&skills);
    skills.skills_directory = options->skills_dir;
    config.skills = &skills;
#define ENABLED(member, bit)                                                   \
  if (!options->member)                                                        \
  preset.tool_capabilities &= ~(bit)
    ENABLED(read_file, CAI_AGENT_PRESET_TOOL_READ_FILE);
    ENABLED(list_files, CAI_AGENT_PRESET_TOOL_LIST_FILES);
    ENABLED(apply_patch, CAI_AGENT_PRESET_TOOL_APPLY_PATCH);
    ENABLED(view_image, CAI_AGENT_PRESET_TOOL_VIEW_IMAGE);
    ENABLED(goals, CAI_AGENT_PRESET_TOOL_GOAL);
    ENABLED(skills, CAI_AGENT_PRESET_TOOL_SKILLS);
    ENABLED(mcp, CAI_AGENT_PRESET_TOOL_MCP);
    ENABLED(chrome, CAI_AGENT_PRESET_TOOL_CHROME);
    ENABLED(web_search, CAI_AGENT_PRESET_TOOL_WEB_SEARCH);
    ENABLED(image_generation, CAI_AGENT_PRESET_TOOL_IMAGE_GENERATION);
#undef ENABLED
    if (m->rc == CAI_OK)
      m->rc = cai_cli_instruction_files_open(options, &m->files, error);
    preset.developer_instructions = m->files.agent;
    preset.developer_instructions_literal = m->files.agent != NULL;
    config.developer_instructions_override = m->files.initial;
    if (options->insecure)
      config.workspace_instructions_root = config.workspace_directory;
    else if (m->rc == CAI_OK) {
      m->rc = cai_cli_pouch_workspace_approval(
          pouch, config.workspace_directory, m->approval_root,
          sizeof(m->approval_root), error);
      if (m->approval_root[0] != '\0')
        config.workspace_instructions_root = m->approval_root;
    }
  }
  if (m->rc == CAI_OK)
    m->rc = cai_agent_preset_prepare_instructions(
        &preset, &config, pouch->logger, &m->instructions, NULL, error);
  cai_handover_config_init(&m->handover);
  m->handover.preset_name = preset.name;
  m->handover.preset_prompt_version = preset.prompt_version;
  m->handover.active_model =
      m->text[REQUEST - MODEL].present ? m->text[REQUEST - MODEL].bytes
      : m->text[MODEL - MODEL].present ? m->text[MODEL - MODEL].bytes
                                       : preset.default_model;
  m->handover.recorded_model =
      m->text[MODEL - MODEL].present ? m->text[MODEL - MODEL].bytes : NULL;
  m->handover.reasoning_effort = m->text[EFFORT - MODEL].present
                                     ? m->text[EFFORT - MODEL].bytes
                                     : preset.default_reasoning_effort;
  m->handover.runtime_state = "idle";
  m->handover.session_id = session->id;
  m->handover.workspace_directory = config.workspace_directory;
  m->handover.session_scope = pouch->program_name;
  m->handover.goal_status = m->text[STATUS - MODEL].present
                                ? m->text[STATUS - MODEL].bytes
                                : "active";
  m->handover.goal_has_token_budget = m->have_number[BUDGET - BUDGET];
  m->handover.goal_token_budget = m->number[BUDGET - BUDGET];
  m->handover.goal_tokens_used = m->number[USED - BUDGET];
  m->handover.goal_elapsed_seconds = m->number[ELAPSED - BUDGET];
  if (m->number[STARTED - BUDGET] > 0 && now > m->number[STARTED - BUDGET]) {
    delta = now - m->number[STARTED - BUDGET];
    m->handover.goal_elapsed_seconds =
        m->handover.goal_elapsed_seconds > LLONG_MAX - delta
            ? LLONG_MAX
            : m->handover.goal_elapsed_seconds + delta;
  }
  if (m->has_goal) {
    m->handover.goal_objective.write = objective;
    m->handover.goal_objective.context = m;
  }
  m->handover.developer_instructions.write = instructions;
  m->handover.developer_instructions.context = m;
write:
  m->handover.history.write = cai_cli_journal_view_history;
  m->handover.history.context = view;
  if (m->rc == CAI_OK)
    m->rc = cai_handover_write(&m->handover, sink, error);
  if (m->rc == CAI_OK)
    m->rc = cai_execution_decoder_open(&m->execution, error);
  if (m->rc == CAI_OK)
    m->rc = cai_cli_journal_view_events(view, execution, m, error);
  rc = m->rc;
  cai_execution_record_cleanup(&m->record);
  cai_execution_decoder_close(m->execution);
  cai_agent_instructions_close(m->instructions);
  cai_cli_instruction_files_close(&m->files);
  lonejson_free(m->json);
  free(m);
  return rc;
}
