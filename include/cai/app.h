#ifndef CAI_APP_H
#define CAI_APP_H

#include <cai/app_types.h>
#include <cai/attachment.h>
#include <cai/config.h>
#include <cai/input_controller.h>
#include <cai/protocol.h>
#include <cai/tools/todo.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_app cai_app;
/** Application receiver. A locally owned run keeps the common
 * controller and host callbacks on its calling thread, with a default-stack
 * terminal or batch worker attached through the public Unix socket client. It
 * captures directory/environment views without installing process environment
 * or locale. Its controller installs no signal state. Hosts give the adapter
 * exclusive use of stdin/stdout; its
 * temporary terminal/signal modes are restored before return, including
 * failures. Temporary runs use an ephemeral owner, joined before a plain
 * default-no workspace cleanup question; host STOP interrupts that question.
 * Redirected input uses bounded lines and live transcript presentation through
 * the same owner. Explicit --server attaches to an existing owner without
 * creating a runtime or invoking this instance's runtime/tool callbacks; use
 * the typed client APIs for remote runtime/status observations. Host STOP
 * detaches that client without stopping its remote server.
 * Maintenance utilities use captured environment/cwd views without changing
 * host process state. Different instances may run independently; their explicit
 * stdout/input channels and shared destinations retain normal host ownership
 * and conflict rules. Repeated runs are supported; one run per instance may
 * be active. start selects an independent terminal-free owner. Borrowed loggers
 * can receive calls from native workers and must be thread-safe. */
struct cai_app {
  /** argv contains only CAI options (exclude the program name). Empty is valid.
   * Returns CAI status for invocation/lifecycle errors. On CAI_OK, exit_code
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
