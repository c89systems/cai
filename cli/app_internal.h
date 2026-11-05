#ifndef CAI_APP_INTERNAL_H
#define CAI_APP_INTERNAL_H
#include <cai/app.h>
#include <pthread.h>
struct cai_cli_prepared;
typedef struct cai_app_command {
  struct cai_app_command *next;
  cai_app_control_type type;
  char *text;
  unsigned long long id;
} cai_app_command;
/** Bounded local startup/lifetime projection, without model context/settings.
 */
typedef struct cai_app_frontend_status {
  int running, ready, has_server, exit_code, code;
  cai_attachment_phase phase;
  unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES];
  char reason[192];
} cai_app_frontend_status;
typedef struct cai_app_bridge {
  pthread_mutex_t lock;
  int fd, status_fd, frontend_fd;
  int accepting;
  int failed;
  int stop_requested;
  int frontend_done;
  int frontend_running, frontend_ready, frontend_phase;
  int frontend_owner_ended, frontend_owner_code;
  size_t count; /* Ordinary commands; the reserved STOP does not consume it. */
  unsigned long long next_id;
  /** Reserved shutdown command: no capacity or allocation dependency. */
  cai_app_command stop;
  cai_app_command *head, *tail;
  cai_app_status status;
  const cai_app_config *config;
  cai_input_controller *input;
} cai_app_bridge;
int cai_app_bridge_open(cai_app_bridge **out, const cai_app_config *config,
                        cai_error *error);
void cai_app_bridge_close(cai_app_bridge *bridge);
void cai_app_bridge_begin(cai_app_bridge *bridge);
void cai_app_bridge_end(cai_app_bridge *bridge, int code,
                        const cai_error *error);
int cai_app_bridge_control(cai_app_bridge *bridge, cai_app_control_type type,
                           const char *text, unsigned long long *id,
                           cai_error *error);
int cai_app_bridge_status(cai_app_bridge *bridge, cai_app_status *out,
                          cai_error *error);
int cai_app_bridge_observe(cai_app_bridge *bridge, cai_app_status *out,
                           cai_error *error);
/** Independent wake channel for run()'s terminal adapter. It shares the owned
 * status snapshot, never consumes a host observer's notification. Open before
 * starting the adapter; close only after its thread and controller have ended.
 */
int cai_app_bridge_frontend_open(cai_app_bridge *, cai_error *);
int cai_app_bridge_frontend_observe(cai_app_bridge *, cai_app_frontend_status *,
                                    cai_error *);
void cai_app_bridge_frontend_close(cai_app_bridge *);
/** End run()'s owned frontend lifetime without inventing a host control ID or
 * acknowledgement. Wakes the controller and its bounded startup cancellation.
 */
void cai_app_bridge_frontend_done(cai_app_bridge *);
int cai_app_bridge_frontend_ended(cai_app_bridge *);
/** End the owned runtime before joining its frontend, retaining the host run
 * lifetime for post-terminal temporary-workspace confirmation. */
void cai_app_bridge_frontend_owner_end(cai_app_bridge *, int code);
/** Drain shutdown controls on the calling owner. STOP remains available while
 * awaiting cleanup input; other commands receive an explicit rejection. */
int cai_app_bridge_cleanup_stop(void *);
void cai_app_bridge_bind_input(cai_app_bridge *bridge,
                               cai_input_controller *input);
void cai_app_bridge_server(
    cai_app_bridge *bridge, const cai_attachment_state *server,
    const unsigned char instance[CAI_WORKSPACE_INSTANCE_BYTES]);
int cai_app_bridge_log(cai_app_bridge *, const char *path, cai_error *);
int cai_app_bridge_input(cai_app_bridge *bridge, uint64_t attachment,
                         unsigned long long conversation,
                         unsigned long long producer,
                         cai_input_connection **out, cai_error *error);
int cai_app_bridge_publish(cai_app_bridge *bridge, cai_agent_runtime *runtime,
                           cai_error *error);
void cai_app_bridge_error(cai_app_bridge *bridge, const cai_error *error);
void cai_app_bridge_fail(cai_app_bridge *bridge, const cai_error *error);
cai_app_command *cai_app_bridge_take(cai_app_bridge *bridge);
/* Handoff owns wakeups temporarily. Leave commands queued for normal dispatch
 * after terminal restoration; drain notifications to avoid spinning while
 * non-STOP commands are deferred. A later submission signals again. */
int cai_app_bridge_shell_stop(cai_app_bridge *bridge);
int cai_app_bridge_stopping(cai_app_bridge *bridge);
void cai_app_bridge_ack(cai_app_bridge *bridge, cai_app_command *command,
                        int status, const cai_error *error);
void cai_app_bridge_outcome(cai_app_bridge *bridge, cai_app_outcome outcome);
int cai_cli_app_run(int argc, char **argv, const cai_app_config *application,
                    cai_app_bridge *bridge);
int cai_cli_app_controller_run(int argc, char **argv,
                               const cai_app_config *application,
                               cai_app_bridge *bridge,
                               const cai_config_context *context,
                               const cai_attachment_config *server,
                               int lifetime_explicit, int frontend_startup,
                               const struct cai_cli_prepared *prepared);
/** Private composition entry: resolve/open the owner, but leave invocation
 * actions to its attaching frontend's accepted queue. Public headless owners
 * retain their explicit startup automation. No new public configuration flag.
 */
int cai_app_start_frontend_owner(cai_app *, const cai_config_context *, int,
                                 const char *const *, cai_error *);
int cai_cli_app_main(int argc, char **argv, const cai_app_config *application);
size_t cai_cli_help_count(void);
const cai_app_option *cai_cli_help_at(size_t index);
int cai_cli_help_write(const char *name, const char *program,
                       const char *environment_prefix, cai_sink *sink,
                       cai_error *error);
int cai_cli_help_print(const char *name, const char *program,
                       const char *environment_prefix);
int cai_cli_app_schema(cai_config **out, cai_error *error);
#endif
