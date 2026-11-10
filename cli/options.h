#ifndef CAI_CLI_OPTIONS_H
#define CAI_CLI_OPTIONS_H

#include "workspace_fields.h"
#include <cai/workspace_claims.h>
#include <stddef.h>
#include <stdio.h>

struct cai_environment;

#define CAI_CLI_STATUS_DEFAULT_FIELDS                                          \
  "model:ctx:dir:branch:weekly:crd:yolo:goal"

/** Explicit invocation controls that may change an already-running owner.
 * Inherited defaults must not overwrite that owner's live settings. */
typedef enum cai_cli_control_option {
  CAI_CLI_CONTROL_TIER = 1U << 0,
  CAI_CLI_CONTROL_YOLO = 1U << 1,
  CAI_CLI_CONTROL_YIELD = 1U << 2,
  CAI_CLI_CONTROL_REASONING_TOOL = 1U << 3,
  CAI_CLI_CONTROL_SHELL_CONTEXT = 1U << 4,
  CAI_CLI_CONTROL_HISTORY = 1U << 5,
  CAI_CLI_CONTROL_RETENTION = 1U << 6,
  CAI_CLI_CONTROL_AUTOQUEUE = 1U << 7,
  CAI_CLI_CONTROL_NORMAL_EFFORT = 1U << 8,
  CAI_CLI_CONTROL_HIGH_EFFORT = 1U << 9,
  CAI_CLI_CONTROL_TODO_LAYOUT = 1U << 10
} cai_cli_control_option;

typedef struct cai_cli_options {
  /* Borrow the application-owned effective configuration environment. */
  struct cai_environment *environment;
  /** Executable-owned receipt for a successfully rendered terminal failure. */
  int *diagnostic_presented;
  const char *program_name;
  const char *invocation_name;
  char environment_prefix[65];
  char default_api_key_env[80];
  const char *namespace_prefix, *queue_prefix;
  const char *sessions_namespace, *auth_namespace, *scopes_namespace;
  const char *todo_namespace, *messages_namespace;
  const char *sessions_store, *auth_store, *messaging_store;
  const char *context_store, *context_namespace, *context_export;
  int context;
  const char *home_dir, *runtime_dir;
  const char *workspace_name;
  /** Explicit existing-workspace selector; never starts a server. */
  const char *server;
  const char *state_dir, *cache_dir, *data_dir;
  const char *clipboard_dir, *log_dir, *pouch_key_file;
  int help, version, headless, terminate_headless, all;
  int no_banner;
  int server_persist;
  int list_themes;
  int prefer_native_pager;
  const char *theme;
  long long theme_offset, subagent_theme_offset, review_theme_offset;
  const char *status_fields;
  int dump_config;
  const char *dump_config_file;
  const char *dump_developer_instructions;
  const char *provider;
  const char *config_file;
  int config_explicit;
  unsigned char owner_explicit[CAI_CLI_OWNER_COUNT];
  unsigned int control_explicit;
  /** Executable composition borrows one bounded startup requirement inventory.
   */
  const cai_workspace_claim *workspace_requirements;
  size_t workspace_requirement_count;
  int no_config;
  int model_configured;
  const char *chrome_socket;
  const char *chrome_manifest;
  int chrome;
  int chrome_explicit;
  int web_search;
  int mcp;
  int messaging;
  const char *message_queue;
  const char *send_queue, *message;
  int enable_auto_enqueue;
  long message_ack_timeout_ms;
  long message_delay_seconds;
  long replay_turns;
  int prompt_history_limit;
  long conversation_retention_days;
  int todo;
  const char *board;
  int board_explicit;
  int searxng;
  int revgeo;
  int read_file, list_files, apply_patch, view_image, goals, skills;
  int subagent_steering;
  int capacity_retry;
  const char *searxng_url;
  const char *revgeo_url;
  int chrome_setup;
  int chrome_remove;
  const char *endpoint;
  const char *api_key_env;
  const char *model_metadata;
  const char *directory;
  const char *auth_json;
  const char *model;
  const char *reasoning_effort;
  int reasoning_tool;
  const char *normal_reasoning_effort, *high_reasoning_effort;
  const char *reasoning_summary;
  const char *service_tier;
  int model_explicit;
  int provider_explicit;
  int reasoning_effort_explicit;
  int reasoning_summary_explicit;
  const char *review_model;
  const char *review_reasoning_effort;
  const char *review_reasoning_summary;
  const char *agents_md;
  const char *config_dir;
  const char *skills_dir;
  int plugins;
  const char *tools_dir, *workspace_tools_dir;
  const char *install_skill;
  int force, temporary, insecure;
  int utc;
  const char *identity;
  const char *developer_instructions;
  const char *developer_instructions_file;
  const char *reviewer_instructions_file;
  const char *developer_instructions_override_file;
  const char *goal;
  const char *base;
  const char *out;
  const char *output_type;
  const char *tool_output;
  const char *todo_layout;
  const char *resume_id;
  const char *delete_id;
  const char *export_id;
  const char *export_out;
  const char *export_dir;
  const char *import_file;
  const char *lockd;
  const char *lockd_client_pem;
  int disable_compression;
  char *const *argv;
  int argc;
  size_t instruction_count;
  int login;
  /** Current-directory and whole-store listings are exclusive utility modes. */
  int list;
  int list_all;
  int resume_list;
  int delete_list;
  int new_session;
  int non_interactive;
  int review;
  int review_and_fix;
  int review_uncommitted;
  int image_generation;
  int terminal;
  int foreground;
  long yield_time_ms;
  int yolo;
  int shell_context;
  size_t shell_context_max_lines, shell_context_max_bytes;
  int review_subagent;
  int review_and_fix_tool;
  int codex_agents_md;
  int verbosity;
  char diagnostic[512];
} cai_cli_options;

void cai_cli_options_init(cai_cli_options *options);
/** Record an explicitly supplied member through the shared owner inventory. */
void cai_cli_options_mark_owner(cai_cli_options *, size_t member_offset);
/* Returns 1 to start, 0 for help/version, -1 for an invalid command line. */
int cai_cli_parse_options(int argc, char *const *argv,
                          cai_cli_options *options);
int cai_cli_read_options(int argc, char *const *argv,
                         const cai_cli_options *defaults,
                         cai_cli_options *options);
int cai_cli_validate_options(cai_cli_options *options);
/** Maintenance selections never open an agent owner or terminal frontend.
 * Help/version/invalid argv and raw Chrome entry points are classified by the
 * invocation dispatcher before there is a valid options object. */
int cai_cli_options_utility(const cai_cli_options *);
const char *cai_cli_instruction_at(const cai_cli_options *options,
                                   size_t index);

#endif
