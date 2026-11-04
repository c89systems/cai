#include "cai_process_environment.h"
#include "cai_internal.h"
#include <string.h>

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
int cai_process_environment_new(const cai_environment *base, int yolo,
                                const char *const *defaults, char ***out,
                                cai_error *error) {
  size_t count = 0U, extra = 0U, i, j, used = 0U;
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
  if (extra >= SIZE_MAX / sizeof(*copy) ||
      count > SIZE_MAX / sizeof(*copy) - extra - 1U) {
    cai_environment_close(captured);
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "command environment is too large");
  }
  copy = cai_alloc(NULL, (count + extra + 1U) * sizeof(*copy));
  if (copy == NULL) {
    cai_environment_close(captured);
    return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
  }
  memset(copy, 0, (count + extra + 1U) * sizeof(*copy));
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
  *out = copy;
  cai_environment_close(captured);
  return CAI_OK;
failed:
  cai_environment_close(captured);
  cai_process_environment_free(copy);
  return cai_set_error(error, CAI_ERR_NOMEM, "copy command environment");
}
