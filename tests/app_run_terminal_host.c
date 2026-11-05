#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** The synchronous owner runs on a default-stack host worker. The main host
 * independently consumes public status notifications while Softline attaches,
 * receives output and closes on another default-stack worker. */
typedef struct invocation {
  cai_app *app;
  pthread_t owner;
  cai_environment *environment;
  char *directory, *locale;
  int argc, rc, code, completed[2];
  char **argv;
  int registrations, calls, events;
} invocation;
unsigned int cai_test_storage_open_count(void);
static void unchanged(const invocation *r) {
  char *directory = getcwd(NULL, 0U);
  const char *value = getenv("CAI_RUN_ENV");
  assert(directory != NULL && strcmp(directory, r->directory) == 0);
  assert(value != NULL && strcmp(value, "host") == 0);
  assert(strcmp(setlocale(LC_CTYPE, NULL), r->locale) == 0);
  free(directory);
}
static int tool(void *context, const char *arguments, cai_sink *output,
                cai_error *error) {
  invocation *r = context;
  static const char result[] = "{\"value\":\"HOST_TOOL_VALUE\"}";
  assert(!pthread_equal(r->owner, pthread_self()) && arguments != NULL);
  ++r->calls;
  return cai_sink_write(output, result, sizeof(result) - 1U, error);
}
static int register_tools(void *context, cai_agent *agent, cai_error *error) {
  invocation *r = context;
  assert(pthread_equal(r->owner, pthread_self()));
  unchanged(r);
  ++r->registrations;
  if (getenv("CAI_RUN_FAIL_START") != NULL) {
    error->code = CAI_ERR_PROTOCOL;
    error->message = strdup("HOST_START_FAILED");
    return CAI_ERR_PROTOCOL;
  }
  return cai_agent_register_raw_tool(agent, "host_fact", "Host fact",
                                     "{\"type\":\"object\"}", 0, tool, r,
                                     error);
}
static int event(void *context, const cai_agent_runtime_event *e,
                 cai_error *error) {
  invocation *r = context;
  (void)error;
  assert(e != NULL && pthread_equal(r->owner, pthread_self()));
  unchanged(r);
  ++r->events;
  return CAI_OK;
}
static int replay(void *context, const cai_agent_session_event *e,
                  cai_error *error) {
  invocation *r = context;
  (void)error;
  assert(e != NULL && pthread_equal(r->owner, pthread_self()));
  unchanged(r);
  return CAI_OK;
}
static void *run(void *context) {
  invocation *r = context;
  cai_error error;
  char byte = 'd';
  r->owner = pthread_self();
  cai_error_init(&error);
  r->rc = cai_app_run(r->app, r->argc - 1, (const char *const *)(r->argv + 1),
                      &r->code, &error);
  cai_error_cleanup(&error);
  assert(write(r->completed[1], &byte, 1U) == 1);
  return NULL;
}
int main(int argc, char **argv) {
  invocation r;
  cai_app_config config;
  cai_app_status status;
  pthread_t worker;
  struct pollfd fds[2];
  cai_environment *after;
  const char *const *before_values, *const *after_values;
  size_t i;
  int ready = 0, observations = 0;
  FILE *report;
  struct sigaction original_int, original_winch, after_action;
  int input_flags = fcntl(STDIN_FILENO, F_GETFL);
  memset(&r, 0, sizeof(r));
  r.argc = argc;
  r.argv = argv;
  r.directory = getcwd(NULL, 0U);
  r.locale = strdup(setlocale(LC_CTYPE, NULL));
  assert(r.locale != NULL);
  assert(sigaction(SIGINT, NULL, &original_int) == 0 &&
         sigaction(SIGWINCH, NULL, &original_winch) == 0);
  assert(r.directory != NULL && pipe(r.completed) == 0);
  assert(cai_environment_open(NULL, &r.environment, NULL) == CAI_OK);
  cai_app_config_init(&config);
  config.register_agent_tools = register_tools;
  config.register_agent_tools_context = &r;
  config.event_callback = event;
  config.event_context = &r;
  config.replay_callback = replay;
  config.replay_context = &r;
  assert(cai_app_open(&config, &r.app, NULL) == CAI_OK);
  cai_app_status_init(&status);
  assert(pthread_create(&worker, NULL, run, &r) == 0);
  memset(fds, 0, sizeof(fds));
  fds[0].fd = cai_app_status_fd(r.app);
  fds[1].fd = r.completed[0];
  fds[0].events = fds[1].events = POLLIN;
  for (;;) {
    assert(poll(fds, 2U, -1) >= 0 || errno == EINTR);
    if (fds[0].revents & POLLIN) {
      assert(cai_app_observe_status(r.app, &status, NULL) == CAI_OK);
      ++observations;
      if (status.running && status.runtime_ready) {
        assert(status.has_server && status.session_id != NULL);
        ready = 1;
      }
    }
    if (fds[1].revents & POLLIN)
      break;
  }
  assert(pthread_join(worker, NULL) == 0);
  unchanged(&r);
  assert(fcntl(STDIN_FILENO, F_GETFL) == input_flags);
  assert(sigaction(SIGINT, NULL, &after_action) == 0 &&
         after_action.sa_handler == original_int.sa_handler);
  assert(sigaction(SIGWINCH, NULL, &after_action) == 0 &&
         after_action.sa_handler == original_winch.sa_handler);
  assert(cai_environment_open(NULL, &after, NULL) == CAI_OK);
  assert(cai_environment_count(after) == cai_environment_count(r.environment));
  before_values = cai_environment_entries(r.environment);
  after_values = cai_environment_entries(after);
  for (i = 0U; before_values[i] != NULL; ++i)
    assert(strcmp(before_values[i], after_values[i]) == 0);
  cai_environment_close(after);
  assert(cai_app_get_status(r.app, &status, NULL) == CAI_OK);
  assert(!status.running && !status.runtime_ready && status.has_server);
  assert(r.registrations == 1 && cai_test_storage_open_count() == 1U);
  if (getenv("CAI_RUN_FAIL_START") == NULL)
    assert(r.rc == CAI_OK && r.code == 0 && ready && observations > 1 &&
           status.outcome == CAI_APP_COMPLETED && r.events > 0);
  else
    assert(r.rc != CAI_OK || r.code != 0);
  report = fopen(getenv("CAI_RUN_REPORT"), "w");
  assert(report != NULL);
  assert(fprintf(report, "{\"calls\":%d,\"ready\":%d,\"observations\":%d}\n",
                 r.calls, ready, observations) > 0);
  assert(fclose(report) == 0);
  cai_app_status_cleanup(&status);
  cai_app_close(r.app);
  cai_environment_close(r.environment);
  free(r.directory);
  free(r.locale);
  close(r.completed[0]);
  close(r.completed[1]);
  return r.rc == CAI_OK ? r.code : 1;
}
