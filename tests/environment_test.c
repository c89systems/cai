#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/environment.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void values(void) {
  static const char *const entries[] = {
      "HOME=/real home",     "A=first",
      "AA=prefix",           "A=ignored duplicate",
      "A_B=underscore",      "EMPTY=",
      "odd-name=a=b\nvalue", NULL};
  static const cai_environment_change changes[] = {
      {"A", "discarded"}, {"A", NULL},  {"B", "added"},    {"HOME", NULL},
      {"A", "last"},      {"HOME", ""}, {"MISSING", NULL}, {"AA", NULL}};
  static const char *const empty[] = {NULL};
  cai_environment *base = NULL, *derived = NULL, *shared = NULL;
  const char *const *vector;
  assert(cai_environment_open(entries, &base, NULL) == CAI_OK);
  assert(cai_environment_count(base) == 6U);
  assert(strcmp(cai_environment_get(base, "A"), "first") == 0);
  assert(strcmp(cai_environment_get(base, "AA"), "prefix") == 0);
  assert(strcmp(cai_environment_get(base, "A_B"), "underscore") == 0);
  assert(strcmp(cai_environment_get(base, "odd-name"), "a=b\nvalue") == 0);
  assert(cai_environment_get(base, "EMPTY")[0] == '\0');
  assert(cai_environment_get(base, "MISSING") == NULL);
  vector = cai_environment_entries(base);
  assert(vector != NULL && vector[6] == NULL);
  assert(strcmp(vector[0], "A=first") == 0);
  assert(cai_environment_derive(base, changes,
                                sizeof(changes) / sizeof(changes[0]), &derived,
                                NULL) == CAI_OK);
  assert(strcmp(cai_environment_get(derived, "A"), "last") == 0);
  assert(strcmp(cai_environment_get(derived, "B"), "added") == 0);
  assert(cai_environment_get(derived, "AA") == NULL);
  assert(cai_environment_get(derived, "HOME")[0] == '\0');
  assert(strcmp(cai_environment_get(base, "HOME"), "/real home") == 0);
  assert(cai_environment_derive(derived, NULL, 0U, &shared, NULL) == CAI_OK);
  assert(shared == derived);
  cai_environment_close(derived);
  assert(strcmp(cai_environment_get(shared, "A"), "last") == 0);
  cai_environment_close(shared);
  cai_environment_close(base);
  assert(cai_environment_open(empty, &base, NULL) == CAI_OK);
  assert(cai_environment_count(base) == 0U &&
         cai_environment_entries(base)[0] == NULL &&
         cai_environment_get(base, "HOME") == NULL);
  cai_environment_close(base);
}
static void capture(void) {
  const char *name = "CAI_TEST_ENVIRONMENT_CAPTURE";
  const char *previous = getenv(name);
  char *saved = previous != NULL ? strdup(previous) : NULL;
  cai_environment *captured = NULL;
  assert(previous == NULL || saved != NULL);
  assert(setenv(name, "first", 1) == 0);
  assert(cai_environment_open(NULL, &captured, NULL) == CAI_OK);
  assert(setenv(name, "later", 1) == 0);
  assert(strcmp(cai_environment_get(captured, name), "first") == 0);
  assert(strcmp(getenv(name), "later") == 0);
  cai_environment_close(captured);
  assert((saved != NULL ? setenv(name, saved, 1) : unsetenv(name)) == 0);
  free(saved);
}
static void owned_large_view(void) {
  char **entries = calloc(2049U, sizeof(*entries));
  char name[32], expected[32];
  cai_environment *environment = NULL;
  size_t i;
  assert(entries != NULL);
  for (i = 0U; i < 2048U; ++i) {
    entries[i] = malloc(64U);
    assert(entries[i] != NULL);
    snprintf(entries[i], 64U, "VALUE_%04lu=value-%lu",
             (unsigned long)(2047U - i), (unsigned long)(2047U - i));
  }
  assert(cai_environment_open((const char *const *)(void *)entries,
                              &environment, NULL) == CAI_OK);
  for (i = 0U; i < 2048U; ++i)
    free(entries[i]);
  free(entries);
  for (i = 0U; i < 2048U; ++i) {
    snprintf(name, sizeof(name), "VALUE_%04lu", (unsigned long)i);
    snprintf(expected, sizeof(expected), "value-%lu", (unsigned long)i);
    assert(strcmp(cai_environment_get(environment, name), expected) == 0);
  }
  assert(cai_environment_count(environment) == 2048U);
  cai_environment_close(environment);
}
static void *reference_worker(void *context) {
  cai_environment *environment = context;
  size_t i;
  for (i = 0U; i < 1000U; ++i) {
    assert(cai_environment_retain(environment, NULL) == CAI_OK);
    assert(strcmp(cai_environment_get(environment, "HOME"), "/host") == 0);
    cai_environment_close(environment);
  }
  return NULL;
}
static void references(void) {
  static const char *const entries[] = {"HOME=/host", NULL};
  cai_environment *environment = NULL;
  pthread_t threads[4];
  size_t i;
  assert(cai_environment_open(entries, &environment, NULL) == CAI_OK);
  for (i = 0U; i < 4U; ++i)
    assert(pthread_create(&threads[i], NULL, reference_worker, environment) ==
           0);
  for (i = 0U; i < 4U; ++i)
    assert(pthread_join(threads[i], NULL) == 0);
  cai_environment_close(environment);
}
static void invalid(void) {
  static const char *const missing_equal[] = {"bad", NULL};
  static const char *const empty_name[] = {"=bad", NULL};
  static const char *const empty[] = {NULL};
  static const cai_environment_change changes[] = {
      {"", "bad"}, {"A=B", "bad"}, {NULL, "bad"}};
  cai_environment *base = NULL, *out = NULL;
  size_t i;
  assert(cai_environment_open(NULL, NULL, NULL) == CAI_ERR_INVALID);
  assert(cai_environment_open(missing_equal, &out, NULL) == CAI_ERR_INVALID);
  assert(cai_environment_open(empty_name, &out, NULL) == CAI_ERR_INVALID);
  assert(out == NULL);
  assert(cai_environment_open(empty, &base, NULL) == CAI_OK);
  for (i = 0U; i < sizeof(changes) / sizeof(changes[0]); ++i)
    assert(cai_environment_derive(base, changes + i, 1U, &out, NULL) ==
               CAI_ERR_INVALID &&
           out == NULL);
  assert(cai_environment_derive(base, NULL, 1U, &out, NULL) == CAI_ERR_INVALID);
  assert(cai_environment_derive(NULL, NULL, 0U, &out, NULL) == CAI_ERR_INVALID);
  assert(cai_environment_get(base, "A=B") == NULL);
  assert(cai_environment_get(NULL, "HOME") == NULL);
  assert(cai_environment_count(NULL) == 0U);
  assert(cai_environment_entries(NULL) == NULL);
  assert(cai_environment_retain(NULL, NULL) == CAI_ERR_INVALID);
  cai_environment_close(base);
  cai_environment_close(NULL);
}
int main(void) {
  values();
  capture();
  owned_large_view();
  references();
  invalid();
  return 0;
}
