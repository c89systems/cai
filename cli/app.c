#define _GNU_SOURCE
#include "app_internal.h"
#include "identity.h"
#include "pouch_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;
static pthread_mutex_t application_lock = PTHREAD_MUTEX_INITIALIZER;
static const int signals[] = {SIGINT,  SIGTERM, SIGHUP,  SIGQUIT, SIGWINCH,
                              SIGTSTP, SIGCONT, SIGPIPE, SIGTTOU, SIGTTIN};
typedef struct app_impl {
  cai_app shell;
  cai_app_config config;
  cai_app_setting *settings;
  char **strings;
  size_t string_count;
  int running;
  cai_app_bridge *bridge;
  pthread_t owner;
  int asynchronous, controller_code;
  int argument_count;
  char **arguments;
  char *directory;
  cai_config_context startup;
  cai_attachment_config lifetime;
  int server;
} app_impl;
typedef struct process_state {
  char **environment;
  size_t count;
  char *locale;
  int flags[3];
  struct sigaction actions[sizeof(signals) / sizeof(signals[0])];
  sigset_t mask;
  struct termios terminal;
  int has_terminal;
} process_state;

void cai_app_config_init(cai_app_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  }
}
static void process_free(process_state *saved) {
  size_t i;
  for (i = 0U; i < saved->count; i++)
    free(saved->environment[i]);
  free(saved->environment);
  free(saved->locale);
}
static int process_save(process_state *saved, cai_error *error) {
  size_t i;
  memset(saved, 0, sizeof(*saved));
  while (environ != NULL && environ[saved->count] != NULL)
    saved->count++;
  saved->environment = calloc(saved->count + 1U, sizeof(char *));
  if (saved->environment == NULL) {
    saved->count = 0U;
    return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host environment");
  }
  for (i = 0U; i < saved->count; i++) {
    saved->environment[i] = strdup(environ[i]);
    if (saved->environment[i] == NULL)
      return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host environment");
  }
  saved->locale = strdup(setlocale(LC_CTYPE, NULL));
  if (saved->locale == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "snapshot host locale");
  if (pthread_sigmask(SIG_SETMASK, NULL, &saved->mask) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "snapshot host process state");
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (sigaction(signals[i], NULL, &saved->actions[i]) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT, "snapshot host signals");
  for (i = 0U; i < 3U; i++)
    saved->flags[i] = fcntl((int)i, F_GETFL);
  saved->has_terminal = tcgetattr(STDIN_FILENO, &saved->terminal) == 0;
  return CAI_OK;
}
static int process_restore(process_state *saved, cai_error *error) {
  size_t i;
  int failed = 0;
  if (clearenv() != 0)
    failed = 1;
  for (i = 0U; i < saved->count; i++) {
    char *equal = strchr(saved->environment[i], '=');
    if (equal != NULL) {
      *equal = '\0';
      if (setenv(saved->environment[i], equal + 1, 1) != 0)
        failed = 1;
      *equal = '=';
    }
  }
  if (setlocale(LC_CTYPE, saved->locale) == NULL)
    failed = 1;
  for (i = 0U; i < sizeof(signals) / sizeof(signals[0]); i++)
    if (sigaction(signals[i], &saved->actions[i], NULL) != 0)
      failed = 1;
  if (pthread_sigmask(SIG_SETMASK, &saved->mask, NULL) != 0)
    failed = 1;
  if (saved->has_terminal &&
      tcsetattr(STDIN_FILENO, TCSANOW, &saved->terminal) != 0)
    failed = 1;
  for (i = 0U; i < 3U; i++)
    if (saved->flags[i] >= 0 && fcntl((int)i, F_SETFL, saved->flags[i]) != 0)
      failed = 1;
  return failed ? cai_cli_error(error, CAI_ERR_TRANSPORT,
                                "could not restore host process state")
                : CAI_OK;
}
int cai_app_run(cai_app *self, int argc, const char *const *argv,
                int *exit_code, cai_error *error) {
  app_impl *app;
  process_state saved;
  char **arguments;
  int i, rc;
  if (self == NULL || exit_code == NULL || argc < 0 || argc == INT_MAX ||
      (argc > 0 && argv == NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application arguments");
  *exit_code = 1;
  app = self->impl;
  if (app->running)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application is already running");
  if (pthread_mutex_trylock(&application_lock) != 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "an application is already running");
  app->running = 1;
  cai_app_bridge_begin(app->bridge);
  arguments = calloc((size_t)argc + 2U, sizeof(*arguments));
  rc = arguments != NULL ? CAI_OK : CAI_ERR_NOMEM;
  for (i = 0; rc == CAI_OK && i <= argc; i++) {
    const char *value = i == 0 ? (app->config.invocation_name != NULL
                                      ? app->config.invocation_name
                                      : app->config.program_name)
                               : argv[i - 1];
    if (value == NULL)
      rc = CAI_ERR_INVALID;
    else if ((arguments[i] = strdup(value)) == NULL)
      rc = CAI_ERR_NOMEM;
  }
  if (rc != CAI_OK) {
    cai_cli_error(error, rc, "copy application arguments");
  } else {
    rc = process_save(&saved, error);
    if (rc == CAI_OK) {
      *exit_code =
          cai_cli_app_run(argc + 1, arguments, &app->config, app->bridge);
      rc = process_restore(&saved, error);
    }
    process_free(&saved);
  }
  if (arguments != NULL) {
    for (i = 0; i <= argc; i++)
      free(arguments[i]);
    free(arguments);
  }
  cai_app_bridge_end(app->bridge, rc == CAI_OK ? *exit_code : 1,
                     rc != CAI_OK ? error : NULL);
  app->running = 0;
  pthread_mutex_unlock(&application_lock);
  return rc;
}
static void controller_arguments_close(app_impl *app) {
  int i;
  for (i = 0; app->arguments != NULL && i <= app->argument_count; ++i)
    free(app->arguments[i]);
  free(app->arguments);
  app->arguments = NULL;
  free(app->directory);
  app->directory = NULL;
  cai_environment_close(app->startup.environment);
  memset(&app->startup, 0, sizeof(app->startup));
}
static void *controller_run(void *context) {
  app_impl *app = context;
  app->controller_code = cai_cli_app_controller_run(
      app->argument_count + 1, app->arguments, &app->config, app->bridge,
      &app->startup, app->server ? &app->lifetime : NULL);
  cai_app_bridge_end(app->bridge, app->controller_code, NULL);
  return NULL;
}
static int controller_start(cai_app *self, const cai_config_context *context,
                            int argc, const char *const *argv,
                            const cai_attachment_config *server,
                            cai_error *error) {
  app_impl *app;
  int i, rc;
  if (self == NULL || argc < 0 || argc == INT_MAX ||
      (argc > 0 && argv == NULL) ||
      (context != NULL && context->directory != NULL &&
       context->directory[0] != '/'))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid controller startup arguments");
  app = self->impl;
  if (app->running)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application is already running");
  app->server = server != NULL;
  if (server != NULL)
    app->lifetime = *server;
  app->argument_count = argc;
  app->directory = context != NULL && context->directory != NULL
                       ? strdup(context->directory)
                       : getcwd(NULL, 0U);
  if (app->directory == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "capture controller directory");
  app->startup.directory = app->directory;
  if (context != NULL && context->environment != NULL) {
    rc = cai_environment_retain(context->environment, error);
    if (rc == CAI_OK)
      app->startup.environment = context->environment;
  } else
    rc = cai_environment_open(NULL, &app->startup.environment, error);
  if (rc == CAI_OK) {
    app->arguments = calloc((size_t)argc + 2U, sizeof(*app->arguments));
    if (app->arguments == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy controller arguments");
  }
  for (i = 0; rc == CAI_OK && i <= argc; ++i) {
    const char *value = i == 0 ? (app->config.invocation_name != NULL
                                      ? app->config.invocation_name
                                      : app->config.program_name)
                               : argv[i - 1];
    if (value == NULL)
      rc = cai_cli_error(error, CAI_ERR_INVALID, "null controller argument");
    else if ((app->arguments[i] = strdup(value)) == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy controller argument");
  }
  if (rc == CAI_OK) {
    app->running = app->asynchronous = 1;
    cai_app_bridge_begin(app->bridge);
    if (pthread_create(&app->owner, NULL, controller_run, app) != 0) {
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "start controller owner");
      cai_app_bridge_end(app->bridge, 1, error);
      app->running = app->asynchronous = 0;
    }
  }
  if (rc != CAI_OK)
    controller_arguments_close(app);
  return rc;
}
int cai_app_start(cai_app *self, const cai_config_context *context, int argc,
                  const char *const *argv, cai_error *error) {
  return controller_start(self, context, argc, argv, NULL, error);
}
int cai_app_start_server(cai_app *self, const cai_config_context *context,
                         int argc, const char *const *argv,
                         const cai_attachment_config *lifetime,
                         cai_error *error) {
  cai_attachment_config defaults;
  cai_attachment_config_init(&defaults);
  return controller_start(self, context, argc, argv,
                          lifetime != NULL ? lifetime : &defaults, error);
}
int cai_app_join(cai_app *self, int *exit_code, cai_error *error) {
  app_impl *app;
  if (self == NULL || exit_code == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "controller join outputs required");
  app = self->impl;
  if (!app->asynchronous || pthread_equal(app->owner, pthread_self()))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "join requires another running controller owner");
  if (pthread_join(app->owner, NULL) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "join controller owner");
  *exit_code = app->controller_code;
  app->running = app->asynchronous = 0;
  controller_arguments_close(app);
  return CAI_OK;
}
int cai_app_control(cai_app *self, cai_app_control_type type, const char *text,
                    unsigned long long *id, cai_error *error) {
  app_impl *app;
  if (id != NULL)
    *id = 0U;
  if (self == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "application is required");
  app = self->impl;
  return cai_app_bridge_control(app->bridge, type, text, id, error);
}
int cai_app_get_status(const cai_app *self, cai_app_status *out,
                       cai_error *error) {
  const app_impl *app;
  if (self == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "application is required");
  app = self->impl;
  return cai_app_bridge_status(app->bridge, out, error);
}
int cai_app_status_fd(const cai_app *self) {
  const app_impl *app = self != NULL ? self->impl : NULL;
  return app != NULL ? app->bridge->status_fd : -1;
}
int cai_app_observe_status(const cai_app *self, cai_app_status *out,
                           cai_error *error) {
  const app_impl *app = self != NULL ? self->impl : NULL;
  return cai_app_bridge_observe(app != NULL ? app->bridge : NULL, out, error);
}
int cai_app_connect_input(cai_app *self, uint64_t attachment,
                          cai_input_connection **out, cai_error *error) {
  app_impl *app = self != NULL ? self->impl : NULL;
  return cai_app_bridge_input(app != NULL ? app->bridge : NULL, attachment, 0U,
                              0U, out, error);
}
int cai_app_resume_input(cai_app *self, uint64_t attachment,
                         unsigned long long conversation,
                         unsigned long long producer,
                         cai_input_connection **out, cai_error *error) {
  app_impl *app = self != NULL ? self->impl : NULL;
  if (out != NULL)
    *out = NULL;
  if (conversation == 0U || producer == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "verified conversation and producer required");
  return cai_app_bridge_input(app != NULL ? app->bridge : NULL, attachment,
                              conversation, producer, out, error);
}
size_t cai_app_option_count(const cai_app *self) {
  return self != NULL ? cai_cli_help_count() : 0U;
}
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index) {
  return self != NULL ? cai_cli_help_at(index) : NULL;
}
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error) {
  const app_impl *app;
  char prefix[65];
  int rc;
  if (self == NULL || sink == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "help requires application and sink");
  app = self->impl;
  rc = cai_cli_environment_prefix(&app->config, prefix, error);
  if (rc != CAI_OK)
    return rc;
  return cai_cli_help_write(app->config.program_name,
                            app->config.invocation_name != NULL
                                ? app->config.invocation_name
                                : app->config.program_name,
                            prefix, sink, error);
}
void cai_app_close(cai_app *self) {
  app_impl *app;
  size_t i;
  if (self == NULL)
    return;
  app = self->impl;
  if (app->running)
    return;
  for (i = 0U; app->strings != NULL && i < app->string_count; i++)
    free(app->strings[i]);
  free(app->strings);
  free(app->settings);
  cai_app_bridge_close(app->bridge);
  free(app);
}
int cai_app_open(const cai_app_config *config, cai_app **out,
                 cai_error *error) {
  cai_app_config defaults;
  cai_config *schema = NULL;
  app_impl *app;
#define STRING_OFFSET(n) offsetof(cai_app_config, n),
  static const size_t offsets[] = {CAI_APP_STRINGS(STRING_OFFSET)};
#undef STRING_OFFSET
  char prefix[65];
  size_t base_count = sizeof(offsets) / sizeof(offsets[0]);
  size_t i;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application output is required");
  *out = NULL;
  cai_app_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (config->introduction_margin_left < 0 ||
      config->introduction_margin_right < 0)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "introduction margins must be nonnegative");
  if ((config->setting_count != 0U && config->settings == NULL) ||
      config->setting_count >
          ((size_t)-1 / sizeof(cai_app_setting) - base_count) / 3U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application settings");
  app = calloc(1U, sizeof(*app));
  if (app == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate application");
  app->shell.impl = app;
  app->shell.run = cai_app_run;
  app->shell.option_count = cai_app_option_count;
  app->shell.option_at = cai_app_option_at;
  app->shell.write_help = cai_app_write_help;
  app->shell.control = cai_app_control;
  app->shell.get_status = cai_app_get_status;
  app->shell.start = cai_app_start;
  app->shell.join = cai_app_join;
  app->shell.close = cai_app_close;
  app->config.setting_count = config->setting_count;
  app->string_count = base_count + 3U * config->setting_count;
  app->strings = calloc(app->string_count, sizeof(char *));
  if (app->strings == NULL) {
    cai_app_close(&app->shell);
    return cai_cli_error(error, CAI_ERR_NOMEM,
                         "copy application configuration");
  }
  app->config = *config;
  rc = CAI_OK;
  for (i = 0U; i < base_count; i++) {
    const char *value =
        *(const char *const *)((const char *)config + offsets[i]);
    if (offsets[i] == offsetof(cai_app_config, program_name) && value == NULL)
      value = CAI_APP_DEFAULT_PROGRAM_NAME;
    app->strings[i] = value != NULL ? strdup(value) : NULL;
    *(const char **)((char *)&app->config + offsets[i]) = app->strings[i];
    if (value != NULL && app->strings[i] == NULL)
      rc =
          cai_cli_error(error, CAI_ERR_NOMEM, "copy application configuration");
  }
  if (rc == CAI_OK)
    rc = cai_cli_environment_prefix(&app->config, prefix, error);
  if (rc == CAI_OK && app->config.invocation_name != NULL &&
      (app->config.invocation_name[0] == '\0' ||
       strlen(app->config.invocation_name) > 512U ||
       strpbrk(app->config.invocation_name, "\r\n\t") != NULL))
    rc = cai_cli_error(error, CAI_ERR_INVALID, "invalid invocation_name");
  if (rc == CAI_OK && app->config.queue_prefix != NULL &&
      !cai_cli_identity_valid(app->config.queue_prefix))
    rc = cai_cli_error(error, CAI_ERR_INVALID, "invalid queue_prefix");
  if (rc != CAI_OK) {
    cai_app_close(&app->shell);
    return rc;
  }
  rc = cai_cli_app_schema(&schema, error) < 0 ? CAI_ERR_INVALID : CAI_OK;
  if (config->setting_count != 0U) {
    app->settings = calloc(config->setting_count, sizeof(*app->settings));
    if (app->settings == NULL)
      rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy application settings");
  }
  for (i = 0U; rc == CAI_OK && i < config->setting_count; i++) {
    const cai_app_setting *s = &config->settings[i];
    rc = cai_config_set(schema, s->section, s->key, s->value, error);
    if (rc == CAI_OK) {
      app->settings[i].section = app->strings[base_count + i * 3U] =
          strdup(s->section);
      app->settings[i].key = app->strings[base_count + 1U + i * 3U] =
          strdup(s->key);
      app->settings[i].value = app->strings[base_count + 2U + i * 3U] =
          strdup(s->value);
      if (app->settings[i].section == NULL || app->settings[i].key == NULL ||
          app->settings[i].value == NULL)
        rc = cai_cli_error(error, CAI_ERR_NOMEM, "copy application setting");
    }
  }
  cai_config_close(schema);
  app->config.settings = app->settings;
  if (rc != CAI_OK) {
    cai_app_close(&app->shell);
    return rc;
  }
  rc = cai_app_bridge_open(&app->bridge, &app->config, error);
  if (rc != CAI_OK) {
    cai_app_close(&app->shell);
    return rc;
  }
  *out = &app->shell;
  return CAI_OK;
}
