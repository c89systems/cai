#ifndef CAI_APP_H
#define CAI_APP_H

#include <cai/attachment.h>
#include <cai/config.h>
#include <cai/input_controller.h>
#include <cai/protocol.h>
#include <cai/tools/todo.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Explicit application setting, using the same sections/keys/values as INI.
 * List settings repeat their key. Strings are copied by cai_app_open. */
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

typedef enum cai_app_outcome {
  CAI_APP_NOT_RUN = 0,
  CAI_APP_RUNNING,
  CAI_APP_COMPLETED,
  CAI_APP_STOPPED,
  CAI_APP_CANCELLED,
  CAI_APP_FAILED
} cai_app_outcome;

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
 * Conversation history requires Pouch collections. Remote lockd stores remain
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
  /** Optional raw interactive banner, written verbatim before terminal
   * renderers start. Defaults to NULL; the host supplies all newlines and ANSI
   * escapes. */
  const char *banner;
  /** Optional interactive introduction rendered through libmdf before session
   * replay. Defaults to NULL. Neither startup text is shown by help, utilities,
   * reviews or non-interactive runs. Both strings are copied by open. */
  const char *introduction_markdown;
  /** Independent libmdf introduction margins, in columns. Both default to zero
   * and must be nonnegative, leaving at least three content columns at runtime.
   * These do not change conversation rendering. */
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

typedef struct cai_app cai_app;
/** Application receiver. Interactive run keeps the common
 * controller and host callbacks on its calling thread, with a default-stack
 * terminal worker attached through the public Unix socket client. It captures
 * directory/environment views without installing process environment or
 * locale. Its controller installs no signal state. Hosts give the adapter
 * exclusive use of stdin/stdout; its
 * temporary terminal/signal modes are restored before return, including
 * failures. Temporary runs use an ephemeral owner, joined before a plain
 * default-no workspace cleanup question; host STOP interrupts that question.
 * Batch mode still uses the combined receiver; maintenance utilities have a
 * separate dispatcher. Both retain the process-resource wrapper for configured
 * logging/environment behavior and must be serialized with other
 * process-resource users. Repeated runs are supported; one run per instance may
 * be active. start selects an independent terminal-free owner. Borrowed loggers
 * can receive calls from native workers and must be thread-safe. */
struct cai_app {
  /** argv contains only CAI options (exclude the program name). Empty is valid.
   * Returns CAI status for invocation/restoration errors. On CAI_OK, exit_code
   * receives the application's CLI status: 0 success, 1 work/runtime failure,
   * 2 invalid options/configuration. CLI diagnostics use the configured logger.
   * Arguments are borrowed only during run. This method never calls exit. */
  int (*run)(cai_app *self, int argc, const char *const *argv, int *exit_code,
             cai_error *error);
  /** Number of immutable parser/help option entries; no startup effects. */
  size_t (*option_count)(const cai_app *self);
  /** Borrow one zero-based option entry, or NULL when index is out of range. */
  const cai_app_option *(*option_at)(const cai_app *self, size_t index);
  /** Write help with the configured identity and invocation_name to a borrowed
   * sink. No configuration files, logger, banner, auth or terminal are
   * initialized.
   */
  int (*write_help)(const cai_app *self, cai_sink *sink, cai_error *error);
  /** Thread-safe while self remains alive. Requires a ready runtime (STOP is
   * also accepted during startup and utilities). text is required only for
   * prompt, steering and goal replacement, and is copied before return.
   * Interactive prompts enter softline's approval/edit queue; batch prompts
   * enter the runtime FIFO. CAI_OK means accepted, with a nonzero id; inspect
   * the acknowledgement for application errors. Ordinary queue capacity is 64;
   * a full queue rejects explicitly. STOP has one reserved allocation-free
   * command, accepted even during startup or at capacity. Repeating pending
   * STOP returns its original ID. Accepted ordinary commands that cannot
   * apply before shutdown receive negative acknowledgements.
   * STOP requests orderly shutdown, cancelling active work. Run/join completion
   * waits for workers; the host process stays alive. */
  int (*control)(cai_app *self, cai_app_control_type type, const char *text,
                 unsigned long long *id, cai_error *error);
  /** Thread-safe copied status and final run result; no file/log parsing.
   * After run returns, running is false and outcome/exit_code/error describe
   * that invocation. A later run resets the result. */
  int (*get_status)(const cai_app *self, cai_app_status *out, cai_error *error);
  /** Start a terminal-free controller on its owner thread. See cai_app_start.
   */
  int (*start)(cai_app *self, const cai_config_context *context, int argc,
               const char *const *argv, cai_error *error);
  /** Explicitly blocking; never call from an owner callback. */
  int (*join)(cai_app *self, int *exit_code, cai_error *error);
  void (*close)(cai_app *self);
  void *impl;
};

