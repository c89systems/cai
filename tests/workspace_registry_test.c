#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/workspace.h>
#include <cai/workspace_registry.h>
#include <cai/workspace_server.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static cai_workspace_server *start(const char *root, const char *workspace,
                                   const char *name, int expected) {
  cai_workspace_server_config config;
  cai_workspace_server *server = NULL;
  cai_workspace_server_config_init(&config);
  config.runtime_directory = root;
  config.workspace = workspace;
  config.name = name;
  assert(cai_workspace_server_open(&config, &server, NULL) == expected);
  assert((server != NULL) == (expected == CAI_OK));
  return server;
}
static cai_workspace_registry *one(const char *root, const char *workspace,
                                   const char *name,
                                   const unsigned char *instance) {
  cai_workspace_registry *r;
  const cai_workspace_record *record;
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_OK && record);
  assert(strcmp(record->workspace, workspace) == 0 &&
         strcmp(record->name, name) == 0);
  assert(strlen(record->id) == 32U);
  if (instance)
    assert(memcmp(record->instance, instance, 16U) == 0);
  return r;
}
static void empty(const char *root) {
  cai_workspace_registry *r;
  const cai_workspace_record *record;
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_OK && !record);
  cai_workspace_registry_close(r);
}
static void path_for(const char *root, const char *workspace, char *out,
                     size_t capacity) {
  cai_workspace_endpoint *endpoint;
  assert(cai_workspace_endpoint_open(root, workspace, &endpoint, NULL) ==
         CAI_OK);
  assert(snprintf(out, capacity, "%s/ws-%s.info", root,
                  cai_workspace_endpoint_id(endpoint)) > 0);
  cai_workspace_endpoint_close(endpoint);
}
static void copy(const char *from, const char *to) {
  char chunk[512];
  int in = open(from, O_RDONLY | O_CLOEXEC), out;
  ssize_t n;
  assert(in >= 0);
  out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(out >= 0);
  while ((n = read(in, chunk, sizeof(chunk))) > 0)
    assert(write(out, chunk, (size_t)n) == n);
  assert(n == 0 && close(in) == 0 && close(out) == 0);
}
static void lifecycle(const char *root, const char *a, const char *b) {
  cai_workspace_server *first, *second;
  cai_workspace_registry *r;
  const cai_workspace_record *record;
  size_t count;
  first = start(root, a, "example", CAI_OK);
  r = one(root, a, "example", cai_workspace_server_instance(first));
  assert(cai_workspace_registry_prune(r, NULL) == CAI_ERR_LIMIT);
  cai_workspace_registry_close(r);
  (void)start(root, b, "example", CAI_ERR_LIMIT);
  second = start(root, b, "second", CAI_OK);
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  count = 0U;
  while (cai_workspace_registry_next(r, &record, NULL) == CAI_OK && record)
    ++count;
  assert(count == 2U);
  cai_workspace_registry_close(r);
  cai_workspace_server_close(first);
  cai_workspace_server_close(second);
  empty(root);
  second = start(root, b, "example", CAI_OK);
  r = one(root, b, "example", cai_workspace_server_instance(second));
  cai_workspace_registry_close(r);
  cai_workspace_server_close(second);
  empty(root);
  (void)start(root, a, "bad/name", CAI_ERR_INVALID);
  (void)start(root, a, "bad\nname", CAI_ERR_INVALID);
  (void)start(root, a, "bad\xc2\x9b", CAI_ERR_INVALID);
  (void)start(root, a, "bad\xff", CAI_ERR_PROTOCOL);
  first = start(root, a, "caf\xc3\xa9", CAI_OK);
  r = one(root, a, "caf\xc3\xa9", cai_workspace_server_instance(first));
  cai_workspace_registry_close(r);
  cai_workspace_server_close(first);
  empty(root);
}
static void replacement(const char *root, const char *workspace) {
  cai_workspace_server *server;
  cai_workspace_registry *r;
  char path[256], replacement_path[256];
  server = start(root, workspace, NULL, CAI_OK);
  r = one(root, workspace, "", cai_workspace_server_instance(server));
  path_for(root, workspace, path, sizeof(path));
  snprintf(replacement_path, sizeof(replacement_path), "%s/replacement", root);
  copy(path, replacement_path);
  assert(rename(replacement_path, path) == 0);
  cai_workspace_server_close(server); /* Must preserve a different inode. */
  assert(access(path, F_OK) == 0);
  assert(cai_workspace_registry_prune(r, NULL) == CAI_ERR_INVALID);
  cai_workspace_registry_close(r);
  r = one(root, workspace, "", NULL);
  assert(cai_workspace_registry_prune(r, NULL) == CAI_OK);
  assert(cai_workspace_registry_prune(r, NULL) == CAI_ERR_INVALID);
  cai_workspace_registry_close(r);
  empty(root);
}
static void failures(const char *root, const char *workspace) {
  cai_workspace_server *server;
  cai_workspace_registry *r;
  const cai_workspace_record *record;
  char path[256], backup[256];
  int fd;
  path_for(root, workspace, path, sizeof(path));
  snprintf(backup, sizeof(backup), "%s/backup", root);
  server = start(root, workspace, NULL, CAI_OK);
  copy(path, backup);
  cai_workspace_server_close(server);
  assert(symlink(backup, path) == 0);
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(r, &record, NULL) != CAI_OK && !record);
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_OK && !record);
  cai_workspace_registry_close(r);
  assert(unlink(path) == 0 && access(backup, F_OK) == 0);
  copy(backup, path);
  assert(chmod(path, 0644) == 0);
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_ERR_PROTOCOL &&
         !record);
  cai_workspace_registry_close(r);
  assert(chmod(path, 0600) == 0);
  fd = open(path, O_WRONLY | O_TRUNC);
  assert(fd >= 0 && write(fd, "corrupt", 7U) == 7 && close(fd) == 0);
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_OK);
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_ERR_PROTOCOL &&
         !record);
  cai_workspace_registry_close(r);
  assert(unlink(path) == 0 && unlink(backup) == 0);
  assert(chmod(root, 0755) == 0);
  assert(cai_workspace_registry_open(root, &r, NULL) == CAI_ERR_INVALID && !r);
  assert(chmod(root, 0700) == 0);
  assert(cai_workspace_registry_open(root, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_workspace_registry_next(NULL, &record, NULL) == CAI_ERR_INVALID);
}
static void crash(const char *self, const char *root, const char *workspace,
                  int prune_first) {
  int pipefd[2], status;
  pid_t child;
  char ready;
  cai_workspace_registry *r;
  cai_workspace_server *server;
  unsigned char instance[16];
  const cai_workspace_record *record;
  assert(pipe(pipefd) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    char descriptor[32];
    close(pipefd[0]);
    snprintf(descriptor, sizeof(descriptor), "%d", pipefd[1]);
    execl(self, self, "--crash-owner", root, workspace, descriptor,
          (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  assert(read(pipefd[0], instance, sizeof(instance)) == 16);
  assert(read(pipefd[0], &ready, 1U) == 0);
  close(pipefd[0]);
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         !WEXITSTATUS(status));
  r = one(root, workspace, "crashed", instance);
  if (prune_first) {
    assert(cai_workspace_registry_prune(r, NULL) == CAI_OK);
    cai_workspace_registry_close(r);
  }
  /* Native crash recovery reuses the same ownership/name locks. */
  server = start(root, workspace, "crashed", CAI_OK);
  if (!prune_first) {
    assert(cai_workspace_registry_prune(r, NULL) == CAI_ERR_LIMIT);
    cai_workspace_registry_close(r);
  }
  r = one(root, workspace, "crashed", cai_workspace_server_instance(server));
  assert(cai_workspace_registry_next(r, &record, NULL) == CAI_OK && !record);
  cai_workspace_registry_close(r);
  cai_workspace_server_close(server);
  empty(root);
}
typedef struct fixture {
  const char *root, *a, *b;
} fixture;
static void staging(const char *root, const char *workspace) {
  cai_workspace_endpoint *endpoint;
  cai_workspace_server *server;
  char path[256], target[256];
  int fd;
  assert(cai_workspace_endpoint_open(root, workspace, &endpoint, NULL) ==
         CAI_OK);
  snprintf(path, sizeof(path), "%s/.ws-%s.tmp", root,
           cai_workspace_endpoint_id(endpoint));
  cai_workspace_endpoint_close(endpoint);
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && write(fd, "partial", 7U) == 7 && close(fd) == 0);
  server = start(root, workspace, NULL, CAI_OK);
  assert(access(path, F_OK) != 0);
  cai_workspace_server_close(server);
  snprintf(target, sizeof(target), "%s/target", root);
  fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(fd >= 0 && close(fd) == 0);
  assert(symlink(target, path) == 0);
  (void)start(root, workspace, NULL, CAI_ERR_INVALID);
  assert(access(target, F_OK) == 0 && unlink(path) == 0);
  assert(mkfifo(path, 0600) == 0);
  (void)start(root, workspace, NULL, CAI_ERR_INVALID);
  assert(unlink(path) == 0 && unlink(target) == 0);
  empty(root);
}
static void *worker(void *context) {
  fixture *f = context;
  lifecycle(f->root, f->a, f->b);
  replacement(f->root, f->a);
  failures(f->root, f->a);
  staging(f->root, f->a);
  return NULL;
}
int main(int argc, char **argv) {
  char root[] = "/tmp/cai-reg-XXXXXX", a[256], b[256], path[512];
  DIR *directory;
  struct dirent *entry;
  pthread_t thread;
  fixture f;
  if (argc == 5 && strcmp(argv[1], "--crash-owner") == 0) {
    cai_workspace_server *server = start(argv[2], argv[3], "crashed", CAI_OK);
    assert(write(atoi(argv[4]), cai_workspace_server_instance(server), 16U) ==
           16);
    _exit(0); /* No library close: emulate abrupt process death. */
  }
  assert(argc == 1);
  assert(mkdtemp(root));
  snprintf(a, sizeof(a), "%s/a", root);
  snprintf(b, sizeof(b), "%s/b", root);
  assert(mkdir(a, 0700) == 0 && mkdir(b, 0700) == 0);
  crash(argv[0], root, a, 0);
  crash(argv[0], root, a, 1);
  f.root = root;
  f.a = a;
  f.b = b;
  assert(pthread_create(&thread, NULL, worker, &f) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(rmdir(a) == 0 && rmdir(b) == 0);
  directory = opendir(root);
  assert(directory);
  while ((entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
    assert(unlink(path) == 0);
  }
  closedir(directory);
  assert(rmdir(root) == 0);
  return 0;
}
