#define _POSIX_C_SOURCE 200809L
#include "../cli/pouch.h"
#include "../cli/session_commands.h"
#include <assert.h>
#include <cai/app.h>
#include <cai/workspace_server.h>
#include <libmdf/mdf.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Saving/restoring process state still exposes temporary changes to another
 * controller and can overwrite that controller's changes. Guard the call. */
static pid_t host_pid;
static pthread_key_t active;
static int parallel;
static unsigned int arrivals;
static pthread_mutex_t arrival_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrival_changed = PTHREAD_COND_INITIALIZER;
int __real_setenv(const char *, const char *, int);
int __wrap_setenv(const char *, const char *, int);
int __wrap_setenv(const char *name, const char *value, int replace) {
  assert(getpid() != host_pid || pthread_getspecific(active) == NULL);
  return __real_setenv(name, value, replace);
}
int __real_unsetenv(const char *);
int __wrap_unsetenv(const char *);
int __wrap_unsetenv(const char *name) {
  assert(getpid() != host_pid || pthread_getspecific(active) == NULL);
  return __real_unsetenv(name);
}
int __real_clearenv(void);
int __wrap_clearenv(void);
int __wrap_clearenv(void) {
  assert(getpid() != host_pid || pthread_getspecific(active) == NULL);
  return __real_clearenv();
}
int __real_chdir(const char *);
int __wrap_chdir(const char *);
int __wrap_chdir(const char *directory) {
  assert(getpid() != host_pid || pthread_getspecific(active) == NULL);
  return __real_chdir(directory);
}
int __real_fchdir(int);
int __wrap_fchdir(int);
int __wrap_fchdir(int descriptor) {
  assert(getpid() != host_pid || pthread_getspecific(active) == NULL);
  return __real_fchdir(descriptor);
}
char *__real_setlocale(int, const char *);
char *__wrap_setlocale(int, const char *);
char *__wrap_setlocale(int category, const char *value) {
  assert(value == NULL || getpid() != host_pid ||
         pthread_getspecific(active) == NULL);
  return __real_setlocale(category, value);
}
int __real_sigaction(int, const struct sigaction *, struct sigaction *);
int __wrap_sigaction(int, const struct sigaction *, struct sigaction *);
int __wrap_sigaction(int signal, const struct sigaction *action,
                     struct sigaction *previous) {
  assert(action == NULL || getpid() != host_pid ||
         pthread_getspecific(active) == NULL);
  return __real_sigaction(signal, action, previous);
}
int __real_cai_cli_pouch_open_options(cai_cli_pouch *, const cai_cli_options *,
                                      pslog_logger *, cai_error *);
int __wrap_cai_cli_pouch_open_options(cai_cli_pouch *, const cai_cli_options *,
                                      pslog_logger *, cai_error *);
int __wrap_cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                                      const cai_cli_options *options,
                                      pslog_logger *logger, cai_error *error) {
  int rc = __real_cai_cli_pouch_open_options(pouch, options, logger, error);
  if (parallel) {
    struct timespec deadline;
    const char *slot = pthread_getspecific(active);
    assert(rc == CAI_OK && slot != NULL);
    assert(strcmp(getenv("UTILITY_SLOT"), "host") == 0);
    assert(strcmp(cai_environment_get(options->environment, "UTILITY_SLOT"),
                  slot) == 0);
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&arrival_lock);
    ++arrivals;
    pthread_cond_broadcast(&arrival_changed);
    while (arrivals < 2U)
      assert(pthread_cond_timedwait(&arrival_changed, &arrival_lock,
                                    &deadline) == 0);
    pthread_mutex_unlock(&arrival_lock);
  }
  return rc;
}

/* Utilities cannot run an agent or construct a frontend. Offline export's
 * existing read-only restoration helper is the sole runtime-open exception;
 * neither it nor another utility may enter a runtime event loop. */
int __real_cai_agent_runtime_open(cai_client *,
                                  const cai_agent_runtime_config *,
                                  cai_agent_runtime **, cai_error *);
int __wrap_cai_agent_runtime_open(cai_client *,
                                  const cai_agent_runtime_config *,
                                  cai_agent_runtime **, cai_error *);
int __wrap_cai_workspace_server_open(const cai_workspace_server_config *,
                                     cai_workspace_server **, cai_error *);
sl_t *__wrap_sl_create_with_config(const sl_config_t *);
mdf_status __wrap_mdf_create(mdf_format, const mdf_options *, mdf **);
int __wrap_cai_agent_runtime_pump(cai_agent_runtime *, long, cai_error *);
int __wrap_cai_agent_runtime_pump_bounded(cai_agent_runtime *, long, size_t,
                                          cai_error *);
