#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
unsigned int cai_test_storage_open_count(void);
typedef struct invocation {
  cai_app *app;
  int argc, rc, code, done[2];
  char **argv;
} invocation;
static int unexpected_tools(void *context, cai_agent *agent, cai_error *error) {
  (void)context;
  (void)agent;
  (void)error;
  assert(0 && "an attachment must not construct a local runtime");
  return CAI_ERR_INVALID;
}
static void *run(void *context) {
  invocation *r = context;
  cai_error error;
  cai_error_init(&error);
  r->rc = cai_app_run(r->app, r->argc - 1, (const char *const *)(r->argv + 1),
                      &r->code, &error);
  if (r->rc != CAI_OK)
    fprintf(stderr, "%s\n",
            error.message != NULL ? error.message : "run failed");
  cai_error_cleanup(&error);
  assert(write(r->done[1], "x", 1U) == 1);
  return NULL;
}
int main(int argc, char **argv) {
  invocation r;
  cai_app_config config;
  cai_app_status status;
  struct pollfd fds[3];
  pthread_t thread;
  char *before = getcwd(NULL, 0U), *after;
  char *locale = strdup(setlocale(LC_CTYPE, NULL));
  int flags = fcntl(STDIN_FILENO, F_GETFL), stopped = 0;
  memset(&r, 0, sizeof(r));
  r.argc = argc;
  r.argv = argv;
  assert(before != NULL && locale != NULL && pipe(r.done) == 0);
  cai_app_config_init(&config);
  config.register_agent_tools = unexpected_tools;
  assert(cai_app_open(&config, &r.app, NULL) == CAI_OK);
  cai_app_status_init(&status);
  assert(pthread_create(&thread, NULL, run, &r) == 0);
  memset(fds, 0, sizeof(fds));
  fds[0].fd = cai_app_status_fd(r.app);
  fds[1].fd = r.done[0];
  fds[0].events = fds[1].events = POLLIN;
  fds[2].fd = getenv("CAI_TEST_ATTACH_STOP_FILE") != NULL
                  ? open(getenv("CAI_TEST_ATTACH_STOP_FILE"),
                         O_RDWR | O_NONBLOCK | O_CLOEXEC)
                  : -1;
  assert(getenv("CAI_TEST_ATTACH_STOP_FILE") == NULL || fds[2].fd >= 0);
  fds[2].events = POLLIN;
  for (;;) {
    assert(poll(fds, 3U, -1) >= 0 || errno == EINTR);
    if (fds[0].revents & POLLIN) {
      assert(cai_app_observe_status(r.app, &status, NULL) == CAI_OK);
    }
    if (status.running && !stopped &&
        (getenv("CAI_TEST_ATTACH_STOP") != NULL || (fds[2].revents & POLLIN))) {
      unsigned long long request;
      if (fds[2].revents & POLLIN) {
        char token;
        assert(read(fds[2].fd, &token, 1U) == 1);
      }
      assert(cai_app_control(r.app, CAI_APP_STOP, NULL, &request, NULL) ==
             CAI_OK);
      assert(request != 0U);
      stopped = 1;
    }
    if (fds[1].revents & POLLIN)
      break;
  }
  assert(pthread_join(thread, NULL) == 0);
  assert(cai_test_storage_open_count() == 0U);
  after = getcwd(NULL, 0U);
  assert(after != NULL && strcmp(before, after) == 0);
  assert(strcmp(locale, setlocale(LC_CTYPE, NULL)) == 0);
  assert(fcntl(STDIN_FILENO, F_GETFL) == flags);
  assert(cai_app_get_status(r.app, &status, NULL) == CAI_OK);
  assert(!status.running && !status.runtime_ready);
  if (stopped)
    assert(r.rc == CAI_OK && r.code == 0 && status.outcome == CAI_APP_STOPPED);
  cai_app_status_cleanup(&status);
  cai_app_close(r.app);
  close(r.done[0]);
  close(r.done[1]);
  if (fds[2].fd >= 0)
    close(fds[2].fd);
  free(before);
  free(after);
  free(locale);
  return r.rc == CAI_OK ? r.code : 1;
}
