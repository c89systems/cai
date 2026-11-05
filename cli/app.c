/** @file Combined app facade: compose the shared owner with a socket frontend,
 * and retain process-scoped maintenance utilities outside owner lifetime. */
#define _GNU_SOURCE
#include "identity.h"
#include "log.h"
#include "pouch_stream.h"
#include "run_frontend.h"
#include "server_internal.h"
#include "utility.h"
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
  cai_app_server *owner;
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
  cai_app_server *app;
  process_state saved;
  char **arguments;
  int i, rc, handled = 0;
  if (self == NULL || exit_code == NULL || argc < 0 || argc == INT_MAX ||
      (argc > 0 && argv == NULL))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application arguments");
  *exit_code = 1;
  app = ((app_impl *)self->impl)->owner;
  if (app->running)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application is already running");
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
    rc = cai_cli_run_frontend(&app->config, app->bridge, argc + 1, arguments,
                              NULL, &handled, exit_code, error);
    if (rc == CAI_OK && !handled) {
      if (pthread_mutex_trylock(&application_lock) != 0)
        rc = cai_cli_error(error, CAI_ERR_INVALID,
                           "a utility application is already running");
      else {
        rc = process_save(&saved, error);
        if (rc == CAI_OK) {
          *exit_code = cai_cli_utility_run(argc + 1, arguments, &app->config,
                                           app->bridge);
          rc = process_restore(&saved, error);
        }
        process_free(&saved);
        pthread_mutex_unlock(&application_lock);
      }
    }
  }
  if (arguments != NULL) {
    for (i = 0; i <= argc; i++)
      free(arguments[i]);
    free(arguments);
  }
  cai_app_bridge_end(app->bridge, rc == CAI_OK ? *exit_code : 1,
                     rc != CAI_OK ? error : NULL);
  app->running = 0;
  return rc;
}
size_t cai_app_option_count(const cai_app *self) {
  return self != NULL ? cai_cli_help_count() : 0U;
}
const cai_app_option *cai_app_option_at(const cai_app *self, size_t index) {
  return self != NULL ? cai_cli_help_at(index) : NULL;
}
int cai_app_write_help(const cai_app *self, cai_sink *sink, cai_error *error) {
  const cai_app_server *app;
  char prefix[65];
  int rc;
  if (self == NULL || sink == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "help requires application and sink");
  app = ((app_impl *)self->impl)->owner;
  rc = cai_cli_environment_prefix(&app->config, prefix, error);
  if (rc != CAI_OK)
    return rc;
  return cai_cli_help_write(app->config.program_name,
                            app->config.invocation_name != NULL
                                ? app->config.invocation_name
                                : app->config.program_name,
                            prefix, sink, error);
}

static cai_app_server *owner(const cai_app *self) {
  return self != NULL ? ((app_impl *)self->impl)->owner : NULL;
}
int cai_app_start(cai_app *self, const cai_config_context *context, int argc,
                  const char *const *argv, cai_error *error) {
  return cai_app_owner_start(owner(self), context, argc, argv, 0, NULL, 0,
                             error);
}
int cai_app_start_server(cai_app *self, const cai_config_context *context,
                         int argc, const char *const *argv,
                         const cai_attachment_config *lifetime,
                         cai_error *error) {
  return cai_app_server_start(owner(self), context, argc, argv, lifetime,
                              error);
}
int cai_app_start_frontend_owner(cai_app *self,
                                 const cai_config_context *context, int argc,
                                 const char *const *argv, cai_error *error) {
  return cai_app_owner_start(owner(self), context, argc, argv, 1, NULL,
                             CAI_CLI_OWNER_FRONTEND, error);
}
int cai_app_join(cai_app *self, int *exit_code, cai_error *error) {
  return cai_app_server_join(owner(self), exit_code, error);
}
int cai_app_control(cai_app *self, cai_app_control_type type, const char *text,
                    unsigned long long *id, cai_error *error) {
  return cai_app_server_control(owner(self), type, text, id, error);
}
int cai_app_get_status(const cai_app *self, cai_app_status *out,
                       cai_error *error) {
  return cai_app_server_get_status(owner(self), out, error);
}
int cai_app_status_fd(const cai_app *self) {
  return cai_app_server_status_fd(owner(self));
}
int cai_app_observe_status(const cai_app *self, cai_app_status *out,
                           cai_error *error) {
  return cai_app_server_observe_status(owner(self), out, error);
}
int cai_app_connect_input(cai_app *self, uint64_t attachment,
                          cai_input_connection **out, cai_error *error) {
  return cai_app_server_connect_input(owner(self), attachment, out, error);
}
int cai_app_resume_input(cai_app *self, uint64_t attachment,
                         unsigned long long conversation,
                         unsigned long long producer,
                         cai_input_connection **out, cai_error *error) {
  return cai_app_server_resume_input(owner(self), attachment, conversation,
                                     producer, out, error);
}
void cai_app_close(cai_app *self) {
  app_impl *app;
  if (self == NULL)
    return;
  app = self->impl;
  if (app->owner->running)
    return;
  cai_app_server_close(app->owner);
  free(app);
}
int cai_app_open(const cai_app_config *config, cai_app **out,
                 cai_error *error) {
  app_impl *app;
  cai_app_server *server;
  int rc;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application output is required");
  *out = NULL;
  rc = cai_app_server_open(config, &server, error);
  if (rc != CAI_OK)
    return rc;
  app = calloc(1U, sizeof(*app));
  if (app == NULL) {
    cai_app_server_close(server);
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate application facade");
  }
  app->owner = server;
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
  *out = &app->shell;
  return CAI_OK;
}

int cai_cli_app_main(int argc, char **argv, const cai_app_config *application) {
  cai_app *app = NULL;
  cai_error error;
  int code = 1, rc;
  cai_error_init(&error);
  rc = cai_app_open(application, &app, &error);
  if (rc == CAI_OK)
    rc = cai_app_run(app, argc - 1, (const char *const *)(argv + 1), &code,
                     &error);
  if (rc != CAI_OK) {
    if (application != NULL && application->logger != NULL)
      cai_cli_log_error(application->logger, "application", &error);
    else
      fprintf(stderr, "application: %s\n",
              error.message != NULL ? error.message : cai_status_string(rc));
  }
  cai_app_close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