/** Seed CAI defaults, automatic XDG paths and no host overrides. */
void cai_app_config_init(cai_app_config *config);
/** Copy configuration and validate settings without reading files or running
 * the application. Logger remains borrowed. On failure *out is NULL. */
int cai_app_open(const cai_app_config *config, cai_app **out, cai_error *error);
/** Run synchronously with options only, excluding argv[0]. See cai_app.run. */
int cai_app_run(cai_app *self, int argc, const char *const *argv,
                int *exit_code, cai_error *error);
/** Asynchronously start a controller, without acquiring stdin/stdout, creating
 * Softline/libmdf objects, changing cwd/environment/locale/signals or starting
 * a listener. Context and arguments are captured/copied before return; NULL
 * context members capture cwd/environment once. Explicit directory must be
 * absolute. Accepts agent startup options, not utility/review/batch modes.
 * Typed prompt/steer/goal/cancel/STOP controls use the same backend queue and
 * runtime as the terminal receiver. Slash/! input requires a frontend and is
 * rejected by this control surface. Borrowed owner callbacks and logger remain
 * configured by open. Runtime events/replay stream to those callbacks.
 * CAI_OK means the owner started, not that restoration succeeded. get_status
 * exposes STARTING (running, !runtime_ready), readiness and startup failure.
 * The controller stays alive while idle until STOP. Call join before close or
 * another run/start. Serialize start/join/run/close on a given receiver. */
int cai_app_start(cai_app *self, const cai_config_context *context, int argc,
                  const char *const *argv, cai_error *error);
/** Start the same terminal-free owner with a workspace socket listener.
 * Uses the canonical selected workspace and effective paths.runtime_dir.
 * An explicit lifetime is copied and overrides startup configuration. NULL
 * uses [server] persist (default false; --headless defaults true) and the
 * normal first-attach grace period. Last detach stops a nonpersistent backend,
 * including active work. A persistent owner runs until explicit shutdown or
 * native STOP. No fork, signal handler, terminal, or second runtime/store is
 * created. Socket identity, role, lifetime, model input and queue operations
 * are bound. Transcript subscriptions and queue payloads use the typed
 * app_client.h binding, including command execution records and frontend
 * delegation to the originating attachment. The owner creates no renderer.
 * The executable owns --headless process
 * logging/signals; native hosts call this API instead of passing that
 * process-mode flag to the synchronous terminal run.
 * Start success is asynchronous; observe status for readiness or failure.
 */
int cai_app_start_server(cai_app *self, const cai_config_context *context,
                         int argc, const char *const *argv,
                         const cai_attachment_config *lifetime,
                         cai_error *error);
/** Wait for a started controller, returning its CLI-style exit code. Does not
 * request STOP. Never call from its callbacks. Query status for error detail.
 * Once joined, the receiver may be started again. */
int cai_app_join(cai_app *self, int *exit_code, cai_error *error);
/** Return the option count without loading configuration or starting UI. */
size_t cai_app_option_count(const cai_app *self);
/** Borrow a zero-based help entry; NULL self/out-of-range returns NULL. */
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index);
/** Write identity-aware help to a borrowed sink without startup side effects.
 */
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error);
/** Thread-safe command acceptance, not completion. See cai_app.control for
 * runtime requirements, acknowledgement, copied text and STOP behavior. */
