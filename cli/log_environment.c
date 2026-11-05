#define _POSIX_C_SOURCE 200809L
#include "log_environment.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/** The optional tee owns only its file branch. The seed output belongs to the
 * application log context, which outlives logger destruction. */
typedef struct log_tee {
  pslog_output first, second;
} log_tee;
static int tee_write(void *context, const char *bytes, size_t count,
                     size_t *written) {
  log_tee *t = context;
  size_t first = 0U, second = 0U;
  int rc = t->first.write(t->first.userdata, bytes, count, &first);
  *written = 0U;
  if (rc != 0)
    return rc;
  while (second < first) {
    size_t n = 0U;
    rc =
        t->second.write(t->second.userdata, bytes + second, first - second, &n);
    if (rc != 0 || n == 0U)
      return rc != 0 ? rc : EIO;
    second += n;
  }
  *written = first;
  return 0;
}
static int tee_close(void *context) {
  log_tee *t = context;
  int rc = t->second.close != NULL ? t->second.close(t->second.userdata) : 0;
  free(t);
  return rc;
}
static int tee_tty(void *context) {
  log_tee *t = context;
  return t->first.isatty != NULL && t->first.isatty(t->first.userdata);
}
static char *trim(const char *value) {
  size_t n;
  char *out;
  if (value == NULL)
    value = "";
  while (isspace((unsigned char)*value))
    ++value;
  n = strlen(value);
  while (n && isspace((unsigned char)value[n - 1U]))
    --n;
  out = malloc(n + 1U);
  if (out != NULL) {
    memcpy(out, value, n);
    out[n] = '\0';
  }
  return out;
}
static int boolean(const cai_environment *env, const char *key, int fallback) {
  char *value = trim(cai_environment_get(env, key));
  int result = fallback;
  if (value != NULL) {
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0)
      result = 1;
    else if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
             strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0)
      result = 0;
  }
  free(value);
  return result;
}
static int file_output(pslog_output *out, const char *directory,
                       const char *path, unsigned int mode) {
  int parent, fd, saved;
  FILE *file;
  parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0)
    return errno;
  fd = openat(parent, path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, mode);
  saved = errno;
  close(parent);
  if (fd < 0)
    return saved;
  file = fdopen(fd, "ab");
  if (file == NULL) {
    saved = errno;
    close(fd);
    return saved;
  }
  *out = pslog_output_from_fp(file, 1);
  return 0;
}
static int output(pslog_output *out, const char *directory, const char *value,
                  unsigned int mode, int *replaced) {
  pslog_output first = *out, second;
  const char *path = value;
  log_tee *tee;
  int paired = 0, rc;
  if (*value == '\0' || strcasecmp(value, "default") == 0)
    return 0;
  if (strcasecmp(value, "stdout") == 0 || strcasecmp(value, "stderr") == 0) {
    *out = pslog_output_from_fp(
        strcasecmp(value, "stdout") == 0 ? stdout : stderr, 0);
    *replaced = 1;
    return 0;
  }
  if (strncasecmp(path, "stdout+", 7U) == 0 ||
      strncasecmp(path, "stderr+", 7U) == 0 ||
      strncasecmp(path, "default+", 8U) == 0) {
    if (strncasecmp(path, "stdout+", 7U) == 0)
      first = pslog_output_from_fp(stdout, 0);
    else if (strncasecmp(path, "stderr+", 7U) == 0)
      first = pslog_output_from_fp(stderr, 0);
    path += strncasecmp(path, "default+", 8U) == 0 ? 8U : 7U;
    while (isspace((unsigned char)*path))
      ++path;
    paired = 1;
    if (*path == '\0') {
      *out = first;
      *replaced = 1;
      return 0;
    }
  }
  rc = file_output(&second, directory, path, mode);
  if (rc != 0)
    return rc;
  if (!paired)
    *out = second;
  else {
    tee = calloc(1U, sizeof(*tee));
    if (tee == NULL) {
      pslog_output_destroy(&second);
      return ENOMEM;
    }
    tee->first = first;
    tee->second = second;
    out->write = tee_write;
    out->close = tee_close;
    out->isatty = tee_tty;
    out->userdata = tee;
    out->owned = 1;
  }
  *replaced = 1;
  return 0;
}
pslog_logger *cai_cli_logger_from_environment(const pslog_config *seed,
                                              const cai_environment *env,
                                              const char *directory) {
  pslog_config config = *seed;
  pslog_logger *logger;
  char *mode = trim(cai_environment_get(env, "LOG_MODE"));
  char *path = trim(cai_environment_get(env, "LOG_OUTPUT"));
  char *permissions = trim(cai_environment_get(env, "LOG_OUTPUT_FILE_MODE"));
  char *palette = trim(cai_environment_get(env, "LOG_PALETTE"));
  const char *value;
  unsigned long file_mode = 0600U;
  int replaced = 0, output_error = 0, mode_error = 0;
  if (mode == NULL || path == NULL || permissions == NULL || palette == NULL) {
    free(mode);
    free(path);
    free(permissions);
    free(palette);
    return NULL;
  }
  value = cai_environment_get(env, "LOG_LEVEL");
  if (value != NULL)
    (void)pslog_parse_level(value, &config.min_level);
  if (strcasecmp(mode, "console") == 0)
    config.mode = PSLOG_MODE_CONSOLE;
  else if (strcasecmp(mode, "json") == 0 || strcasecmp(mode, "structured") == 0)
    config.mode = PSLOG_MODE_JSON;
  config.timestamps =
      !boolean(env, "LOG_DISABLE_TIMESTAMP", !config.timestamps);
  config.verbose_fields =
      boolean(env, "LOG_VERBOSE_FIELDS", config.verbose_fields);
  config.utc = boolean(env, "LOG_UTC", config.utc);
  if (boolean(env, "LOG_NO_COLOR", 0))
    config.color = PSLOG_COLOR_NEVER;
  else if (boolean(env, "LOG_FORCE_COLOR", 0) &&
           config.color != PSLOG_COLOR_NEVER)
    config.color = PSLOG_COLOR_ALWAYS;
  value = cai_environment_get(env, "LOG_TIME_FORMAT");
  if (value != NULL && *value != '\0')
    config.time_format = value;
  if (*palette != '\0')
    config.palette = pslog_palette_by_name(palette);
  if (*permissions != '\0') {
    char *end, *start = permissions;
    if (start[0] == '0' && (start[1] == 'o' || start[1] == 'O'))
      start += 2;
    errno = 0;
    file_mode = strtoul(start, &end, 8);
    if (errno || *start == '\0' || *end != '\0' || file_mode > 0777U ||
        *start == '+' || *start == '-') {
      mode_error = 1;
      file_mode = 0600U;
    }
  }
  output_error = output(&config.output, directory, path,
                        (unsigned int)file_mode, &replaced);
  logger = pslog_new(&config);
  if (logger == NULL && replaced)
    pslog_output_destroy(&config.output);
  if (logger != NULL && mode_error) {
    pslog_field field = pslog_str("output_file_mode", permissions);
    logger->error(logger, "logger.output.file_mode.invalid", &field, 1U);
  }
  if (logger != NULL && output_error) {
    pslog_field fields[2];
    fields[0] = pslog_str("output", path);
    fields[1] = pslog_str("error", strerror(output_error));
    logger->error(logger, "logger.output.open.failed", fields, 2U);
  }
  free(mode);
  free(path);
  free(permissions);
  free(palette);
  return logger;
}
