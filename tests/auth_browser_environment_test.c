/** Prove OAuth browser launch uses independent environment/cwd views on
 * default-size workers, and resets signals only in the exec child. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/auth.h>
#include <cai/environment.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char url[] = "https://example.test/authorize?a=1;touch nope";
typedef struct fixture {
  char directory[4096], executable[4096], result[8192], expected[8192];
  const char *slot;
} fixture;

static int child(void) {
  struct sigaction action;
  sigset_t mask;
  FILE *file;
  char *cwd = getcwd(NULL, 0U);
  assert(cwd != NULL && getenv("LD_PRELOAD") == NULL);
  assert(sigaction(SIGPIPE, NULL, &action) == 0 &&
         action.sa_handler == SIG_DFL);
  assert(pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
         !sigismember(&mask, SIGINT));
  file = fopen("browser-result", "wb");
  assert(file != NULL);
  assert(fprintf(file, "%s\n%s\n%s\n%s\n%s\n", url, getenv("UTILITY_SLOT"),
                 getenv("HOME"), getenv("XDG_CACHE_HOME"), cwd) > 0);
  assert(fclose(file) == 0);
  free(cwd);
  file = fopen("browser-done", "wb");
  assert(file != NULL && fclose(file) == 0);
  return 0;
}
static void check_output(fixture *f) {
  char path[4096];
  struct timespec delay = {0, 1000000L};
  FILE *file;
  size_t length;
  unsigned int i;
  assert(snprintf(path, sizeof(path), "%s/browser-done", f->directory) <
         (int)sizeof(path));
  for (i = 0U; i < 2000U && access(path, F_OK) != 0; ++i)
    (void)nanosleep(&delay, NULL);
  assert(i < 2000U && unlink(path) == 0);
  assert(snprintf(path, sizeof(path), "%s/browser-result", f->directory) <
         (int)sizeof(path));
  file = fopen(path, "rb");
  assert(file != NULL);
  length = fread(f->result, 1U, sizeof(f->result) - 1U, file);
  assert(!ferror(file) && fclose(file) == 0);
  f->result[length] = '\0';
  assert(!strcmp(f->result, f->expected));
  assert(unlink(path) == 0);
}
static void *run(void *context) {
  fixture *f = context;
  cai_environment *base, *view;
  cai_environment_change change;
  cai_chatgpt_login_browser_config config;
  const char *entries[6];
  const char *paths[6];
  char slot[64], absolute[4096];
  char *long_path = malloc(5000U);
  size_t i;
  assert(long_path != NULL);
  memset(long_path, 'x', 4500U);
  strcpy(long_path + 4500U, ":bin");
  assert(snprintf(slot, sizeof(slot), "UTILITY_SLOT=%s", f->slot) <
         (int)sizeof(slot));
  entries[0] = "PATH=bin";
  entries[1] = "HOME=/browser-home";
  entries[2] = "XDG_CACHE_HOME=/browser-cache";
  entries[3] = slot;
  entries[4] = "LD_PRELOAD=/no/such/preload.so";
  entries[5] = NULL;
  assert(cai_environment_open(entries, &base, NULL) == CAI_OK);
  assert(snprintf(absolute, sizeof(absolute), "%s/bin", f->directory) <
         (int)sizeof(absolute));
  paths[0] = "missing:bin";
  paths[1] = "";
  paths[2] = absolute;
  paths[3] = "bin";
  paths[4] = "missing";
  paths[5] = long_path;
  assert(snprintf(f->expected, sizeof(f->expected),
                  "%s\n%s\n/browser-home\n/browser-cache\n%s\n", url, f->slot,
                  f->directory) < (int)sizeof(f->expected));
  cai_chatgpt_login_browser_config_init(&config);
  config.directory = f->directory;
  change.name = "PATH";
  for (i = 0U; i < 6U; ++i) {
    change.value = paths[i];
    assert(cai_environment_derive(base, &change, 1U, &view, NULL) == CAI_OK);
    config.environment = view;
    config.command = i == 3U   ? "./bin/cai-browser-child"
                     : i == 4U ? f->executable
                               : "cai-browser-child";
    assert(cai_chatgpt_login_open_browser_with_config(&config, url, NULL) ==
           CAI_OK);
    check_output(f);
    assert(strcmp(cai_environment_get(view, "LD_PRELOAD"),
                  "/no/such/preload.so") == 0);
    cai_environment_close(view);
  }
  config.environment = base;
  config.command = "unavailable-browser";
  assert(cai_chatgpt_login_open_browser_with_config(&config, url, NULL) ==
         CAI_ERR_TRANSPORT);
  config.command = "cai-browser-child";
  config.directory = ".";
  assert(cai_chatgpt_login_open_browser_with_config(&config, url, NULL) ==
         CAI_ERR_INVALID);
  config.directory = "/no/such/browser-directory";
  assert(cai_chatgpt_login_open_browser_with_config(&config, url, NULL) ==
         CAI_ERR_INVALID);
  cai_environment_close(base);
  free(long_path);
  return NULL;
}
int main(int argc, char **argv) {
  fixture *f;
  char *root, *self, *cwd, *after_cwd;
  char path[4096];
  cai_environment *before, *after;
  const char *const *a, *const *b;
  struct sigaction ignored, original, current;
  sigset_t blocked, previous, current_mask;
  pthread_t workers[2];
  size_t i;
  if (argc == 2 && !strcmp(argv[1], url))
    return child();
  assert(argc == 2);
  root = malloc(4096U);
  f = calloc(2U, sizeof(*f));
  self = realpath(argv[0], NULL);
  cwd = getcwd(NULL, 0U);
  assert(root != NULL && f != NULL && self != NULL && cwd != NULL);
  assert(snprintf(root, 4096U, "%s/browser-view-XXXXXX", argv[1]) < 4096);
  assert(mkdtemp(root) != NULL);
  memset(&ignored, 0, sizeof(ignored));
  ignored.sa_handler = SIG_IGN;
  assert(sigemptyset(&ignored.sa_mask) == 0);
  assert(sigaction(SIGPIPE, &ignored, &original) == 0);
  assert(sigemptyset(&blocked) == 0 && sigaddset(&blocked, SIGINT) == 0);
  assert(pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0);
  assert(cai_environment_open(NULL, &before, NULL) == CAI_OK);
  for (i = 0U; i < 2U; ++i) {
    f[i].slot = i == 0U ? "one" : "two";
    assert(snprintf(f[i].directory, sizeof(f[i].directory), "%s/%s", root,
                    f[i].slot) < (int)sizeof(f[i].directory));
    assert(mkdir(f[i].directory, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/bin", f[i].directory) <
           (int)sizeof(path));
    assert(mkdir(path, 0700) == 0);
    assert(snprintf(f[i].executable, sizeof(f[i].executable),
                    "%s/bin/cai-browser-child",
                    f[i].directory) < (int)sizeof(f[i].executable));
    assert(symlink(self, f[i].executable) == 0);
    assert(snprintf(path, sizeof(path), "%s/cai-browser-child",
                    f[i].directory) < (int)sizeof(path));
    assert(symlink(self, path) == 0);
    assert(pthread_create(&workers[i], NULL, run, &f[i]) == 0);
  }
  for (i = 0U; i < 2U; ++i)
    assert(pthread_join(workers[i], NULL) == 0);
  assert(cai_environment_open(NULL, &after, NULL) == CAI_OK);
  assert(cai_environment_count(before) == cai_environment_count(after));
  a = cai_environment_entries(before);
  b = cai_environment_entries(after);
  for (i = 0U; a[i] != NULL; ++i)
    assert(!strcmp(a[i], b[i]));
  after_cwd = getcwd(NULL, 0U);
  assert(after_cwd != NULL && !strcmp(cwd, after_cwd));
  assert(sigaction(SIGPIPE, NULL, &current) == 0 &&
         current.sa_handler == SIG_IGN);
  assert(pthread_sigmask(SIG_SETMASK, NULL, &current_mask) == 0 &&
         sigismember(&current_mask, SIGINT));
  assert(sigaction(SIGPIPE, &original, NULL) == 0);
  assert(pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0);
  cai_environment_close(before);
  cai_environment_close(after);
  for (i = 0U; i < 2U; ++i) {
    assert(unlink(f[i].executable) == 0);
    assert(snprintf(path, sizeof(path), "%s/bin", f[i].directory) <
           (int)sizeof(path));
    assert(rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/cai-browser-child",
                    f[i].directory) < (int)sizeof(path));
    assert(unlink(path) == 0 && rmdir(f[i].directory) == 0);
  }
  assert(rmdir(root) == 0);
  free(after_cwd);
  free(cwd);
  free(self);
  free(f);
  free(root);
  return 0;
}
