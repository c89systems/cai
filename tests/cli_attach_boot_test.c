/** Exercises executable startup without a UI, runtime or storage client.
 * Real native probes verify a winner after a deterministic first-launch race;
 * tiny forked children supply readiness/failure observations only. */
#define _GNU_SOURCE
#include "../cli/attach.h"
#include "../cli/bootstrap.h"
#include <assert.h>
#include <cai/workspace.h>
#include <cai/workspace_server.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ms(void) {
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
int __wrap_lc_client_open(void *, void *, void *);
int __wrap_lc_client_open(void *config, void *out, void *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
static int ignore(void *context, const cai_wire_event *event,
                  cai_error *error) {
  (void)context;
  (void)event;
  (void)error;
  return CAI_OK;
}
static int no_launch(const void *context, int fd, int argc,
                     const char *const *argv) {
  (void)context;
  (void)fd;
  (void)argc;
  (void)argv;
  abort();
}
static int child(const void *context, int fd, int argc,
                 const char *const *argv) {
  cai_cli_bootstrap_message *message = calloc(1U, sizeof(*message));
  cai_wire_peer *peer;
  int ready;
  (void)context;
  assert(argc == 2 && message != NULL && strcmp(argv[0], "--no-persist") == 0);
  ready = strcmp(argv[1], "ready") == 0;
  assert(cai_wire_peer_open(fd, 16384U, ignore, NULL, &peer, NULL) == CAI_OK);
  if (ready) {
    message->ready = 1;
    memset(message->instance, 19, sizeof(message->instance));
    strcpy(message->session, "fixture-session");
    assert(cai_cli_bootstrap_write(peer, message, NULL) == CAI_OK);
    message->ready = 0;
  } else {
    message->exit_code = 1;
    message->code =
        strcmp(argv[1], "conflict") == 0 ? CAI_ERR_LIMIT : CAI_ERR_INVALID;
    strcpy(message->message, "fixture startup failure");
  }
  assert(cai_cli_bootstrap_write(peer, message, NULL) == CAI_OK);
  assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
  assert(cai_wire_peer_pending(peer) == 0U);
  cai_wire_peer_close(peer);
  free(message);
  _exit(ready ? 0 : 1);
}
static cai_workspace_server *owner(const char *runtime, const char *workspace,
                                   const char *name) {
  cai_workspace_server_config config;
  cai_workspace_server *server;
  cai_workspace_server_config_init(&config);
  config.runtime_directory = runtime;
  config.workspace = workspace;
  config.name = name;
  config.lifetime.persist = 1;
  assert(cai_workspace_server_open(&config, &server, NULL) == CAI_OK);
  /* A STARTING identity is sufficient for lookup, without acquiring control. */
  return server;
}
static cai_cli_attach_boot *open_boot(cai_cli_options *options,
                                      cai_cli_spawn_entry_fn entry,
                                      const char *const *arguments) {
  cai_cli_attach_boot *boot;
  assert(cai_cli_attach_boot_open(options, entry, NULL, 1, arguments, &boot,
                                  NULL) == CAI_OK);
  return boot;
}
static int drive(cai_cli_attach_boot *boot, cai_workspace_server *server,
                 cai_app_client_config *target, int *ready, cai_error *error) {
  const cai_cli_frontend_boot *adapter = cai_cli_attach_boot_adapter(boot);
  uint64_t until = now_ms() + 5000U;
  int rc;
  *ready = 0;
  do {
    struct pollfd fds[CAI_WORKSPACE_SERVER_FDS + 1U];
    uint64_t deadline, now;
    size_t count = 0U;
    if (server != NULL) {
      assert(cai_workspace_server_pump(server, now_ms(), NULL) == CAI_OK);
      assert(cai_workspace_server_prepare(server, fds, CAI_WORKSPACE_SERVER_FDS,
                                          &count, &deadline, NULL) == CAI_OK);
    }
    rc = adapter->pump(adapter->context, now_ms(), target, ready, error);
    if (rc != CAI_OK || *ready)
      return rc;
    assert(adapter->prepare(adapter->context, &fds[count], &deadline, NULL) ==
           CAI_OK);
    now = now_ms();
    assert(now < until);
    if (deadline == 0U || deadline > until)
      deadline = until;
    assert(poll(fds, count + 1U, deadline > now ? (int)(deadline - now) : 0) >=
               0 ||
           errno == EINTR);
  } while (1);
}
static void no_attachment(cai_workspace_server *server) {
  cai_attachment_state state;
  assert(cai_workspace_server_get_state(server, &state, NULL) == CAI_OK);
  assert(state.count == 0U && state.controller == 0U);
}
static void cases(const char *runtime, const char *workspace,
                  const char *other) {
  cai_cli_options options;
  cai_cli_attach_boot *boot;
  cai_workspace_server *server;
  cai_app_client_config target;
  const cai_cli_frontend_boot *adapter;
  const char *const conflict[] = {"conflict"}, *const invalid[] = {"invalid"},
                    *const ready_child[] = {"ready"};
  cai_error error;
  int ready, rc;
  memset(&options, 0, sizeof(options));
  options.directory = workspace;
  options.runtime_dir = runtime;
  cai_error_init(&error);

  server = owner(runtime, other, "named-fixture");
  options.server = "named-fixture";
  boot = open_boot(&options, no_launch, conflict);
  assert(drive(boot, server, &target, &ready, &error) == CAI_OK);
  assert(ready == CAI_CLI_FRONTEND_EXISTING &&
         strcmp(target.workspace, other) == 0 &&
         memcmp(target.expected_instance, cai_workspace_server_instance(server),
                16U) == 0);
  {
    struct pollfd fd;
    uint64_t deadline;
    adapter = cai_cli_attach_boot_adapter(boot);
    assert(adapter->prepare(adapter->context, &fd, &deadline, NULL) == CAI_OK);
    assert(fd.fd == -1 && deadline == 0U);
  }
  no_attachment(server);
  cai_cli_attach_boot_close(boot);
  cai_workspace_server_close(server);

  options.server = "missing-fixture";
  boot = open_boot(&options, no_launch, conflict);
  assert(drive(boot, NULL, &target, &ready, &error) == CAI_ERR_INVALID);
  assert(!ready && strstr(error.message, "no running server"));
  cai_error_cleanup(&error);
  cai_cli_attach_boot_close(boot);
  options.server = NULL;

  /* Both launches observed absence. Before reading the losing child's EXIT,
   * another native owner appears. The loser must attach to that identity and
   * retain its own startup intents, without launching a second writer. */
  boot = open_boot(&options, child, conflict);
  adapter = cai_cli_attach_boot_adapter(boot);
  assert(adapter->pump(adapter->context, now_ms(), &target, &ready, NULL) ==
             CAI_OK &&
         !ready);
  assert(adapter->pump(adapter->context, now_ms(), &target, &ready, NULL) ==
             CAI_OK &&
         !ready);
  server = owner(runtime, workspace, NULL);
  assert(drive(boot, server, &target, &ready, &error) == CAI_OK);
  assert(ready == CAI_CLI_FRONTEND_EXISTING &&
         strcmp(target.workspace, workspace) == 0 &&
         memcmp(target.expected_instance, cai_workspace_server_instance(server),
                16U) == 0);
  no_attachment(server);
  cai_cli_attach_boot_close(boot);
  cai_workspace_server_close(server);

  /* A claim failure with no verifiable winner remains the original failure. */
  boot = open_boot(&options, child, conflict);
  assert(drive(boot, NULL, &target, &ready, &error) == CAI_ERR_LIMIT);
  assert(!ready && strcmp(error.message, "fixture startup failure") == 0);
  cai_error_cleanup(&error);
  cai_cli_attach_boot_close(boot);

  /* A different startup failure must not be hidden by a concurrent owner. */
  boot = open_boot(&options, child, invalid);
  adapter = cai_cli_attach_boot_adapter(boot);
  assert(adapter->pump(adapter->context, now_ms(), &target, &ready, NULL) ==
         CAI_OK);
  assert(adapter->pump(adapter->context, now_ms(), &target, &ready, NULL) ==
         CAI_OK);
  server = owner(runtime, workspace, NULL);
  assert(drive(boot, server, &target, &ready, &error) == CAI_ERR_INVALID);
  assert(!ready && strcmp(error.message, "fixture startup failure") == 0);
  cai_error_cleanup(&error);
  cai_cli_attach_boot_close(boot);
  cai_workspace_server_close(server);

  boot = open_boot(&options, child, ready_child);
  assert(drive(boot, NULL, &target, &ready, &error) == CAI_OK);
  assert(ready == CAI_CLI_FRONTEND_STARTED &&
         target.expected_instance[0] == 19);
  adapter = cai_cli_attach_boot_adapter(boot);
  /* Retain the exact child watch until its EXIT is reaped. */
  for (;;) {
    struct pollfd fd;
    uint64_t deadline, now = now_ms();
    assert(adapter->prepare(adapter->context, &fd, &deadline, NULL) == CAI_OK);
    if (fd.fd == -1 && deadline == 0U)
      break;
    assert(poll(&fd, 1U, deadline > now ? (int)(deadline - now) : 1) >= 0 ||
           errno == EINTR);
    assert(adapter->pump(adapter->context, now_ms(), &target, &ready, NULL) ==
           CAI_OK);
  }
  cai_cli_attach_boot_close(boot);

  /* Cancelling lookup creates neither a process nor a controller lease. */
  server = owner(runtime, workspace, NULL);
  boot = open_boot(&options, no_launch, conflict);
  cai_cli_attach_boot_close(boot);
  no_attachment(server);
  cai_workspace_server_close(server);
  rc = cai_cli_attach_boot_open(&options, child, NULL, 65536, conflict, &boot,
                                NULL);
  assert(rc == CAI_ERR_INVALID && boot == NULL);
  rc = cai_cli_attach_boot_open(NULL, NULL, NULL, 0, NULL, &boot, NULL);
  assert(rc == CAI_ERR_INVALID && boot == NULL);
}
int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "--stall") == 0) {
    cai_workspace_endpoint *endpoint;
    assert(cai_workspace_endpoint_open(argv[2], argv[3], &endpoint, NULL) ==
           CAI_OK);
    assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
    puts("STALLED");
    fflush(stdout);
    for (;;)
      pause();
  }
  assert(argc == 4);
  cases(argv[1], argv[2], argv[3]);
  return 0;
}
