#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <cai/app.h>
#include <libmdf/mdf.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* A controller must not create presentation objects, even with a terminal. */
mdf_status __wrap_mdf_create(mdf_format, const mdf_options *, mdf **);
mdf_status __wrap_mdf_create(mdf_format format, const mdf_options *options,
                             mdf **out) {
  (void)format;
  (void)options;
  (void)out;
  abort();
}
sl_t *__wrap_sl_create_with_config(const sl_config_t *);
sl_t *__wrap_sl_create_with_config(const sl_config_t *config) {
  (void)config;
  abort();
}
typedef struct host {
  cai_app *app;
  pthread_t caller, owner;
  const char *mode;
  pthread_mutex_t lock;
  int starts, completed, text, replay, acks, rejected, steered, has_owner;
} host;
unsigned int cai_test_storage_open_count(void);
static void control(host *h, cai_app_control_type type, const char *text) {
  unsigned long long id;
  assert(cai_app_control(h->app, type, text, &id, NULL) == CAI_OK && id != 0U);
}
static void owner(host *h) {
  assert(!pthread_equal(h->caller, pthread_self()));
  if (!h->has_owner) {
    h->owner = pthread_self();
    h->has_owner = 1;
  }
  assert(pthread_equal(h->owner, pthread_self()));
  assert(getenv("CAI_CONTROLLER_PRIVATE") == NULL);
}
static int event(void *context, const cai_agent_runtime_event *e,
                 cai_error *error) {
  host *h = context;
  owner(h);
  if (e->type == CAI_AGENT_EVENT_RUN_STARTED) {
    int code;
    pthread_mutex_lock(&h->lock);
    ++h->starts;
    pthread_mutex_unlock(&h->lock);
    assert(cai_app_join(h->app, &code, NULL) == CAI_ERR_INVALID);
    assert(cai_app_start(h->app, NULL, 0, NULL, NULL) == CAI_ERR_INVALID);
    if (strcmp(h->mode, "queue") == 0 && h->starts == 1) {
      control(h, CAI_APP_QUEUE_PROMPT, "CONTROLLER QUEUED ONE");
      control(h, CAI_APP_QUEUE_PROMPT, "CONTROLLER QUEUED TWO");
    }
    if (strcmp(h->mode, "cancel") == 0 && h->starts == 1)
      control(h, CAI_APP_CANCEL_TURN, NULL);
    if (strcmp(h->mode, "stop_active") == 0)
      control(h, CAI_APP_STOP, NULL);
  }
  if (e->type == CAI_AGENT_EVENT_TEXT_DELTA) {
    pthread_mutex_lock(&h->lock);
    ++h->text;
    pthread_mutex_unlock(&h->lock);
    if (strcmp(h->mode, "observer_error") == 0) {
      error->code = CAI_ERR_PROTOCOL;
      error->message = strdup("CONTROLLER OBSERVER FAILED");
      assert(error->message != NULL);
      return CAI_ERR_PROTOCOL;
    }
  }
  if (e->type == CAI_AGENT_EVENT_TOOL_CALL_COMPLETED &&
      strcmp(h->mode, "steer") == 0 && !h->steered) {
    h->steered = 1;
    control(h, CAI_APP_STEER, "CONTROLLER STEERING");
  }
  if (e->type == CAI_AGENT_EVENT_RUN_COMPLETED) {
    pthread_mutex_lock(&h->lock);
    ++h->completed;
    pthread_mutex_unlock(&h->lock);
  }
  if (e->type == CAI_AGENT_EVENT_RUN_CANCELLED &&
      strcmp(h->mode, "cancel") == 0)
    control(h, CAI_APP_QUEUE_PROMPT, "AFTER CONTROLLER CANCEL");
  if (e->type == CAI_AGENT_EVENT_RUN_FAILED &&
      strcmp(h->mode, "provider_error") == 0 && !h->steered) {
    h->steered = 1;
    control(h, CAI_APP_QUEUE_PROMPT, "AFTER PROVIDER FAILURE");
  }
  return CAI_OK;
}
static int replay(void *context, const cai_agent_session_event *e,
                  cai_error *error) {
  host *h = context;
  (void)e;
  (void)error;
  owner(h);
  if (++h->replay == 1 && strcmp(h->mode, "replay_stop") == 0)
    control(h, CAI_APP_STOP, NULL);
  return CAI_OK;
}
static void ack(void *context, unsigned long long id, int status,
                const cai_error *error) {
  host *h = context;
  (void)error;
  owner(h);
  assert(id != 0U);
  pthread_mutex_lock(&h->lock);
  ++h->acks;
  if (status != CAI_OK)
    ++h->rejected;
  pthread_mutex_unlock(&h->lock);
}
static int register_tools(void *context, cai_agent *agent, cai_error *error) {
  host *h = context;
  (void)agent;
  (void)error;
  owner(h);
  if (strcmp(h->mode, "startup_stop") == 0)
    control(h, CAI_APP_STOP, NULL);
  return CAI_OK;
}
static void pause_tick(void) {
  struct timespec delay = {0, 1000000L};
  nanosleep(&delay, NULL);
}
static void wait_ready(host *h) {
  cai_app_status s;
  struct pollfd fd;
  size_t ticks;
  cai_app_status_init(&s);
  for (ticks = 0U; ticks < 10000U; ++ticks) {
    assert(cai_app_observe_status(h->app, &s, NULL) == CAI_OK);
    if (s.runtime_ready)
      break;
    if (!s.running)
      fprintf(stderr, "controller startup failed: code=%d %s (%s)\n",
              s.exit_code,
              s.error.message != NULL ? s.error.message : "no message",
              s.error.detail != NULL ? s.error.detail : "no detail");
    assert(s.running);
    fd.fd = cai_app_status_fd(h->app);
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 10000) == 1 && (fd.revents & POLLIN) != 0);
  }
  assert(s.running && s.runtime_ready && s.session_id != NULL);
  cai_app_status_cleanup(&s);
}
static void wait_count(host *h, int turns, int rejected) {
  size_t ticks;
  int done = 0;
  for (ticks = 0U; ticks < 10000U; ++ticks) {
    pthread_mutex_lock(&h->lock);
    done = h->completed >= turns && h->rejected >= rejected;
    pthread_mutex_unlock(&h->lock);
    if (done)
      break;
    pause_tick();
  }
  assert(done);
}
static void open_host(host *h, const char *mode) {
  cai_app_config config;
  static const cai_app_setting settings[] = {
      {"env", "CAI_CONTROLLER_PRIVATE", "never-installed"}};
  memset(h, 0, sizeof(*h));
  h->mode = mode;
  h->caller = pthread_self();
  assert(pthread_mutex_init(&h->lock, NULL) == 0);
  cai_app_config_init(&config);
  config.settings = settings;
  config.setting_count = 1U;
  config.event_callback = event;
  config.event_context = h;
  config.replay_callback = replay;
  config.replay_context = h;
  config.control_callback = ack;
  config.control_context = h;
  config.register_agent_tools = register_tools;
  config.register_agent_tools_context = h;
  assert(cai_app_open(&config, &h->app, NULL) == CAI_OK);
}
static void independent_controllers(int argc, char **argv) {
  host a, b;
  cai_config_context context;
  cai_environment *base = NULL, *first = NULL, *second = NULL;
  cai_environment_change change;
  cai_app_status sa, sb;
  const char **arguments = calloc((size_t)argc + 3U, sizeof(*arguments));
  const char *work = getenv("CAI_TEST_CONTROLLER_WORK");
  int i, count = 0, code;
  unsigned int opens = cai_test_storage_open_count();
  assert(arguments != NULL && work != NULL);
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0 ||
        strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--directory") == 0) {
      ++i;
      assert(i < argc);
    } else
      arguments[count++] = argv[i];
  }
  arguments[count++] = "--no-config";
  arguments[count++] = "-C";
  arguments[count++] = ".";
  assert(cai_environment_open(NULL, &base, NULL) == CAI_OK);
  change.name = "CAI_MODEL";
  change.value = "gpt-6-luna";
  assert(cai_environment_derive(base, &change, 1U, &first, NULL) == CAI_OK);
  change.value = "gpt-6.1-sol";
  assert(cai_environment_derive(base, &change, 1U, &second, NULL) == CAI_OK);
  cai_environment_close(base);
  open_host(&a, "contexts");
  open_host(&b, "contexts");
  context.directory = work;
  context.environment = first;
  assert(cai_app_start(a.app, &context, count, arguments, NULL) == CAI_OK);
  arguments[count - 1] = "child";
  context.environment = second;
  assert(cai_app_start(b.app, &context, count, arguments, NULL) == CAI_OK);
  free(arguments);
  cai_environment_close(first);
  cai_environment_close(second);
  wait_ready(&a);
  wait_ready(&b);
  cai_app_status_init(&sa);
  cai_app_status_init(&sb);
  assert(cai_app_get_status(a.app, &sa, NULL) == CAI_OK);
  assert(cai_app_get_status(b.app, &sb, NULL) == CAI_OK);
  assert(strcmp(sa.effective.model, "gpt-6-luna") == 0);
  assert(strcmp(sb.effective.model, "gpt-6.1-sol") == 0);
  assert(strcmp(sa.session_id, sb.session_id) != 0);
  assert(cai_test_storage_open_count() == opens + 1U);
  control(&a, CAI_APP_STOP, NULL);
  assert(cai_app_join(a.app, &code, NULL) == CAI_OK && code == 0);
  /* Stopping A must not close B's shared-root SDK client or its context. */
  assert(cai_app_get_status(b.app, &sb, NULL) == CAI_OK && sb.runtime_ready);
  control(&b, CAI_APP_STOP, NULL);
  assert(cai_app_join(b.app, &code, NULL) == CAI_OK && code == 0);
  cai_app_close(a.app);
  cai_app_close(b.app);
  cai_app_status_cleanup(&sa);
  cai_app_status_cleanup(&sb);
  pthread_mutex_destroy(&a.lock);
  pthread_mutex_destroy(&b.lock);
}
int main(int argc, char **argv) {
  host h;
  cai_app_status status;
  struct sigaction before, after;
  char cwd[4096], current[4096];
  char *locale = strdup(setlocale(LC_CTYPE, NULL));
  const char *mode = getenv("CAI_TEST_CONTROLLER_MODE");
  int code;
  assert(locale != NULL && mode != NULL &&
         getenv("CAI_CONTROLLER_PRIVATE") == NULL);
  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(sigaction(SIGINT, NULL, &before) == 0);
  if (strcmp(mode, "contexts") == 0) {
    independent_controllers(argc, argv);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, cwd) == 0);
    assert(strcmp(locale, setlocale(LC_CTYPE, NULL)) == 0);
    assert(sigaction(SIGINT, NULL, &after) == 0 &&
           before.sa_handler == after.sa_handler);
    free(locale);
    return 0;
  }
  cai_app_status_init(&status);
  open_host(&h, mode);
  assert(cai_app_join(h.app, &code, NULL) == CAI_ERR_INVALID);
  assert(cai_app_start(h.app, NULL, argc - 1, (const char *const *)(argv + 1),
                       NULL) == CAI_OK);
  assert(cai_app_start(h.app, NULL, 0, NULL, NULL) == CAI_ERR_INVALID);
  if (strcmp(mode, "startup_stop") != 0 && strcmp(mode, "replay_stop") != 0 &&
      strncmp(mode, "invalid", 7U) != 0) {
    wait_ready(&h);
    control(&h,
            strcmp(mode, "goal") == 0 ? CAI_APP_REPLACE_GOAL
                                      : CAI_APP_QUEUE_PROMPT,
            "CONTROLLER FIRST PROMPT");
    if (strcmp(mode, "observer_error") != 0 &&
        strcmp(mode, "stop_active") != 0) {
      wait_count(&h, strcmp(mode, "queue") == 0 ? 3 : 1, 0);
      assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK &&
             status.running);
      if (strcmp(mode, "run") == 0) {
        clockid_t clock;
        struct timespec before_cpu, after_cpu, idle = {0, 250000000L};
        assert(pthread_getcpuclockid(h.owner, &clock) == 0);
        assert(clock_gettime(clock, &before_cpu) == 0);
        nanosleep(&idle, NULL);
        assert(clock_gettime(clock, &after_cpu) == 0);
        assert((after_cpu.tv_sec - before_cpu.tv_sec) * 1000000000LL +
                   after_cpu.tv_nsec - before_cpu.tv_nsec <
               50000000LL);
        control(&h, CAI_APP_QUEUE_PROMPT, "CONTROLLER NEXT PROMPT");
        wait_count(&h, 2, 0);
        control(&h, CAI_APP_QUEUE_PROMPT, "/status");
        wait_count(&h, 2, 1);
      }
      control(&h, CAI_APP_STOP, NULL);
    }
  }
  assert(cai_app_join(h.app, &code, NULL) == CAI_OK);
  {
    struct pollfd fd;
    fd.fd = cai_app_status_fd(h.app);
    fd.events = POLLIN;
    fd.revents = 0;
    assert(poll(&fd, 1U, 0) == 1);
    assert(cai_app_observe_status(h.app, &status, NULL) == CAI_OK &&
           !status.running);
    assert(poll(&fd, 1U, 0) == 0);
  }
  assert(cai_app_get_status(h.app, &status, NULL) == CAI_OK &&
         !status.running && !status.runtime_ready);
  if (strncmp(mode, "invalid", 7U) == 0)
    assert(code == 2 && status.outcome == CAI_APP_FAILED &&
           status.error.code != CAI_OK && status.error.message != NULL);
  else if (strcmp(mode, "observer_error") == 0)
    assert(code == 1 && status.outcome == CAI_APP_FAILED &&
           status.error.code == CAI_ERR_PROTOCOL);
  else {
    assert(code == 0 && status.outcome == CAI_APP_STOPPED);
    if (strcmp(mode, "replay_stop") == 0)
      assert(h.replay == 1 && h.starts == 0 && status.error.code == CAI_OK);
    else if (strcmp(mode, "startup_stop") == 0)
      assert(h.starts == 0 && h.acks == 1 && status.error.code == CAI_OK);
    else if (strcmp(mode, "stop_active") == 0)
      assert(h.starts == 1);
    else
      assert(h.starts ==
                 (strcmp(mode, "run") == 0 || strcmp(mode, "cancel") == 0 ||
                          strcmp(mode, "provider_error") == 0
                      ? 2
                  : strcmp(mode, "queue") == 0 ? 3
                                               : 1) &&
             h.text > 0);
    if (strcmp(mode, "goal") == 0)
      assert(status.goal.has_goal &&
             strcmp(status.goal.status, "complete") == 0);
    if (strcmp(mode, "provider_error") == 0)
      assert(status.error.code == CAI_OK);
  }
  assert(getenv("CAI_CONTROLLER_PRIVATE") == NULL);
  assert(getcwd(current, sizeof(current)) != NULL && strcmp(current, cwd) == 0);
  assert(strcmp(locale, setlocale(LC_CTYPE, NULL)) == 0);
  assert(sigaction(SIGINT, NULL, &after) == 0 &&
         before.sa_handler == after.sa_handler);
  free(locale);
  if (strcmp(mode, "run") == 0) {
    const char *invalid[] = {NULL};
    cai_config_context context;
    memset(&context, 0, sizeof(context));
    context.directory = "relative";
    assert(cai_app_start(h.app, &context, 0, NULL, NULL) == CAI_ERR_INVALID);
    assert(cai_app_start(h.app, NULL, 1, invalid, NULL) == CAI_ERR_INVALID);
    h.mode = "startup_stop";
    h.has_owner = h.starts = h.acks = 0;
    assert(cai_app_start(h.app, NULL, argc - 1, (const char *const *)(argv + 1),
                         NULL) == CAI_OK);
    assert(cai_app_join(h.app, &code, NULL) == CAI_OK && code == 0);
    assert(h.starts == 0 && h.acks == 1);
  }
  cai_app_close(h.app);
  cai_app_status_cleanup(&status);
  pthread_mutex_destroy(&h.lock);
  return 0;
}
