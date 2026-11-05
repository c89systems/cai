#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <cai/workspace_server.h>
#include <libmdf/mdf.h>
#include <pthread.h>
#include <softline/softline.h>
#include <stdlib.h>
#include <string.h>

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
} call;
static void *run(void *context) {
  call *c = context;
  cai_error error;
  cai_error_init(&error);
  c->rc = cai_app_run(c->app, c->argc - 1, (const char *const *)(c->argv + 1),
                      &c->code, &error);
  cai_error_cleanup(&error);
  return NULL;
}
int main(int argc, char **argv) {
  call c;
  cai_app_config config;
  cai_app_status status;
  cai_environment *before, *after;
  const char *const *a, *const *b;
  pthread_t worker;
  size_t i;
  memset(&c, 0, sizeof(c));
  c.argc = argc;
  c.argv = argv;
  cai_app_config_init(&config);
  cai_app_status_init(&status);
  assert(cai_environment_open(NULL, &before, NULL) == CAI_OK);
  assert(cai_app_open(&config, &c.app, NULL) == CAI_OK);
  /* No stack override: includes list/import/export/skill paths on musl. */
  assert(pthread_create(&worker, NULL, run, &c) == 0);
  assert(pthread_join(worker, NULL) == 0);
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
  return c.rc == CAI_OK ? c.code : 1;
}
