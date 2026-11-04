#include "cai_internal.h"
#include <cai/environment.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
struct cai_environment {
  pthread_mutex_t lock;
  char **entries;
  size_t count, references;
};
typedef struct environment_item {
  const char *name, *value;
  size_t length, order;
} environment_item;
static int compare_name(const char *a, size_t na, const char *b, size_t nb) {
  int rc = memcmp(a, b, na < nb ? na : nb);
  return rc != 0 ? rc : na < nb ? -1 : na > nb ? 1 : 0;
}
static int compare_item(const void *first, const void *second) {
  const environment_item *a = first, *b = second;
  int rc = compare_name(a->name, a->length, b->name, b->length);
  return rc != 0 ? rc : a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}
static int allocate(size_t capacity, cai_environment **out, cai_error *error) {
  cai_environment *environment;
  if (capacity >= SIZE_MAX / sizeof(char *))
    return cai_set_error(error, CAI_ERR_LIMIT, "environment is too large");
  environment = cai_alloc(NULL, sizeof(*environment));
  if (environment == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate environment view");
  memset(environment, 0, sizeof(*environment));
  if (pthread_mutex_init(&environment->lock, NULL) != 0) {
    cai_free_mem(NULL, environment);
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "initialize environment references");
  }
  environment->references = 1U;
  environment->entries = cai_alloc(NULL, (capacity + 1U) * sizeof(char *));
  if (environment->entries == NULL) {
    cai_environment_close(environment);
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate environment entries");
  }
  memset(environment->entries, 0, (capacity + 1U) * sizeof(char *));
  *out = environment;
  return CAI_OK;
}
static int append(cai_environment *environment, const char *name, size_t length,
                  const char *value, cai_error *error) {
  size_t n = strlen(value);
  char *entry;
  if (length > SIZE_MAX - 2U || n > SIZE_MAX - length - 2U)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "environment entry is too large");
  entry = cai_alloc(NULL, length + n + 2U);
  if (entry == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy environment entry");
  memcpy(entry, name, length);
  entry[length] = '=';
  memcpy(entry + length + 1U, value, n + 1U);
  environment->entries[environment->count++] = entry;
  return CAI_OK;
}
int cai_environment_open(const char *const *entries, cai_environment **out,
                         cai_error *error) {
  cai_environment *environment = NULL;
  environment_item *items = NULL;
  size_t count = 0U, i;
  int rc;
  if (out == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "environment output is required");
  *out = NULL;
  if (entries == NULL)
    entries = (const char *const *)(void *)environ;
  while (entries != NULL && entries[count] != NULL) {
    if (count == SIZE_MAX / sizeof(*items) - 1U)
      return cai_set_error(error, CAI_ERR_LIMIT, "environment is too large");
    ++count;
  }
  if (count != 0U) {
    items = cai_alloc(NULL, count * sizeof(*items));
    if (items == NULL)
      return cai_set_error(error, CAI_ERR_NOMEM, "index environment names");
  }
  for (i = 0U; i < count; ++i) {
    const char *equal = strchr(entries[i], '=');
    if (equal == NULL || equal == entries[i]) {
      cai_free_mem(NULL, items);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "environment entry requires a nonempty name");
    }
    items[i].name = entries[i];
    items[i].length = (size_t)(equal - entries[i]);
    items[i].value = equal + 1U;
    items[i].order = i;
  }
  if (count != 0U)
    qsort(items, count, sizeof(*items), compare_item);
  rc = allocate(count, &environment, error);
  for (i = 0U; rc == CAI_OK && i < count; ++i)
    if (i == 0U || compare_name(items[i - 1U].name, items[i - 1U].length,
                                items[i].name, items[i].length) != 0)
      rc = append(environment, items[i].name, items[i].length, items[i].value,
                  error);
  cai_free_mem(NULL, items);
  if (rc == CAI_OK)
    *out = environment;
  else
    cai_environment_close(environment);
  return rc;
}
int cai_environment_derive(cai_environment *base,
                           const cai_environment_change *changes, size_t count,
                           cai_environment **out, cai_error *error) {
  cai_environment *environment = NULL;
  environment_item *items;
  size_t i, at = 0U, input = 0U;
  int rc;
  if (out != NULL)
    *out = NULL;
  if (base == NULL || out == NULL || (changes == NULL && count != 0U))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "environment base, changes and output are required");
  if (count == 0U) {
    rc = cai_environment_retain(base, error);
    if (rc == CAI_OK)
      *out = base;
    return rc;
  }
  if (count > SIZE_MAX / sizeof(*items) || count > SIZE_MAX - base->count)
    return cai_set_error(error, CAI_ERR_LIMIT,
                         "environment changes are too large");
  items = cai_alloc(NULL, count * sizeof(*items));
  if (items == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "index environment changes");
  for (i = 0U; i < count; ++i) {
    if (changes[i].name == NULL || changes[i].name[0] == '\0' ||
        strchr(changes[i].name, '=') != NULL) {
      cai_free_mem(NULL, items);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "environment name must be nonempty without '='");
    }
    items[i].name = changes[i].name;
    items[i].length = strlen(changes[i].name);
    items[i].value = changes[i].value;
    items[i].order = count - i; /* Last assignment wins within a name. */
  }
  qsort(items, count, sizeof(*items), compare_item);
  rc = allocate(base->count + count, &environment, error);
  while (rc == CAI_OK && (at < count || input < base->count)) {
    const char *entry = input < base->count ? base->entries[input] : NULL;
    const char *equal = entry != NULL ? strchr(entry, '=') : NULL;
    size_t length = entry != NULL ? (size_t)(equal - entry) : 0U;
    int compare =
        at == count ? 1
        : entry == NULL
            ? -1
            : compare_name(items[at].name, items[at].length, entry, length);
    if (compare > 0) {
      rc = append(environment, entry, length, equal + 1U, error);
      ++input;
    } else {
      size_t next = at + 1U;
      if (items[at].value != NULL)
        rc = append(environment, items[at].name, items[at].length,
                    items[at].value, error);
      if (compare == 0)
        ++input;
      while (next < count &&
             compare_name(items[at].name, items[at].length, items[next].name,
                          items[next].length) == 0)
        ++next;
      at = next;
    }
  }
  cai_free_mem(NULL, items);
  if (rc == CAI_OK)
    *out = environment;
  else
    cai_environment_close(environment);
  return rc;
}
const char *cai_environment_get(const cai_environment *environment,
                                const char *name) {
  size_t low = 0U, high, length;
  if (environment == NULL || name == NULL || name[0] == '\0' ||
      strchr(name, '=') != NULL)
    return NULL;
  high = environment->count;
  length = strlen(name);
  while (low < high) {
    size_t middle = low + (high - low) / 2U;
    const char *entry = environment->entries[middle];
    const char *equal = strchr(entry, '=');
    int rc = compare_name(name, length, entry, (size_t)(equal - entry));
    if (rc == 0)
      return equal + 1U;
    if (rc < 0)
      high = middle;
    else
      low = middle + 1U;
  }
  return NULL;
}
size_t cai_environment_count(const cai_environment *environment) {
  return environment != NULL ? environment->count : 0U;
}
const char *const *cai_environment_entries(const cai_environment *environment) {
  return environment != NULL ? (const char *const *)(void *)environment->entries
                             : NULL;
}
int cai_environment_retain(cai_environment *environment, cai_error *error) {
  int rc = CAI_OK;
  if (environment == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "environment is required");
  pthread_mutex_lock(&environment->lock);
  if (environment->references == SIZE_MAX)
    rc =
        cai_set_error(error, CAI_ERR_LIMIT, "environment references exhausted");
  else
    ++environment->references;
  pthread_mutex_unlock(&environment->lock);
  return rc;
}
void cai_environment_close(cai_environment *environment) {
  size_t i;
  int release;
  if (environment == NULL)
    return;
  pthread_mutex_lock(&environment->lock);
  release = --environment->references == 0U;
  pthread_mutex_unlock(&environment->lock);
  if (!release)
    return;
  for (i = 0U; i < environment->count; ++i)
    cai_free_mem(NULL, environment->entries[i]);
  cai_free_mem(NULL, environment->entries);
  pthread_mutex_destroy(&environment->lock);
  cai_free_mem(NULL, environment);
}
