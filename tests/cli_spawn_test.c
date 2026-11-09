#define _POSIX_C_SOURCE 200809L
#include "../cli/spawn.h"
#include <assert.h>
#include <cai/app_client.h>
#include <cai/workspace_client.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static cai_cli_spawn *active, *other;
static volatile sig_atomic_t owned[2];
static void failed(int number) {
  size_t i;
  for (i = 0U; i < 2U; i++) {
    pid_t pid = (pid_t)owned[i];
    int status;
    if (pid > 0 && waitpid(pid, &status, WNOHANG) == 0) {
      (void)kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  _exit(128 + number);
}
static int open_spawn(const char *program, int argc, const char *const *argv,
                      uint64_t now, uint64_t timeout, cai_cli_spawn **out,
                      cai_error *error) {
  int rc = cai_cli_spawn_open(program, argc, argv, now, timeout, out, error);
  if (rc == CAI_OK)
    owned[out == &other ? 1 : 0] = (sig_atomic_t)cai_cli_spawn_pid(*out);
  return rc;
}
#define cai_cli_spawn_open open_spawn
static uint64_t clock_ms(void) {
  struct timespec value;
  assert(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
  return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}
static int ended(cai_cli_spawn *s) {
  cai_cli_spawn_phase p = cai_cli_spawn_get_state(s)->phase;
  return p == CAI_CLI_SPAWN_EXITED || p == CAI_CLI_SPAWN_FAILED;
}
static void step(cai_cli_spawn *s) {
  struct pollfd fd;
  uint64_t deadline, now = clock_ms();
  int timeout = 10000;
  cai_error error;
  cai_error_init(&error);
  assert(cai_cli_spawn_prepare(s, &fd, &deadline, &error) == CAI_OK);
  if (deadline && deadline <= now)
    timeout = 0;
  else if (deadline && deadline - now < (uint64_t)timeout)
    timeout = (int)(deadline - now);
  assert(poll(&fd, 1U, timeout) >= 0 || errno == EINTR);
  if (cai_cli_spawn_pump(s, clock_ms(), &error) != CAI_OK) {
    fprintf(stderr, "spawn pump: %s\n",
            error.message ? error.message : "unknown");
    abort();
  }
  cai_error_cleanup(&error);
}
static void wait_phase(cai_cli_spawn *s, int ready) {
  uint64_t until = clock_ms() + 10000U;
  while (!ended(s) && (!ready || !cai_cli_spawn_get_state(s)->was_ready)) {
    assert(clock_ms() < until);
    step(s);
  }
  if (ready && !cai_cli_spawn_get_state(s)->was_ready) {
    fprintf(stderr, "owner failed: %s\n",
            cai_cli_spawn_get_state(s)->observation.message);
    abort();
  }
}
static void finish(cai_cli_spawn **s, int expected) {
  struct pollfd fd;
  uint64_t deadline;
  pid_t pid = cai_cli_spawn_pid(*s);
  int status;
  wait_phase(*s, 0);
  assert(cai_cli_spawn_get_state(*s)->phase == (cai_cli_spawn_phase)expected);
  assert(cai_cli_spawn_prepare(*s, &fd, &deadline, NULL) == CAI_OK &&
         fd.fd == -1 && deadline == 0U);
  assert(waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
  owned[s == &other ? 1 : 0] = 0;
  assert(cai_cli_spawn_close(*s, NULL) == CAI_OK);
  *s = NULL;
}
/* No parent-side storage clients may be opened by launching or attaching. */
int __wrap_lc_client_open(void *config, void *out, void *error);
int __wrap_lc_client_open(void *config, void *out, void *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
  return -1;
}
static int ignore_record(void *context, const cai_wire_event *e,
                         cai_error *error) {
  (void)context;
  (void)e;
  (void)error;
  return CAI_OK;
}
static void fake_child(int argc, char **argv) {
  int fd = atoi(argv[3]);
  const char *mode = argv[5];
  cai_wire_peer *peer;
  cai_cli_bootstrap_message *v = calloc(1U, sizeof(*v));
  assert(argc == 6 && strcmp(argv[1], "-x") == 0 && v);
  if (strcmp(mode, "early-exit") == 0)
    _exit(23);
  if (strcmp(mode, "truncated") == 0) {
    static const unsigned char partial[] = {0, 0, 0, 2, 0x81};
    assert(write(fd, partial, sizeof(partial)) == sizeof(partial));
    _exit(24);
  }
  if (strcmp(mode, "wait") == 0)
    for (;;)
      pause();
  assert(cai_wire_peer_open(fd, 16384U, ignore_record, NULL, &peer, NULL) ==
         CAI_OK);
  if (strcmp(mode, "stubborn") == 0) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGTERM, &action, NULL) == 0);
    assert(cai_cli_bootstrap_write(peer, v, NULL) == CAI_OK);
    assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
    for (;;)
      pause();
  }
  assert(strcmp(mode, "duplicate") == 0 || strcmp(mode, "noisy") == 0 ||
         strcmp(mode, "nonzero") == 0);
  v->ready = 1;
  memset(v->instance, 42, sizeof(v->instance));
  strcpy(v->session, "fake");
  assert(cai_cli_bootstrap_write(peer, v, NULL) == CAI_OK);
  if (strcmp(mode, "duplicate") == 0) {
    assert(cai_cli_bootstrap_write(peer, v, NULL) == CAI_OK);
  } else if (strcmp(mode, "noisy") == 0) {
    char byte;
    assert(write(1, "MUST NOT REACH STDOUT", 21U) == 21);
    assert(write(2, "MUST NOT REACH STDERR", 21U) == 21);
    assert(read(0, &byte, 1U) == 0);
    v->ready = 0;
    assert(cai_cli_bootstrap_write(peer, v, NULL) == CAI_OK);
  } else {
    v->ready = 0;
    v->exit_code = 23;
    assert(cai_cli_bootstrap_write(peer, v, NULL) == CAI_OK);
  }
  assert(cai_wire_peer_flush(peer, 0U, NULL) == CAI_OK);
  cai_wire_peer_close(peer);
  free(v);
  _exit(strcmp(mode, "nonzero") == 0 ? 23 : 0);
}
static void fixtures(const char *self) {
  static const char *const names[] = {"early-exit", "truncated", "duplicate",
                                      "nonzero", "noisy"};
  size_t i;
  cai_error error;
  cai_error_init(&error);
  for (i = 0U; i < sizeof(names) / sizeof(names[0]); i++) {
    assert(cai_cli_spawn_open(self, 1, &names[i], clock_ms(), 0U, &active,
                              &error) == CAI_OK);
    assert(cai_cli_spawn_close(active, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    if (strcmp(names[i], "nonzero") == 0) {
      wait_phase(active, 0);
      assert(cai_cli_spawn_get_state(active)->was_ready &&
             cai_cli_spawn_get_state(active)->exit_code == 23 &&
             cai_cli_spawn_get_state(active)->observation.code ==
                 CAI_ERR_TRANSPORT);
      assert(strstr(cai_cli_spawn_get_state(active)->observation.message,
                    "unsuccessfully"));
    }
    finish(&active, strcmp(names[i], "noisy") == 0 ? CAI_CLI_SPAWN_EXITED
                                                   : CAI_CLI_SPAWN_FAILED);
  }
  {
    const char *args[] = {"noisy"};
    int saved_input = dup(STDIN_FILENO);
    assert(saved_input >= 0 && close(STDIN_FILENO) == 0);
    assert(cai_cli_spawn_open(self, 1, args, clock_ms(), 0U, &active, NULL) ==
           CAI_OK);
    assert(dup2(saved_input, STDIN_FILENO) == STDIN_FILENO);
    close(saved_input);
    finish(&active, CAI_CLI_SPAWN_EXITED);
  }
  {
    const char *args[] = {"wait"};
    uint64_t now = clock_ms();
    assert(cai_cli_spawn_open(self, 1, args, now, 1U, &active, NULL) == CAI_OK);
    assert(cai_cli_spawn_pump(active, now + 2U, NULL) == CAI_OK);
    assert(strstr(cai_cli_spawn_get_state(active)->observation.message,
                  "timed out"));
    finish(&active, CAI_CLI_SPAWN_FAILED);
    {
      sigset_t block, before, after;
      sigemptyset(&block);
      sigaddset(&block, SIGTERM);
      assert(sigprocmask(SIG_BLOCK, &block, &before) == 0);
      assert(cai_cli_spawn_open(self, 1, args, clock_ms(), 0U, &active, NULL) ==
             CAI_OK);
      assert(sigprocmask(SIG_BLOCK, NULL, &after) == 0 &&
             sigismember(&after, SIGTERM) == 1);
      assert(sigprocmask(SIG_SETMASK, &before, NULL) == 0);
    }
    assert(cai_cli_spawn_cancel(active, clock_ms(), NULL) == CAI_OK);
    wait_phase(active, 0);
    assert(cai_cli_spawn_get_state(active)->signal == SIGTERM);
    finish(&active, CAI_CLI_SPAWN_FAILED);
  }
  {
    const char *args[] = {"stubborn"};
    assert(cai_cli_spawn_open(self, 1, args, clock_ms(), 0U, &active, NULL) ==
           CAI_OK);
    while (cai_cli_spawn_get_state(active)->phase == CAI_CLI_SPAWN_STARTING)
      step(active);
    assert(cai_cli_spawn_cancel(active, clock_ms(), NULL) == CAI_OK);
    assert(cai_cli_spawn_pump(active, clock_ms() + 5001U, NULL) == CAI_OK);
    wait_phase(active, 0);
    assert(cai_cli_spawn_get_state(active)->signal == SIGKILL);
    finish(&active, CAI_CLI_SPAWN_FAILED);
  }
  assert(cai_cli_spawn_open("relative", 0, NULL, 0U, 0U, &active, NULL) ==
             CAI_ERR_INVALID &&
         active == NULL);
  assert(cai_cli_spawn_open("/no-such-cai-executable", 0, NULL, 0U, 0U, &active,
                            NULL) == CAI_ERR_INVALID &&
         active == NULL);
  {
    struct sigaction action, before;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGCHLD, &action, &before) == 0);
    assert(cai_cli_spawn_open(self, 0, NULL, 0U, 0U, &active, NULL) ==
               CAI_ERR_INVALID &&
           active == NULL);
    assert(sigaction(SIGCHLD, &before, NULL) == 0);
  }
}
static cai_workspace_client *attach(const unsigned char *instance,
                                    int persistent) {
  cai_workspace_client_config config;
  cai_workspace_client_state state;
  cai_workspace_client *client;
  uint64_t until = clock_ms() + 10000U;
  cai_workspace_client_config_init(&config);
  config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
  config.workspace = getenv("CAI_TEST_CONTROLLER_WORK");
  config.expected_instance = instance;
  assert(cai_workspace_client_open(&config, clock_ms(), &client, NULL) ==
         CAI_OK);
  for (;;) {
    struct pollfd fd;
    uint64_t deadline;
    assert(clock_ms() < until);
    assert(cai_workspace_client_pump(client, clock_ms(), NULL) == CAI_OK);
    assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
    assert(state.phase != CAI_WORKSPACE_DISCONNECTED &&
           state.phase != CAI_WORKSPACE_FAILED);
    if (state.phase == CAI_WORKSPACE_CONNECTED && state.generation &&
        state.controller == state.attachment)
      break;
    assert(cai_workspace_client_prepare(client, &fd, &deadline, NULL) ==
           CAI_OK);
    assert(poll(&fd, 1U, 1000) >= 0);
  }
  assert(state.persist == persistent);
  assert(memcmp(state.instance, instance, CAI_WORKSPACE_INSTANCE_BYTES) == 0);
  return client;
}
static void send_control(cai_workspace_client *client, int shutdown) {
  cai_workspace_client_state state;
  assert(cai_workspace_client_get_state(client, &state, NULL) == CAI_OK);
  if (shutdown)
    assert(cai_workspace_client_shutdown(client, 1U, state.generation, NULL) ==
           CAI_OK);
  else
    assert(cai_workspace_client_detach(client, NULL) == CAI_OK);
  assert(cai_workspace_client_pump(client, clock_ms(), NULL) == CAI_OK);
  cai_workspace_client_close(client);
}
static int admission(void *context, const cai_app_client_event *event,
                     cai_error *error) {
  int *accepted = context;
  (void)error;
  if (event->type == CAI_APP_CLIENT_INPUT && event->input != NULL &&
      event->input->type == CAI_INPUT_OBSERVATION_RESULT) {
    assert(event->input->result.outcome == CAI_INPUT_ACCEPTED);
    *accepted = 1;
  }
  return CAI_OK;
}
/** Quiet owners open the runtime but cannot consume argv instructions. Only
 * this subsequent authenticated queue admission starts inference. */
static cai_app_client *admit_after_ready(const unsigned char *instance,
                                         int *accepted) {
  cai_app_client_config config;
  cai_app_client_state state;
  cai_app_client *client = NULL;
  uint64_t until = clock_ms() + 5000U;
  int bound = 0, offered = 0;
  cai_app_client_config_init(&config);
  config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
  config.workspace = getenv("CAI_TEST_CONTROLLER_WORK");
  config.expected_instance = instance;
  config.event = admission;
  config.context = accepted;
  assert(cai_app_client_open(&config, clock_ms(), &client, NULL) == CAI_OK);
  while (!*accepted) {
    struct pollfd fd;
    uint64_t deadline;
    assert(clock_ms() < until);
    assert(cai_app_client_pump(client, clock_ms(), NULL) == CAI_OK);
    assert(cai_app_client_get_state(client, &state, NULL) == CAI_OK);
    if (state.transport.phase == CAI_WORKSPACE_CONNECTED && !bound) {
      assert(cai_app_client_bind(client, 1U, 0U, 0U, NULL, NULL) == CAI_OK);
      bound = 1;
    }
    if (state.ready && !offered) {
      cai_input_request request;
      cai_input_scope scope;
      struct stat st;
      assert(state.queue.count == 0U && state.last_request == 0U);
      assert(stat(getenv("CAI_TEST_SPAWN_REQUESTS"), &st) != 0 ||
             st.st_size == 0);
      memset(&request, 0, sizeof(request));
      request.request_id = 1U;
      request.text = "Admitted after readiness";
      scope.conversation = state.conversation;
      scope.generation = state.transport.generation;
      assert(cai_app_client_request(client, &scope, &request, NULL) == CAI_OK);
      offered = 1;
    }
    if (!*accepted) {
      assert(cai_app_client_prepare(client, &fd, &deadline, NULL) == CAI_OK);
      assert(poll(&fd, 1U, 1000) >= 0);
    }
  }
  return client;
}
int main(int argc, char **argv) {
  cai_workspace_client *client;
  cai_app_client *admitted = NULL;
  const cai_cli_spawn_state *state;
  const char *mode = getenv("CAI_TEST_SPAWN_MODE");
  int persistent, admitted_receipt = 0;
  if (argc > 2 && strcmp(argv[1], "-x") == 0 &&
      strcmp(argv[2], "--_workspace-owner") == 0) {
    fake_child(argc, argv);
    return 0;
  }
  {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = failed;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGABRT, &action, NULL) == 0);
    assert(sigaction(SIGTERM, &action, NULL) == 0);
  }
  assert(argc > 2 && mode);
  if (strcmp(mode, "fixtures") == 0) {
    fixtures(argv[0]);
    return 0;
  }
  persistent =
      strcmp(mode, "persistent") == 0 || strcmp(mode, "half-close") == 0;
  assert(cai_cli_spawn_open(argv[1], argc - 2, (const char *const *)(argv + 2),
                            clock_ms(), 0U, &active, NULL) == CAI_OK);
  if (strcmp(mode, "cancel") == 0) {
    assert(cai_cli_spawn_cancel(active, clock_ms(), NULL) == CAI_OK);
    finish(&active, CAI_CLI_SPAWN_FAILED);
    return 0;
  }
  if (strcmp(mode, "disconnect") == 0) {
    struct pollfd fd;
    uint64_t deadline;
    assert(cai_cli_spawn_prepare(active, &fd, &deadline, NULL) == CAI_OK);
    assert(shutdown(fd.fd, SHUT_WR) == 0);
    finish(&active, CAI_CLI_SPAWN_FAILED);
    return 0;
  }
  if (strcmp(mode, "invalid") == 0) {
    wait_phase(active, 0);
    state = cai_cli_spawn_get_state(active);
    assert(!state->was_ready && state->observation.code &&
           state->exit_code == 2);
    assert(strstr(state->observation.message, "replay_turns"));
    finish(&active, CAI_CLI_SPAWN_FAILED);
    return 0;
  }
  wait_phase(active, 1);
  state = cai_cli_spawn_get_state(active);
  assert(state->observation.log_path[0] == '/' &&
         state->observation.session[0]);
  assert(strstr(state->observation.log_path, state->observation.session));
  assert(access(state->observation.log_path, F_OK) == 0);
  assert(cai_cli_spawn_cancel(active, clock_ms(), NULL) == CAI_ERR_INVALID);
  if (strcmp(mode, "signal") == 0 || strcmp(mode, "active") == 0) {
    if (strcmp(mode, "active") == 0) {
      struct stat request;
      const char *path = getenv("CAI_TEST_SPAWN_REQUESTS");
      uint64_t until = clock_ms() + 5000U;
      assert(path != NULL);
      admitted =
          admit_after_ready(state->observation.instance, &admitted_receipt);
      while (stat(path, &request) != 0 || request.st_size == 0) {
        assert(clock_ms() < until);
        (void)poll(NULL, 0U, 1);
      }
    }
    assert(kill(cai_cli_spawn_pid(active), SIGTERM) == 0);
    finish(&active, CAI_CLI_SPAWN_EXITED);
    cai_app_client_close(admitted);
    return 0;
  }
  if (strcmp(mode, "half-close") == 0) {
    struct pollfd fd;
    uint64_t deadline;
    assert(cai_cli_spawn_prepare(active, &fd, &deadline, NULL) == CAI_OK);
    assert(shutdown(fd.fd, SHUT_WR) == 0);
  }
  assert(cai_cli_spawn_open(argv[1], argc - 2, (const char *const *)(argv + 2),
                            clock_ms(), 0U, &other, NULL) == CAI_OK);
  wait_phase(other, 0);
  assert(cai_cli_spawn_get_state(other)->observation.code == CAI_ERR_LIMIT);
  finish(&other, CAI_CLI_SPAWN_FAILED);
  client = attach(state->observation.instance, persistent);
  send_control(client, 0);
  if (persistent) {
    struct pollfd fd;
    uint64_t deadline;
    assert(cai_cli_spawn_prepare(active, &fd, &deadline, NULL) == CAI_OK &&
           fd.fd >= 0 && deadline == 0U);
    assert(poll(&fd, 1U, 50) == 0);
    client = attach(state->observation.instance, persistent);
    send_control(client, 1);
  }
  finish(&active, CAI_CLI_SPAWN_EXITED);
  return 0;
}
