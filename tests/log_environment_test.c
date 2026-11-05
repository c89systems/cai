#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/log_environment.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Compare the public dependency overlay with the captured application
 * overlay. Only this isolated test process changes its environment; the
 * captured constructor runs after those values have deliberately changed. */
static const char *const keys[] = {"LOG_MODE",
                                   "LOG_LEVEL",
                                   "LOG_DISABLE_TIMESTAMP",
                                   "LOG_VERBOSE_FIELDS",
                                   "LOG_UTC",
                                   "LOG_TIME_FORMAT",
                                   "LOG_NO_COLOR",
                                   "LOG_FORCE_COLOR",
                                   "LOG_PALETTE",
                                   "LOG_OUTPUT",
                                   "LOG_OUTPUT_FILE_MODE"};
typedef struct fixture {
  char *directory;
  cai_environment *environment;
  pslog_config config;
} fixture;
static void emit(pslog_logger *logger) {
  pslog_field field = pslog_str("entry", "captured environment");
  assert(logger != NULL);
  logger->info(logger, "LOG_ENV_INFO", &field, 1U);
  logger->warn(logger, "LOG_ENV_WARN", &field, 1U);
  assert(logger->close(logger) == 0);
  logger->destroy(logger);
}
static void *captured(void *context) {
  fixture *f = context;
  emit(cai_cli_logger_from_environment(&f->config, f->environment,
                                       f->directory));
  return NULL;
}
static void compare(const char *a, const char *b) {
  FILE *left = fopen(a, "rb"), *right = fopen(b, "rb");
  int x, y;
  assert(left != NULL && right != NULL);
  do {
    x = fgetc(left);
    y = fgetc(right);
    assert(x == y);
  } while (x != EOF);
  assert(!ferror(left) && !ferror(right));
  assert(fclose(left) == 0 && fclose(right) == 0);
}
int main(void) {
  fixture f;
  char directory[] = "log-environment.XXXXXX";
  char reference[4096], result[4096], route[4096];
  const char *const variants[][2] = {{"LOG_MODE", "console"},
                                     {"LOG_MODE", "structured"},
                                     {"LOG_LEVEL", "warn"},
                                     {"LOG_LEVEL", "invalid"},
                                     {"LOG_VERBOSE_FIELDS", " yes "},
                                     {"LOG_DISABLE_TIMESTAMP", "true"},
                                     {"LOG_UTC", "on"},
                                     {"LOG_FORCE_COLOR", "true"},
                                     {"LOG_NO_COLOR", "true"},
                                     {"LOG_OUTPUT_FILE_MODE", "0o640"},
                                     {"LOG_PALETTE", " dracula "},
                                     {"LOG_TIME_FORMAT", "%Y"}};
  FILE *discard = fopen("/dev/null", "wb");
  size_t i, j;
  int paired;
  assert(discard != NULL && mkdtemp(directory) != NULL);
  memset(&f, 0, sizeof(f));
  f.directory = realpath(directory, NULL);
  assert(f.directory != NULL);
  snprintf(reference, sizeof(reference), "%s/reference.log", f.directory);
  snprintf(result, sizeof(result), "%s/captured.log", f.directory);
  for (paired = 0; paired < 2; ++paired)
    for (i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i) {
      cai_environment *base;
      cai_environment_change change;
      pthread_t worker;
      struct stat a, b;
      for (j = 0; j < sizeof(keys) / sizeof(keys[0]); ++j)
        assert(unsetenv(keys[j]) == 0);
      assert(setenv(variants[i][0], variants[i][1], 1) == 0);
      if (strcmp(variants[i][0], "LOG_NO_COLOR") == 0)
        assert(setenv("LOG_FORCE_COLOR", "true", 1) == 0);
      snprintf(route, sizeof(route), "%s%s", paired ? "default+" : "",
               reference);
      assert(setenv("LOG_OUTPUT", route, 1) == 0);
      pslog_default_config(&f.config);
      f.config.mode = PSLOG_MODE_JSON;
      f.config.min_level = PSLOG_LEVEL_INFO;
      f.config.timestamps = 0;
      f.config.color = PSLOG_COLOR_AUTO;
      f.config.output = pslog_output_from_fp(discard, 0);
      emit(pslog_new_from_env("LOG_", &f.config));
      assert(cai_environment_open(NULL, &base, NULL) == CAI_OK);
      change.name = "LOG_OUTPUT";
      change.value = paired ? "default+captured.log" : "captured.log";
      assert(cai_environment_derive(base, &change, 1U, &f.environment, NULL) ==
             CAI_OK);
      cai_environment_close(base);
      assert(setenv("LOG_MODE", "console", 1) == 0 &&
             setenv("LOG_LEVEL", "off", 1) == 0 &&
             setenv("LOG_OUTPUT", "/not/a/real/output/file", 1) == 0);
      assert(pthread_create(&worker, NULL, captured, &f) == 0);
      assert(pthread_join(worker, NULL) == 0);
      cai_environment_close(f.environment);
      compare(reference, result);
      assert(stat(reference, &a) == 0 && stat(result, &b) == 0 &&
             (a.st_mode & 0777U) == (b.st_mode & 0777U));
      assert(unlink(reference) == 0 && unlink(result) == 0);
      assert(fputs("seed output remains borrowed", discard) >= 0);
    }
  assert(fclose(discard) == 0 && rmdir(directory) == 0);
  free(f.directory);
  return 0;
}