int __wrap_cai_agent_runtime_open(cai_client *client,
                                  const cai_agent_runtime_config *config,
                                  cai_agent_runtime **out, cai_error *error) {
  assert(config->session_store != NULL && config->resume_session_id != NULL &&
         config->disable_terminal && config->disable_review_subagent &&
         config->session_store->commit != NULL &&
         config->session_store->append_event != NULL);
  return __real_cai_agent_runtime_open(client, config, out, error);
}
int __wrap_cai_agent_runtime_pump(cai_agent_runtime *runtime, long timeout,
                                  cai_error *error) {
  (void)runtime;
  (void)timeout;
  (void)error;
  abort();
}
int __wrap_cai_agent_runtime_pump_bounded(cai_agent_runtime *runtime,
                                          long timeout, size_t count,
                                          cai_error *error) {
  (void)runtime;
  (void)timeout;
  (void)count;
  (void)error;
  abort();
}
int __wrap_cai_workspace_server_open(const cai_workspace_server_config *config,
                                     cai_workspace_server **out,
                                     cai_error *error) {
  (void)config;
  (void)out;
  (void)error;
  abort();
}
sl_t *__wrap_sl_create_with_config(const sl_config_t *config) {
  (void)config;
  abort();
}
mdf_status __wrap_mdf_create(mdf_format format, const mdf_options *options,
                             mdf **out) {
  (void)format;
  (void)options;
  (void)out;
  abort();
}
typedef struct call {
  cai_app *app;
  int argc, rc, code;
  char **argv;
  const char *slot;
} call;
static void *run(void *context) {
  call *c = context;
  cai_error error;
  cai_agent_history_entry entry;
  char row[128];
  locale_t locale = uselocale((locale_t)0);
  cai_error_init(&error);
  assert(pthread_setspecific(active, c->slot != NULL ? c->slot : "single") ==
         0);
  memset(&entry, 0, sizeof(entry));
  entry.remove_count = 1U;
  entry.kind = CAI_AGENT_HISTORY_TURN;
  entry.summary = "räv 東京";
  cai_cli_history_row(row, sizeof(row), &entry, 60);
  assert(strcmp(row, "1. räv 東京\n") == 0);
  assert(uselocale((locale_t)0) == locale);
  c->rc = cai_app_run(c->app, c->argc - 1, (const char *const *)(c->argv + 1),
                      &c->code, &error);
  assert(uselocale((locale_t)0) == locale);
  assert(pthread_setspecific(active, NULL) == 0);
  cai_error_cleanup(&error);
  return NULL;
}
int main(int argc, char **argv) {
  call c;
  call other;
  cai_app_config config;
  cai_app_setting settings[2];
  cai_app_status status;
  cai_environment *before, *after;
  const char *const *a, *const *b;
  pthread_t worker, second;
  size_t i;
  memset(&c, 0, sizeof(c));
  host_pid = getpid();
  assert(pthread_key_create(&active, NULL) == 0);
  parallel = getenv("CAI_TEST_UTILITY_CONCURRENT") != NULL;
  if (parallel)
    assert(setenv("UTILITY_SLOT", "host", 1) == 0);
  c.argc = argc;
  c.argv = argv;
  cai_app_config_init(&config);
  cai_app_status_init(&status);
  assert(cai_environment_open(NULL, &before, NULL) == CAI_OK);
  if (parallel) {
    settings[0].section = "";
    settings[0].key = "config_env_has_precedence";
    settings[0].value = "true";
    settings[1].section = "env";
    settings[1].key = "UTILITY_SLOT";
    settings[1].value = c.slot = "alpha";
    config.settings = settings;
    config.setting_count = 2U;
  }
  assert(cai_app_open(&config, &c.app, NULL) == CAI_OK);
  memset(&other, 0, sizeof(other));
  if (parallel) {
    other.argc = argc;
    other.argv = argv;
    settings[1].value = other.slot = "beta";
    assert(cai_app_open(&config, &other.app, NULL) == CAI_OK);
  }
  /* No stack override: includes list/import/export/skill paths on musl. */
  assert(pthread_create(&worker, NULL, run, &c) == 0);
  if (parallel)
    assert(pthread_create(&second, NULL, run, &other) == 0);
  assert(pthread_join(worker, NULL) == 0);
  if (parallel) {
    assert(pthread_join(second, NULL) == 0);
    assert(other.rc == CAI_OK && other.code == 0 && arrivals == 2U);
    cai_app_close(other.app);
  }
  assert(cai_app_get_status(c.app, &status, NULL) == CAI_OK);
  assert(!status.running && !status.runtime_ready && !status.has_server &&
         status.session_id == NULL);
  assert(cai_environment_open(NULL, &after, NULL) == CAI_OK);
  assert(cai_environment_count(before) == cai_environment_count(after));
  a = cai_environment_entries(before);
  b = cai_environment_entries(after);
  for (i = 0U; a[i] != NULL; ++i)
    assert(strcmp(a[i], b[i]) == 0);
  cai_environment_close(before);
  cai_environment_close(after);
  cai_app_status_cleanup(&status);
  cai_app_close(c.app);
  assert(pthread_key_delete(active) == 0);
  return c.rc == CAI_OK ? c.code : 1;
}
