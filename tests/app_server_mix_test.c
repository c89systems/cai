/** Two public API surfaces must share one live Pouch client registry. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <cai/app_server.h>
#include <poll.h>
#include <pslog.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct fixture {
  pthread_mutex_t lock;
  int completed;
} fixture;
static int event(void *context, const cai_agent_runtime_event *value,
                 cai_error *error) {
  fixture *f = context;
  (void)error;
  if (value->type == CAI_AGENT_EVENT_RUN_COMPLETED) {
    pthread_mutex_lock(&f->lock);
    ++f->completed;
    pthread_mutex_unlock(&f->lock);
  }
  return CAI_OK;
}
static void ready(cai_app_server *server, cai_app *app, int finished) {
  cai_app_status status;
  struct pollfd fd;
  int i;
  cai_app_status_init(&status);
  fd.fd = server != NULL ? cai_app_server_status_fd(server)
                         : cai_app_status_fd(app);
  fd.events = POLLIN;
  for (i = 0; i < 150; ++i) {
    int rc = server != NULL
                 ? cai_app_server_observe_status(server, &status, NULL)
                 : cai_app_observe_status(app, &status, NULL);
    assert(rc == CAI_OK);
    if ((!finished && status.runtime_ready) || (finished && !status.running))
      break;
    assert(status.running);
    fd.revents = 0;
    assert(poll(&fd, 1U, 100) >= 0);
  }
  assert(i < 150);
  cai_app_status_cleanup(&status);
}
int main(int argc, char **argv) {
  cai_app_config config;
  cai_attachment_config lifetime;
  cai_app_server *server;
  cai_app *app;
  cai_app_status status;
  fixture f;
  pslog_config logging;
  pslog_logger *logger;
  FILE *output;
  const char **other;
  const char *child = getenv("CAI_TEST_SERVER_CHILD");
  const char *path = getenv("CAI_TEST_SERVER_LOG");
  char *directory = getcwd(NULL, 0U), *after;
  unsigned long long id;
  int code, i;
  assert(argc > 1 && child != NULL && path != NULL && directory != NULL);
  output = fopen(path, "w");
  assert(output != NULL);
  pslog_default_config(&logging);
  logging.mode = PSLOG_MODE_JSON;
  logging.min_level = PSLOG_LEVEL_TRACE;
  logging.output = pslog_output_from_fp(output, 0);
  logger = pslog_new(&logging);
  assert(logger != NULL);
  memset(&f, 0, sizeof(f));
  assert(pthread_mutex_init(&f.lock, NULL) == 0);
  cai_app_config_init(&config);
  config.logger = logger;
  config.event_callback = event;
  config.event_context = &f;
  config.runtime_directory = getenv("CAI_TEST_SERVER_RUNTIME");
  cai_attachment_config_init(&lifetime);
  lifetime.persist = 1;
  assert(cai_app_server_open(&config, &server, NULL) == CAI_OK);
  assert(cai_app_server_start(server, NULL, argc - 1,
                              (const char *const *)(argv + 1), &lifetime,
                              NULL) == CAI_OK);
  ready(server, NULL, 0);
  other = calloc((size_t)argc + 2U, sizeof(*other));
  assert(other != NULL);
  for (i = 1; i < argc; ++i)
    other[i - 1] = argv[i];
  other[argc - 1] = "-C";
  other[argc] = child;
  assert(cai_app_open(&config, &app, NULL) == CAI_OK);
  assert(cai_app_start(app, NULL, argc + 1, other, NULL) == CAI_OK);
  free(other);
  ready(NULL, app, 0);
  /* The surviving owner must retain the shared SDK client's lifetime. */
  assert(cai_app_control(app, CAI_APP_STOP, NULL, &id, NULL) == CAI_OK &&
         id != 0U);
  assert(cai_app_join(app, &code, NULL) == CAI_OK && code == 0);
  cai_app_close(app);
  cai_app_status_init(&status);
  assert(cai_app_server_get_status(server, &status, NULL) == CAI_OK &&
         status.runtime_ready);
  cai_app_status_cleanup(&status);
  assert(cai_app_server_control(server, CAI_APP_QUEUE_PROMPT,
                                "SURVIVING SERVER", &id, NULL) == CAI_OK &&
         id != 0U);
  for (i = 0; i < 150; ++i) {
    struct pollfd fd;
    int done;
    pthread_mutex_lock(&f.lock);
    done = f.completed;
    pthread_mutex_unlock(&f.lock);
    if (done)
      break;
    fd.fd = cai_app_server_status_fd(server);
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 100) >= 0);
    cai_app_status_init(&status);
    assert(cai_app_server_observe_status(server, &status, NULL) == CAI_OK);
    cai_app_status_cleanup(&status);
  }
  assert(i < 150);
  assert(cai_app_server_control(server, CAI_APP_STOP, NULL, &id, NULL) ==
         CAI_OK);
  ready(server, NULL, 1);
  assert(cai_app_server_join(server, &code, NULL) == CAI_OK && code == 0);
  cai_app_server_close(server);
  logger->destroy(logger);
  assert(fclose(output) == 0);
  assert(pthread_mutex_destroy(&f.lock) == 0);
  after = getcwd(NULL, 0U);
  assert(after != NULL && strcmp(after, directory) == 0);
  free(after);
  free(directory);
  return 0;
}
