#define _XOPEN_SOURCE 700
#include "cai_process_environment.h"
#include "cai_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cai_process_program(const char *name, char *const *environment,
                        const char *workspace, char path[4096],
                        cai_error *error) {
  const char *paths = "/usr/local/bin:/usr/bin:/bin", *end;
  char candidate[4096];
  size_t i, length;
  int n;
  if (strchr(name, '/') != NULL) {
    n = name[0] == '/'
            ? snprintf(candidate, sizeof(candidate), "%s", name)
            : snprintf(candidate, sizeof(candidate), "%s/%s", workspace, name);
    if (n >= 0 && (size_t)n < sizeof(candidate) &&
        realpath(candidate, path) != NULL && access(path, X_OK) == 0)
      return CAI_OK;
  } else {
    for (i = 0U; environment[i] != NULL; i++)
      if (strncmp(environment[i], "PATH=", 5U) == 0)
        paths = environment[i] + 5U;
    do {
      end = strchr(paths, ':');
      length = end != NULL ? (size_t)(end - paths) : strlen(paths);
      if (length < sizeof(candidate)) {
        if (length == 0U)
          n = snprintf(candidate, sizeof(candidate), "%s/%s", workspace, name);
        else if (paths[0] == '/')
          n = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length,
                       paths, name);
        else
          n = snprintf(candidate, sizeof(candidate), "%s/%.*s/%s", workspace,
                       (int)length, paths, name);
        if (n >= 0 && (size_t)n < sizeof(candidate) &&
            realpath(candidate, path) != NULL && access(path, X_OK) == 0)
          return CAI_OK;
      }
      paths = end != NULL ? end + 1U : NULL;
    } while (paths != NULL);
  }
  return cai_set_error_detail(error, CAI_ERR_INVALID,
                              "child executable is unavailable", name);
}

void cai_process_environment_free(char **environment) {
  size_t i;
  if (environment == NULL)
    return;
  for (i = 0U; environment[i] != NULL; i++)
    cai_free_mem(NULL, environment[i]);
  cai_free_mem(NULL, environment);
}
static int preserved(const char *entry) {
  return strncmp(entry, "HOME=", 5U) == 0 || strncmp(entry, "PATH=", 5U) == 0 ||
         strncmp(entry, "XDG_", 4U) == 0;
}
#ifdef CAI_TEST_THREAD_DEFAULTS
/** Test children need the loader setting even with a filtered/empty
 * environment. Preserve only tunables admitted by the normal policy, replacing
 * the stack-cache setting without making unrelated variables admissible. */
static int debug_thread_tunables(char **entries, size_t *count) {
  static const char name[] = "GLIBC_TUNABLES=";
  static const char key[] = "glibc.pthread.stack_cache_size=";
  static const char setting[] = "glibc.pthread.stack_cache_size=0";
  const char *value = "", *end;
  char *copy, *next;
  size_t index, length;
  for (index = 0U; index < *count; index++)
    if (strncmp(entries[index], name, sizeof(name) - 1U) == 0) {
      value = entries[index] + sizeof(name) - 1U;
      break;
    }
  length = strlen(value);
  if (length > SIZE_MAX - sizeof(name) - sizeof(setting))
    return -1;
  copy = cai_alloc(NULL, length + sizeof(name) + sizeof(setting));
  if (copy == NULL)
    return -1;
  memcpy(copy, name, sizeof(name) - 1U);
  next = copy + sizeof(name) - 1U;
  while (*value != '\0') {
    end = strchr(value, ':');
    length = end != NULL ? (size_t)(end - value) : strlen(value);
    if (length != 0U && !(length >= sizeof(key) - 1U &&
                          memcmp(value, key, sizeof(key) - 1U) == 0)) {
      memcpy(next, value, length);
      next += length;
      *next++ = ':';
    }
    if (end == NULL)
      break;
    value = end + 1;
  }
  memcpy(next, setting, sizeof(setting));
  if (index < *count)
    cai_free_mem(NULL, entries[index]);
  else
    (*count)++;
  entries[index] = copy;
  return 0;
}
#endif
int cai_process_environment_new(const cai_environment *base, int yolo,
                                const char *const *defaults, char ***out,
                                cai_error *error) {
  size_t count = 0U, extra = 0U, i, j, used = 0U, spare = 1U;
  cai_environment *captured = NULL;
  const char *const *entries;
  char **copy;
  int rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "command environment output is required");
  *out = NULL;
  if (base == NULL) {
    rc = cai_environment_open(NULL, &captured, error);
    if (rc != CAI_OK)
      return rc;
    base = captured;
  }
  entries = cai_environment_entries(base);
  count = cai_environment_count(base);
  if (!yolo && defaults != NULL)
    while (defaults[extra] != NULL)
      extra++;
#ifdef CAI_TEST_THREAD_DEFAULTS
  spare++; /* Loader setting plus the terminator. */
#endif
  if (extra > SIZE_MAX / sizeof(*copy) - spare ||
      count > SIZE_MAX / sizeof(*copy) - extra - spare) {
    cai_environment_close(captured);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "command environment is too large");
  }
  copy = cai_alloc(NULL, (count + extra + spare) * sizeof(*copy));
  if (copy == NULL) {
    cai_environment_close(captured);
    return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
  }
  memset(copy, 0, (count + extra + spare) * sizeof(*copy));
  for (i = 0U; i < count; i++) {
    if (!yolo && !preserved(entries[i]))
      continue;
    copy[used] = cai_strdup(NULL, entries[i]);
    if (copy[used] == NULL)
      goto failed;
    used++;
  }
  for (i = 0U; i < extra; i++) {
    const char *equal = strchr(defaults[i], '=');
    size_t length = equal != NULL ? (size_t)(equal - defaults[i]) + 1U : 0U;
    for (j = 0U; j < used; j++)
      if (length != 0U && strncmp(copy[j], defaults[i], length) == 0)
        break;
    if (j != used)
      continue;
    copy[used] = cai_strdup(NULL, defaults[i]);
    if (copy[used] == NULL)
      goto failed;
    used++;
  }
#ifdef CAI_TEST_THREAD_DEFAULTS
  if (debug_thread_tunables(copy, &used) != 0)
    goto failed;
#endif
  *out = copy;
  cai_environment_close(captured);
  return CAI_OK;
failed:
  cai_environment_close(captured);
  cai_process_environment_free(copy);
  return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
}