int cai_app_control(cai_app *self, cai_app_control_type type, const char *text,
                    unsigned long long *id, cai_error *error);
/** Copy the most recent safe status. Initialize out first; success replaces
 * its owned values and failure leaves them unchanged. Thread-safe. */
int cai_app_get_status(const cai_app *self, cai_app_status *out,
                       cai_error *error);
/** Borrow a level-triggered status wakeup descriptor, or -1 for NULL. Watch
 * POLLIN and call observe_status; never close/read/write this descriptor.
 * Notifications coalesce into the newest snapshot, with bounded storage and
 * no idle timer. Multiple consumers can use get_status, but exactly one host
 * loop owns draining this notification channel. Valid until app close. */
int cai_app_status_fd(const cai_app *self);
/** Atomically copy the newest owned status and drain its wakeup. Publication
 * racing this operation remains readable afterward. Initialize out first.
 * Allocation failure leaves out and the notification unchanged. Thread-safe.
 */
int cai_app_observe_status(const cai_app *self, cai_app_status *out,
                           cai_error *error);
/** Thread-safe attachment to the active conversation's backend queue. Does
 * not open a runtime/store or wait for a snapshot. Use connection_fd/poll and
 * request from input_controller.h. Requires a ready controller or interactive
 * receiver; batch/review modes have no frontend queue. attachment is a fixed
 * authenticated identity chosen by the host. NULL input.authorize is trusted
 * in-process operation. The current terminal-free owner accepts model input
 * and queue operations; slash/! presentation is not wired there and is
 * rejected. A host must enforce authority separately for its trusted control()
 * surface. Close every returned connection; it may outlive receiver
 * stop/join/close, retaining observations/allocation, never runtime or journal
 * authority. Do not race receiver close with a new connect/resume call. */
int cai_app_connect_input(cai_app *self, uint64_t attachment,
                          cai_input_connection **out, cai_error *error);
/** Resume a verified retained producer rather than reserving another one.
 * See input_controller_resume for epoch/identity and asynchronous failure
 * semantics. A changed session or unavailable receipt must be reconciled;
 * never automatically reoffer an uncertain prompt under a new producer. */
int cai_app_resume_input(cai_app *self, uint64_t attachment,
                         unsigned long long conversation,
                         unsigned long long producer,
                         cai_input_connection **out, cai_error *error);
/** Close after run returns or an asynchronous controller is joined, outside
 * its callbacks. Calls made before then are ignored. NULL is accepted.
 * Must not race other receiver calls. */
void cai_app_close(cai_app *self);

/** Independent shared kanban access for HTTP hosts and other embedders.
 * This opens the same encrypted Pouch store as CAI without running a terminal
 * application or changing environment/cwd. NULL paths use normal CAI defaults.
 * The logger is borrowed through the final receiver close; if absent, a logger
 * is created from LOG_ with warn as its default level. */
typedef struct cai_app_kanban_config {
  const char *program_name;
  const char *namespace_prefix;
  const char *todo_namespace;
  const char *pouch_key_file;
  /** Absolute state directory containing pouch.key; NULL derives XDG/name. */
  const char *state_directory;
  /** Pouch or remote lockd endpoint; NULL selects state_directory/pouch. */
  const char *lockd;
  /** Optional client PEM for remote lockd mutual TLS. */
  const char *lockd_client_pem;
  /** Initial selection, or NULL for "default". Not shared with other receivers.
   */
  const char *default_board;
  /** Maximum returned cards/boards; zero selects 100. */
  size_t max_result_items;
  /** Disable Pouch zlib compression. Zero enables it; must match the root. */
  int disable_compression;
  /** Borrowed native logger, valid through the final receiver close. */
  struct pslog_logger *logger;
} cai_app_kanban_config;
/** Initialize automatic CAI paths, the default board and bounded listing. */
void cai_app_kanban_config_init(cai_app_kanban_config *config);
/** Returns an owned core receiver with Pouch-backed transaction callbacks.
 * Its retained references own the store; the caller closes the receiver. */
int cai_app_kanban_open(const cai_app_kanban_config *config, cai_todo **out,
                        cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
