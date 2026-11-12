#ifndef CAI_APP_TYPES_H
#define CAI_APP_TYPES_H

#include <cai/app_view.h>
#include <cai/attachment.h>
#include <cai/config.h>
#include <cai/input_controller.h>
#include <cai/protocol.h>
#include <cai/tools/todo.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Explicit application setting, using the same sections/keys/values as INI.
 * List settings repeat their key. App/server constructors copy the strings. */
typedef struct cai_app_setting {
  const char *section;
  const char *key;
  const char *value;
} cai_app_setting;

/** Host commands are copied into a bounded FIFO and applied on the run thread.
 * Queueing a prompt starts a new turn; steering targets the active turn.
 * Goal replacement starts pursuit with no token budget. */
typedef enum cai_app_control_type {
  CAI_APP_QUEUE_PROMPT = 1,
  CAI_APP_STEER,
  CAI_APP_CANCEL_TURN,
  CAI_APP_STOP,
  CAI_APP_REPLACE_GOAL,
  CAI_APP_PAUSE_GOAL,
  CAI_APP_RESUME_GOAL,
  CAI_APP_CLEAR_GOAL
} cai_app_control_type;

/** Owned snapshot. Initialize before first use; cleanup after the last use.
 * Getters replace an initialized snapshot atomically on success. Strings and
 * errors remain valid independently of the app, later queries and later runs.
 * Values reflect the most recent owner-thread safe boundary, not live workers.
 * runtime_ready distinguishes startup/utilities from an open agent runtime. */
typedef struct cai_app_status {
  int running;
  int runtime_ready;
  /** Socket owner state, present only for start_server. The final snapshot
   * retains STOPPING and its reason after the endpoint has closed. */
  int has_server;
  cai_attachment_state server;
  unsigned char server_instance[CAI_WORKSPACE_INSTANCE_BYTES];
  cai_app_outcome outcome;
  int exit_code;
  cai_agent_run_state state;
  const char *session_id;
  /** Full path of CAI's owned session/startup log, if present. Explicit host
   * loggers retain their own output policy and have no inferred path. */
  const char *log_path;
  cai_agent_runtime_settings effective;
  cai_agent_runtime_settings pending;
  cai_agent_goal_snapshot goal;
  cai_agent_runtime_metrics metrics;
  /** Most recent run error, including provider detail when supplied. May
   * describe a recovered interactive diagnostic; check outcome/exit_code. */
  cai_error error;
  /** Most recently processed host command; IDs restart at one each run. */
  unsigned long long control_id;
  int control_status;
  cai_error control_error;
} cai_app_status;

/** Initialize an empty owned status before its first query. */
void cai_app_status_init(cai_app_status *status);
/** Release owned status strings/errors and reset it. NULL is accepted. */
void cai_app_status_cleanup(cai_app_status *status);

/** Argument shape accepted by the shared CAI command-line parser. */
typedef enum cai_app_option_type {
  CAI_APP_FLAG = 0,
  CAI_APP_STRING = 1,
  CAI_APP_INTEGER = 2,
  CAI_APP_OPTIONAL_STRING = 3,
  CAI_APP_COUNTER = 4
} cai_app_option_type;

/** Immutable help entry. long_flag includes --; short_flag is zero if absent.
 * negatable advertises a corresponding --no-NAME spelling. */
typedef struct cai_app_option {
  const char *long_flag;
  char short_flag;
  cai_app_option_type type;
  const char *argument;
  const char *description;
  int repeatable;
  int negatable;
} cai_app_option;

/** Initialize before use. NULL paths derive XDG locations from program_name.
 * settings supplies explicit overrides for the complete CAI INI surface,
 * including providers, models, instructions, tools, MCP and storage. Order:
 * CAI defaults < INI/environment < settings < forwarded arguments.
 * [env] retains its normal config_env_has_precedence policy.
 * Runtime defaults match cai; startup text is empty unless the host sets it.
 * Session history requires Pouch collections. Remote lockd stores remain
 * available for separately selected authentication, messaging and kanban.
 * ui.prompt_history_limit defaults to 200 recall entries; storage pruning is
 * deferred. storage.conversation_retention_days defaults to 30 days since the
 * last journal append; 0 disables expiry. SDK maintenance removes whole
 * collections, not individual journal entries. */
