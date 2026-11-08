#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/identity.h"
#include "../cli/temp.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include "worker_stack.h"
#include <sys/resource.h>

typedef struct removal {
  int parent, code, system_error;
} removal;
static void *remove_worker(void *context) {
  removal *request = context;
  request->code = cai_cli_remove_at(request->parent, "deep-cleanup");
  request->system_error = errno;
  return NULL;
}
static void deep_cleanup(const char *directory) {
  struct rlimit original, limited;
  removal request;
  pthread_t worker;
  int child, next;
  size_t i;
  assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
  if (original.rlim_cur < 900U)
    return;
  request.parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(request.parent >= 0);
  assert(mkdirat(request.parent, "deep-cleanup", 0700) == 0);
  child = openat(request.parent, "deep-cleanup",
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(child >= 0);
  for (i = 0U; i < 700U; i++) {
    assert(mkdirat(child, "d", 0700) == 0);
    next = openat(child, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(next >= 0 && close(child) == 0);
    child = next;
  }
  assert(symlinkat("/", child, "outside") == 0 && close(child) == 0);
  limited = original;
  limited.rlim_cur = 64U;
  assert(setrlimit(RLIMIT_NOFILE, &limited) == 0);
  assert(test_worker_start(&worker, remove_worker, &request) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(request.code == -1 && request.system_error == EMFILE);
  assert(setrlimit(RLIMIT_NOFILE, &original) == 0);
  /* Retry proves failure unwinds its descriptors as well as heap frames. */
  assert(test_worker_start(&worker, remove_worker, &request) == 0);
  assert(pthread_join(worker, NULL) == 0 && request.code == 0);
  assert(faccessat(request.parent, "deep-cleanup", F_OK, 0) == -1 &&
         errno == ENOENT);
  assert(cai_cli_remove_at(request.parent, ".") == -1 && errno == EINVAL);
  assert(close(request.parent) == 0);
}
#endif

static int exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}
int main(int argc, char **argv) {
  static const char *const answers[] = {"yes\n", "Y\n", "n\n",
                                        "\n",    "",    "not yes\n"};
  cai_cli_options options;
  cai_cli_temp temp, nested;
  cai_error error;
  char original[4096], current[4096], outside[4096], link[4096];
  struct stat st;
  int kept, pipefd[2], saved_input;
  size_t i;
  FILE *file;
  assert(argc == 2 && getcwd(original, sizeof(original)) != NULL);
#ifdef __linux__
  deep_cleanup(argv[1]);
#endif
  snprintf(outside, sizeof(outside), "%s/sentinel", argv[1]);
  file = fopen(outside, "w");
  assert(file != NULL && fputs("outside", file) >= 0 && fclose(file) == 0);
  cai_error_init(&error);
  for (i = 0U; i < sizeof(answers) / sizeof(answers[0]); i++) {
    cai_cli_options_init(&options);
    options.cache_dir = argv[1];
    options.runtime_dir = argv[1];
    options.server_persist = 1;
    cai_cli_temp_init(&temp);
    assert(cai_cli_temp_open(&temp, &options, &error) == CAI_OK);
    assert(cai_cli_temp_runtime_open(&temp, &options, &error) == CAI_OK);
    assert(strcmp(options.runtime_dir, temp.runtime) == 0 &&
           !options.server_persist && exists(temp.runtime));
    assert(stat(temp.root, &st) == 0 && (st.st_mode & 0777U) == 0700U);
    assert(options.lockd == NULL);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, original) == 0);
    snprintf(link, sizeof(link), "%s/outside-link", temp.workspace);
    assert(symlink(outside, link) == 0);
    snprintf(link, sizeof(link), "%s/outside-link", temp.private_dir);
    assert(symlink(outside, link) == 0);
    assert(pipe(pipefd) == 0);
    assert(write(pipefd[1], answers[i], strlen(answers[i])) ==
           (ssize_t)strlen(answers[i]));
    close(pipefd[1]);
    saved_input = dup(STDIN_FILENO);
    assert(saved_input >= 0 && dup2(pipefd[0], STDIN_FILENO) >= 0);
    close(pipefd[0]);
    assert(cai_cli_temp_close(&temp, 0, 1, -1, NULL, NULL, &kept, &error) ==
           CAI_OK);
    assert(dup2(saved_input, STDIN_FILENO) >= 0);
    close(saved_input);
    assert(kept == (i >= 2U));
    assert(!exists(temp.private_dir) && !exists(temp.runtime) &&
           exists(outside));
    assert(exists(temp.workspace) == kept && exists(temp.root) == kept);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, original) == 0);
  }
  cai_cli_options_init(&options);
  options.cache_dir = argv[1];
  cai_cli_temp_init(&temp);
  assert(cai_cli_temp_open(&temp, &options, &error) == CAI_OK);
  options.cache_dir = argv[1];
  cai_cli_temp_init(&nested);
  assert(cai_cli_temp_open(&nested, &options, &error) == CAI_OK);
  assert(cai_cli_temp_close(&nested, 1, 0, -1, NULL, NULL, &kept, &error) ==
             CAI_OK &&
         !kept);
  assert(getcwd(current, sizeof(current)) != NULL &&
         strcmp(current, original) == 0);
  assert(cai_cli_temp_close(&temp, 1, 0, -1, NULL, NULL, &kept, &error) ==
             CAI_OK &&
         !kept);
  assert(getcwd(current, sizeof(current)) != NULL &&
         strcmp(current, original) == 0);
  /* A replaced runtime name must never redirect cleanup into another tree.
   * The opened directory is retained for an explicit caller decision. */
  cai_cli_options_init(&options);
  options.cache_dir = options.runtime_dir = argv[1];
  cai_cli_temp_init(&temp);
  assert(cai_cli_temp_open(&temp, &options, &error) == CAI_OK);
  assert(cai_cli_temp_runtime_open(&temp, &options, &error) == CAI_OK);
  snprintf(link, sizeof(link), "%s/moved-runtime", argv[1]);
  assert(rename(temp.runtime, link) == 0);
  assert(symlink(argv[1], temp.runtime) == 0);
  assert(cai_cli_temp_close(&temp, 1, 0, -1, NULL, NULL, &kept, &error) ==
         CAI_ERR_INVALID);
  assert(kept && exists(outside) && exists(link) && exists(temp.workspace));
  assert(unlink(temp.runtime) == 0 && rmdir(link) == 0);
  cai_error_cleanup(&error);
  options.cache_dir = outside;
  cai_cli_temp_init(&temp);
  assert(cai_cli_temp_open(&temp, &options, &error) != CAI_OK);
  cai_error_cleanup(&error);
  assert(cai_cli_temp_close(&temp, 0, 0, -1, NULL, NULL, &kept, &error) ==
         CAI_OK);
  cai_error_cleanup(&error);
  return 0;
}
