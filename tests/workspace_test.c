#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/wire.h>
#include <cai/workspace.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct fixture {
  char root[4096], runtime[4096], workspace[4096], alias[4096], child[4096];
} fixture;
static void path(char *out, size_t capacity, const char *root,
                 const char *name) {
  int n = snprintf(out, capacity, "%s/%s", root, name);
  assert(n > 0 && (size_t)n < capacity);
}
static void setup(fixture *state) {
  /* The musl build directory itself can exceed sun_path once a socket name
   * is added. Use a short disposable root; path-limit rejection has its own
   * explicit test and must not prevent every other socket test from running. */
  char name[] = "/tmp/cai-workspace-XXXXXX";
  char *root = mkdtemp(name);
  assert(root != NULL && realpath(root, state->root) != NULL);
  path(state->runtime, sizeof(state->runtime), state->root, "run");
  path(state->workspace, sizeof(state->workspace), state->root, "work");
  path(state->alias, sizeof(state->alias), state->root, "alias");
  path(state->child, sizeof(state->child), state->workspace, "child");
  assert(mkdir(state->runtime, 0700) == 0);
  assert(mkdir(state->workspace, 0700) == 0);
  assert(mkdir(state->child, 0700) == 0);
  assert(symlink(state->workspace, state->alias) == 0);
}
static void lock_path(const cai_workspace_endpoint *endpoint, char *out,
                      size_t capacity) {
  size_t length = strlen(cai_workspace_endpoint_socket(endpoint));
  assert(length + 1U <= capacity && length > 5U);
  memcpy(out, cai_workspace_endpoint_socket(endpoint), length + 1U);
  memcpy(out + length - 4U, "lock", 4U);
}
static void cleanup(fixture *state, cai_workspace_endpoint *endpoint) {
  char lock[4096];
  lock_path(endpoint, lock, sizeof(lock));
  cai_workspace_endpoint_close(endpoint);
  assert(unlink(lock) == 0);
  assert(unlink(state->alias) == 0);
  assert(rmdir(state->child) == 0);
  assert(rmdir(state->workspace) == 0);
  assert(rmdir(state->runtime) == 0);
  assert(rmdir(state->root) == 0);
}
static int listen_raw(const char *path, int listening) {
  struct sockaddr_un address;
  int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(descriptor >= 0 && strlen(path) < sizeof(address.sun_path));
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path, strlen(path) + 1U);
  assert(bind(descriptor, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(chmod(path, 0600) == 0);
  if (listening)
    assert(listen(descriptor, 4) == 0);
  return descriptor;
}
static void identity(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *a = NULL, *b = NULL, *child = NULL;
  char cwd[4096], after[4096];
  assert(state != NULL && getcwd(cwd, sizeof(cwd)) != NULL);
  setup(state);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace, &a,
                                     NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(state->runtime, state->alias, &b, NULL) ==
         CAI_OK);
  assert(cai_workspace_endpoint_open(state->runtime, state->child, &child,
                                     NULL) == CAI_OK);
  assert(strcmp(cai_workspace_endpoint_id(a), cai_workspace_endpoint_id(b)) ==
         0);
  assert(strcmp(cai_workspace_endpoint_socket(a),
                cai_workspace_endpoint_socket(b)) == 0);
  assert(strcmp(cai_workspace_endpoint_id(a),
                cai_workspace_endpoint_id(child)) != 0);
  assert(strcmp(cai_workspace_endpoint_directory(b), state->workspace) == 0);
  assert(getcwd(after, sizeof(after)) != NULL && strcmp(cwd, after) == 0);
  cai_workspace_endpoint_close(b);
  cai_workspace_endpoint_close(child);
  assert(cai_workspace_endpoint_listen(a, NULL) == CAI_OK);
  cleanup(state, a);
  free(state);
}
static int receive(void *context, const cai_wire_event *event,
                   cai_error *error) {
  int *value = context;
  (void)error;
  if (event->type == CAI_WIRE_TOKEN && event->token->type == CAI_CBOR_UINT) {
    assert(event->token->argument == 42U);
    *value = 1;
  }
  if (event->type == CAI_WIRE_END) {
    assert(*value == 1);
    *value = 2;
  }
  return CAI_OK;
}
static void connection(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *a = NULL, *b = NULL;
  cai_wire_peer *sender = NULL, *reader = NULL;
  cai_cbor_token token;
  struct stat before, after;
  char lock[4096];
  int client, server, connecting, eof, value = 0;
  assert(state != NULL);
  setup(state);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace, &a,
                                     NULL) == CAI_OK);
  assert(cai_workspace_endpoint_open(state->runtime, state->alias, &b, NULL) ==
         CAI_OK);
  assert(cai_workspace_endpoint_connect(a, &client, &connecting, NULL) ==
         CAI_ERR_TRANSPORT);
  assert(cai_workspace_endpoint_listen(a, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_listen(a, NULL) == CAI_ERR_INVALID);
  assert(cai_workspace_endpoint_listen(b, NULL) == CAI_ERR_LIMIT);
  assert(cai_workspace_endpoint_accept(a, &server, NULL) == CAI_OK &&
         server == -1);
  assert(stat(cai_workspace_endpoint_socket(a), &after) == 0);
  assert(S_ISSOCK(after.st_mode) && (after.st_mode & 0777U) == 0600);
  assert(cai_workspace_endpoint_connect(b, &client, &connecting, NULL) ==
         CAI_OK);
  if (connecting) {
    struct pollfd fd;
    memset(&fd, 0, sizeof(fd));
    fd.fd = client;
    fd.events = POLLOUT;
    assert(poll(&fd, 1U, 1000) > 0);
  }
  assert(cai_workspace_endpoint_connect_finish(client, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_accept(a, &server, NULL) == CAI_OK &&
         server >= 0);
  assert((fcntl(client, F_GETFD) & FD_CLOEXEC) != 0);
  assert((fcntl(server, F_GETFD) & FD_CLOEXEC) != 0);
  assert(cai_wire_peer_open(client, 0U, receive, &value, &sender, NULL) ==
         CAI_OK);
  assert(cai_wire_peer_open(server, 0U, receive, &value, &reader, NULL) ==
         CAI_OK);
  memset(&token, 0, sizeof(token));
  token.type = CAI_CBOR_UINT;
  token.argument = 42U;
  assert(cai_wire_peer_begin(sender, 2U, NULL) == CAI_OK);
  assert(cai_wire_peer_write(sender, &token, NULL) == CAI_OK);
  assert(cai_wire_peer_end(sender, NULL) == CAI_OK);
  assert(cai_wire_peer_flush(sender, 0U, NULL) == CAI_OK);
  assert(cai_wire_peer_read(reader, 0U, &eof, NULL) == CAI_OK && !eof);
  assert(value == 2);
  cai_wire_peer_close(sender);
  cai_wire_peer_close(reader);
  lock_path(a, lock, sizeof(lock));
  assert(stat(lock, &before) == 0);
  cai_workspace_endpoint_close(a);
  assert(cai_workspace_endpoint_listen(b, NULL) == CAI_OK);
  assert(stat(lock, &after) == 0 && after.st_ino == before.st_ino &&
         after.st_dev == before.st_dev);
  cleanup(state, b);
  free(state);
}
static void unsafe_paths(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *endpoint = NULL, *invalid = NULL;
  char socket_path[108];
  struct stat status;
  char lock[4096];
  int file, other, connecting;
  assert(state != NULL);
  setup(state);
  assert(chmod(state->runtime, 0755) == 0);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace, &invalid,
                                     NULL) == CAI_ERR_INVALID);
  assert(invalid == NULL && chmod(state->runtime, 0700) == 0);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace,
                                     &endpoint, NULL) == CAI_OK);
  assert(strlen(cai_workspace_endpoint_socket(endpoint)) < sizeof(socket_path));
  memcpy(socket_path, cai_workspace_endpoint_socket(endpoint),
         strlen(cai_workspace_endpoint_socket(endpoint)) + 1U);
  file = open(socket_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
  assert(file >= 0);
  close(file);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_ERR_INVALID);
  assert(cai_workspace_endpoint_connect(endpoint, &other, &connecting, NULL) ==
         CAI_ERR_INVALID);
  assert(lstat(socket_path, &status) == 0 && S_ISREG(status.st_mode));
  assert(unlink(socket_path) == 0);
  assert(symlink(state->workspace, socket_path) == 0);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_ERR_INVALID);
  assert(lstat(socket_path, &status) == 0 && S_ISLNK(status.st_mode));
  assert(unlink(socket_path) == 0);
  lock_path(endpoint, lock, sizeof(lock));
  assert(chmod(lock, 0644) == 0);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_ERR_INVALID);
  assert(chmod(lock, 0600) == 0);
  assert(unlink(lock) == 0 && symlink(state->workspace, lock) == 0);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) != CAI_OK);
  assert(lstat(lock, &status) == 0 && S_ISLNK(status.st_mode));
  assert(unlink(lock) == 0);
  other = listen_raw(socket_path, 1);
  assert(chmod(socket_path, 0660) == 0);
  assert(cai_workspace_endpoint_connect(endpoint, &file, &connecting, NULL) ==
         CAI_ERR_INVALID);
  assert(chmod(socket_path, 0600) == 0);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_ERR_LIMIT);
  assert(lstat(socket_path, &status) == 0 && S_ISSOCK(status.st_mode));
  close(other);
  /* Now the same private socket is stale and can be reclaimed. */
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
  /* Replacing the filesystem entry must not make close unlink the new file. */
  assert(unlink(socket_path) == 0);
  file = open(socket_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
  assert(file >= 0);
  close(file);
  cai_workspace_endpoint_close(endpoint);
  endpoint = NULL;
  assert(lstat(socket_path, &status) == 0 && S_ISREG(status.st_mode));
  assert(unlink(socket_path) == 0);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace,
                                     &endpoint, NULL) == CAI_OK);
  cleanup(state, endpoint);
  free(state);
}
static void invalid_directories(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *endpoint = NULL, *invalid = NULL;
  char long_path[4096], name[81];
  int file;
  assert(state != NULL);
  setup(state);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace,
                                     &endpoint, NULL) == CAI_OK);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
  memset(name, 'p', sizeof(name) - 1U);
  name[sizeof(name) - 1U] = '\0';
  path(long_path, sizeof(long_path), state->root, name);
  assert(mkdir(long_path, 0700) == 0);
  assert(cai_workspace_endpoint_open(long_path, state->workspace, &invalid,
                                     NULL) == CAI_ERR_INVALID);
  assert(invalid == NULL && rmdir(long_path) == 0);
  assert(cai_workspace_endpoint_open(state->alias, state->workspace, &invalid,
                                     NULL) == CAI_ERR_INVALID);
  path(long_path, sizeof(long_path), state->root, "not-a-directory");
  file = open(long_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  assert(file >= 0);
  close(file);
  assert(cai_workspace_endpoint_open(state->runtime, long_path, &invalid,
                                     NULL) == CAI_ERR_INVALID);
  assert(invalid == NULL && unlink(long_path) == 0);
  cleanup(state, endpoint);
  free(state);
}
static void crash_recovery(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *endpoint = NULL;
  int ready[2], status;
  char byte;
  pid_t child;
  assert(state != NULL);
  setup(state);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace,
                                     &endpoint, NULL) == CAI_OK);
  assert(pipe(ready) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(ready[0]);
    assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
    assert(write(ready[1], "r", 1U) == 1);
    for (;;)
      pause();
  }
  close(ready[1]);
  assert(read(ready[0], &byte, 1U) == 1 && byte == 'r');
  close(ready[0]);
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_ERR_LIMIT);
  assert(kill(child, SIGKILL) == 0);
  assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
  assert(cai_workspace_endpoint_listen(endpoint, NULL) == CAI_OK);
  cleanup(state, endpoint);
  free(state);
}
static void simultaneous_start(void) {
  fixture *state = calloc(1U, sizeof(*state));
  cai_workspace_endpoint *endpoint = NULL;
  int start[2], result[2], stop[2], status;
  char results[2];
  pid_t children[2];
  size_t i;
  assert(state != NULL);
  setup(state);
  assert(cai_workspace_endpoint_open(state->runtime, state->workspace,
                                     &endpoint, NULL) == CAI_OK);
  assert(pipe(start) == 0 && pipe(result) == 0 && pipe(stop) == 0);
  for (i = 0U; i < 2U; ++i) {
    children[i] = fork();
    assert(children[i] >= 0);
    if (children[i] == 0) {
      int rc;
      char byte;
      close(start[1]);
      close(result[0]);
      close(stop[1]);
      assert(read(start[0], &byte, 1U) == 1);
      rc = cai_workspace_endpoint_listen(endpoint, NULL);
      assert(rc == CAI_OK || rc == CAI_ERR_LIMIT);
      assert(write(result[1], rc == CAI_OK ? "w" : "l", 1U) == 1);
      assert(read(stop[0], &byte, 1U) == 1);
      cai_workspace_endpoint_close(endpoint);
      _exit(0);
    }
  }
  close(start[0]);
  close(result[1]);
  close(stop[0]);
  assert(write(start[1], "ss", 2U) == 2);
  assert(read(result[0], results, 1U) == 1);
  assert(read(result[0], results + 1U, 1U) == 1);
  assert((results[0] == 'w' && results[1] == 'l') ||
         (results[0] == 'l' && results[1] == 'w'));
  assert(write(stop[1], "ss", 2U) == 2);
  for (i = 0U; i < 2U; ++i) {
    assert(waitpid(children[i], &status, 0) == children[i]);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  close(start[1]);
  close(result[0]);
  close(stop[1]);
  cleanup(state, endpoint);
  free(state);
}
static void *worker_cases(void *context) {
  (void)context;
  identity();
  connection();
  unsafe_paths();
  invalid_directories();
  return NULL;
}
int main(void) {
  pthread_t worker;
  /* Also exercise crypto/path handling on musl's default worker stack. */
  assert(pthread_create(&worker, NULL, worker_cases, NULL) == 0);
  assert(pthread_join(worker, NULL) == 0);
  crash_recovery();
  simultaneous_start();
  return 0;
}