#define CAI_APP_DEFAULT_PROGRAM_NAME "cai"
typedef struct cai_app_config {
  /** Application identity: lowercase letter followed by lowercase letters,
   * digits, '_' or '-', up to 32 bytes. NULL selects "cai". This seeds all
   * application defaults; INI files and argv cannot change the identity. */
  const char *program_name;
  /** Optional usage label, e.g. "vectis -a smith --". */
  const char *invocation_name;
  /** NULL derives uppercase(program_name) + '_'. Provider-standard API key
   * variables and pslog's LOG_ remain independent. */
  const char *environment_prefix;
  /** NULL derives program_name. Individual namespaces may be overridden in
   * [storage]; queue_name in [messaging] overrides the complete queue name. */
  const char *namespace_prefix;
  const char *queue_prefix;
  /** Optional unified application home. Unset individual roots derive as
   * home/config, home/state, home/cache, home/data and home/run. Individual
   * path settings take precedence. NULL retains the normal XDG defaults.
   * --home and the application-prefixed HOME variable also select this root
   * before config-file discovery. The process HOME is never changed. */
  const char *home_directory;
  /** Independent root/child defaults. NULL derives roots from home_directory
   * or the XDG roots and program_name, key from state, and
   * clipboard/logs from cache.
   * Explicit INI/environment, settings and argv override these defaults. */
  const char *state_directory;
  const char *cache_directory;
  const char *data_directory;
  /** Runtime files; NULL derives home/run, otherwise XDG_RUNTIME_DIR/name,
   * falling back to cache_directory when no runtime directory is available. */
  const char *runtime_directory;
  const char *clipboard_directory;
  const char *log_directory;
  const char *pouch_key_file;
  /** Optional raw interactive banner, written verbatim before
   * attachment/replay. NULL or empty prints nothing; no welcome is generated.
   * The host supplies all newlines and ANSI escapes. --no-banner suppresses all
   * startup text. */
  const char *banner;
  /** Optional interactive introduction rendered through libmdf before session
   * replay. Defaults to NULL. Neither startup text is shown by help, utilities,
   * reviews or non-interactive runs. Both strings are copied by open. */
  const char *introduction_markdown;
  /** Independent libmdf introduction margins, in columns. Both default to zero
   * and must be nonnegative, leaving at least three content columns at runtime.
   * These do not change session rendering. */
  int introduction_margin_left;
  int introduction_margin_right;
  const char *config_file;
  const char *config_directory;
  int no_config;
  const cai_app_setting *settings;
  size_t setting_count;
  /** Optional already-initialized, thread-safe logger. Borrowed; it and its
   * output must outlive the app and other store contexts sharing its SDK
   * client. CAI neither closes nor reconfigures it and creates no default log
   * files. Its output must be compatible with terminal ownership during a run.
   */
  struct pslog_logger *logger;
  /** Optional native tool registration hook, using the same contract as
   * cai_agent_runtime_config.register_agent_tools. Called on the run thread
   * whenever an agent runtime opens, after built-in tools (including messaging)
   * are registered and before workers start. Use cai_agent_register_* here.
   * The agent is borrowed only for this call; context and registered handler
   * contexts remain host-owned and must outlive each run. Handlers execute on
   * worker threads and must not call an owning-thread-only scripting VM.
   * Errors abort runtime startup. Not inherited by subagents and rejected by
   * isolated review. Help and utility invocations do not call this hook. */
  int (*register_agent_tools)(void *context, cai_agent *agent,
                              cai_error *error);
  void *register_agent_tools_context;
  /** Optional host catalog, retained by each opened runtime. Borrowed for run;
   * executor contexts must outlive run. NULL enables application-managed file
   * packages from configured tool roots. --no-plugins disables either source.
   * Host VM executors marshal calls to their owner thread themselves. */
  struct cai_plugins *plugins;
  /** Optional gate for attachment-bound queue mutations, evaluated on the
   * owner at admission after receipt lookup. Bind IDs to authenticated
   * connections; never trust an ID supplied in an untrusted request body.
   * Callback/context follow input_controller.h's lifetime/reentry contract.
   * Trusted native control() calls are separate from attachment admission. */
  cai_input_controller_config input;
  /** Optional owner-thread observers, alongside normal rendering and logging.
   * Events are borrowed only for the call. Return CAI_OK, or an error to abort
   * the run. Observers may call control/get_status, but must not reenter run,
   * close the app, or access the runtime. Contexts outlive each run. Replay
   * observations never execute tools or trigger inference. Console replay
   * limits do not filter these observations. */
  cai_agent_runtime_event_fn event_callback;
  void *event_context;
  cai_agent_session_event_fn replay_callback;
  void *replay_context;
  /** Owner-thread acknowledgement for every accepted command, including those
   * rejected or abandoned during shutdown. error is borrowed for the call;
   * acceptance is distinct from successful application. May call get_status
   * or queue another control. Do not reenter run or close the app here, even
   * when the final status is no longer running. NULL is valid; status retains
   * the last result. */
  void (*control_callback)(void *context, unsigned long long id, int status,
                           const cai_error *error);
  void *control_context;
} cai_app_config;

/** Initialize shared application/server configuration defaults. */
void cai_app_config_init(cai_app_config *config);

#ifdef __cplusplus
}
#endif
#endif
