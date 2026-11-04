#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/config.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void join(char out[4096], const char *base, const char *child) {
  int n = snprintf(out, 4096U, "%s/%s", base, child);
  assert(n > 0 && n < 4096);
}
static void write_config(const char *directory, const char *value) {
  char path[4096];
  FILE *file;
  join(path, directory, "config.ini");
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fprintf(file,
                 "[client]\nca_path=certs\n[env]\nADDED=%s\n"
                 "HOME=/configured\nKEEP=not-empty\n",
                 value) > 0);
  assert(fclose(file) == 0);
}
static void remove_config(const char *directory) {
  char path[4096];
  join(path, directory, "config.ini");
  assert(unlink(path) == 0 && rmdir(directory) == 0);
}
static void *contexts(void *context) {
  static const char *const values_a[] = {
      "HOME=/home/a", "CAI_TEST_SCOPE=host-a", "KEEP=", NULL};
  static const char *const values_b[] = {"HOME=/home/b",
                                         "CAI_TEST_SCOPE=host-b", NULL};
  char root[4096], a[4096], b[4096], cwd[4096], current[4096], expected[4096];
  cai_config_context options;
  cai_config *first = NULL, *second = NULL, *invalid = NULL;
  cai_environment *environment_a = NULL, *environment_b = NULL;
  cai_environment *snapshot = NULL, *later = NULL;
  const char *value, *previous = getenv("CAI_TEST_SCOPE");
  char *saved = previous != NULL ? strdup(previous) : NULL;
  int n;
  assert(previous == NULL || saved != NULL);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  n = snprintf(root, sizeof(root), "%s/config-context-XXXXXX",
               (const char *)context);
  assert(n > 0 && (size_t)n < sizeof(root) && mkdtemp(root) != NULL);
  join(a, root, "a");
  join(b, root, "b");
  assert(mkdir(a, 0700) == 0 && mkdir(b, 0700) == 0);
  write_config(a, "only-a");
  write_config(b, "only-b");
  assert(cai_environment_open(values_a, &environment_a, NULL) == CAI_OK);
  assert(cai_environment_open(values_b, &environment_b, NULL) == CAI_OK);
  options.directory = a;
  options.environment = environment_a;
  assert(cai_config_open(NULL, 0U, &options, &first, NULL) == CAI_OK);
  options.directory = b;
  options.environment = environment_b;
  assert(cai_config_open(NULL, 0U, &options, &second, NULL) == CAI_OK);
  cai_environment_close(environment_a);
  cai_environment_close(environment_b);
  assert(setenv("CAI_TEST_SCOPE", "host-changed", 1) == 0);
  assert(chdir(b) == 0);
  /* Reads and config-relative paths use A while the host happens to be in B. */
  assert(cai_config_load_file(first, "config.ini", NULL) == CAI_OK);
  assert(getcwd(current, sizeof(current)) != NULL && strcmp(current, b) == 0);
  assert(cai_config_resolve_path(first, "client", "ca_path", &value, NULL) ==
         CAI_OK);
  join(expected, a, "certs");
  assert(strcmp(value, expected) == 0);
  assert(strcmp(cai_config_directory(first), a) == 0);
  assert(strcmp(cai_config_getenv(first, "CAI_TEST_SCOPE"), "host-a") == 0);
  assert(cai_config_getenv(first, "KEEP")[0] == '\0');
  assert(strcmp(cai_config_getenv(first, "ADDED"), "only-a") == 0);
  assert(first->environment(first, &snapshot, NULL) == CAI_OK);
  assert(strcmp(cai_environment_get(snapshot, "ADDED"), "only-a") == 0);
  assert(cai_config_set(first, "env", "ADDED", "later-a", NULL) == CAI_OK);
  assert(strcmp(cai_config_getenv(first, "ADDED"), "later-a") == 0);
  assert(strcmp(cai_environment_get(snapshot, "ADDED"), "only-a") == 0);
  assert(cai_config_set(first, "client", "ca_bundle_path", "~/bundle", NULL) ==
         CAI_OK);
  assert(cai_config_resolve_path(first, "client", "ca_bundle_path", &value,
                                 NULL) == CAI_OK);
  assert(strcmp(value, "/home/a/bundle") == 0);
  assert(cai_config_set(first, "", "config_env_has_precedence", "true", NULL) ==
         CAI_OK);
  assert(strcmp(cai_config_getenv(first, "HOME"), "/configured") == 0);
  assert(cai_config_environment(first, &later, NULL) == CAI_OK);
  assert(strcmp(cai_environment_get(later, "KEEP"), "not-empty") == 0);
  assert(strcmp(cai_environment_get(later, "HOME"), "/configured") == 0);
  assert(strcmp(cai_environment_get(snapshot, "HOME"), "/home/a") == 0);
  assert(chdir(a) == 0);
  assert(cai_config_load_file(second, "config.ini", NULL) == CAI_OK);
  assert(cai_config_resolve_path(second, "client", "ca_path", &value, NULL) ==
         CAI_OK);
  join(expected, b, "certs");
  assert(strcmp(value, expected) == 0);
  assert(strcmp(cai_config_getenv(second, "ADDED"), "only-b") == 0);
  assert(strcmp(cai_config_getenv(second, "CAI_TEST_SCOPE"), "host-b") == 0);
  assert(getcwd(current, sizeof(current)) != NULL && strcmp(current, a) == 0);
  assert(strcmp(getenv("CAI_TEST_SCOPE"), "host-changed") == 0);
  options.directory = "relative";
  options.environment = NULL;
  assert(cai_config_open(NULL, 0U, &options, &invalid, NULL) ==
             CAI_ERR_INVALID &&
         invalid == NULL);
  cai_config_close(first);
  cai_config_close(second);
  assert(strcmp(cai_environment_get(snapshot, "ADDED"), "only-a") == 0);
  cai_environment_close(snapshot);
  cai_environment_close(later);
  assert(chdir(cwd) == 0);
  assert((saved != NULL ? setenv("CAI_TEST_SCOPE", saved, 1)
                        : unsetenv("CAI_TEST_SCOPE")) == 0);
  free(saved);
  remove_config(a);
  remove_config(b);
  assert(rmdir(root) == 0);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t worker;
  assert(argc == 2);
  assert(pthread_create(&worker, NULL, contexts, argv[1]) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
