/** @file Renderer-free application configuration and owner lifetime. */
#define _GNU_SOURCE
#include "identity.h"
#include "pouch_stream.h"
#include "server_internal.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void cai_app_config_init(cai_app_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->program_name = CAI_APP_DEFAULT_PROGRAM_NAME;
  }
}
static void controller_arguments_close(cai_app_server *app) {
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
  cai_app_server *app = context;
  app->controller_code = cai_cli_app_controller_run(
      app->argument_count + 1, app->arguments, &app->config, app->bridge,
      &app->startup, app->server ? &app->lifetime : NULL,
      app->lifetime_explicit, app->frontend_startup, NULL);
  cai_app_bridge_end(app->bridge, app->controller_code, NULL);
  return NULL;
}
int cai_app_owner_start(cai_app_server *self, const cai_config_context *context,
                        int argc, const char *const *argv, int server_mode,
                        const cai_attachment_config *server,
                        int frontend_startup, cai_error *error) {
  cai_app_server *app;
  int i, rc;
  if (self == NULL || argc < 0 || argc == INT_MAX ||
      (argc > 0 && argv == NULL) ||
      (context != NULL && context->directory != NULL &&
       context->directory[0] != '/'))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid controller startup arguments");
  app = self;
  if (app->running)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application is already running");
  app->server = server_mode;
  app->frontend_startup = frontend_startup;
  app->lifetime_explicit = server != NULL;
  cai_attachment_config_init(&app->lifetime);
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
int cai_app_server_start(cai_app_server *self,
                         const cai_config_context *context, int argc,
                         const char *const *argv,
                         const cai_attachment_config *lifetime,
                         cai_error *error) {
  return cai_app_owner_start(self, context, argc, argv, 1, lifetime, 0, error);
}
int cai_app_server_join(cai_app_server *self, int *exit_code,
                        cai_error *error) {
  cai_app_server *app;
  if (self == NULL || exit_code == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "controller join outputs required");
  app = self;
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
int cai_app_server_control(cai_app_server *self, cai_app_control_type type,
                           const char *text, unsigned long long *id,
                           cai_error *error) {
  cai_app_server *app;
  if (id != NULL)
    *id = 0U;
  if (self == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "application is required");
  app = self;
  return cai_app_bridge_control(app->bridge, type, text, id, error);
}
int cai_app_server_get_status(const cai_app_server *self, cai_app_status *out,
                              cai_error *error) {
  const cai_app_server *app;
  if (self == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "application is required");
  app = self;
  return cai_app_bridge_status(app->bridge, out, error);
}
int cai_app_server_status_fd(const cai_app_server *self) {
  const cai_app_server *app = self;
  return app != NULL ? app->bridge->status_fd : -1;
}
int cai_app_server_observe_status(const cai_app_server *self,
                                  cai_app_status *out, cai_error *error) {
  const cai_app_server *app = self;
  return cai_app_bridge_observe(app != NULL ? app->bridge : NULL, out, error);
}
int cai_app_server_connect_input(cai_app_server *self, uint64_t attachment,
                                 cai_input_connection **out, cai_error *error) {
  cai_app_server *app = self;
  return cai_app_bridge_input(app != NULL ? app->bridge : NULL, attachment, 0U,
                              0U, out, error);
}
int cai_app_server_resume_input(cai_app_server *self, uint64_t attachment,
                                unsigned long long conversation,
                                unsigned long long producer,
                                cai_input_connection **out, cai_error *error) {
  cai_app_server *app = self;
  if (out != NULL)
    *out = NULL;
  if (conversation == 0U || producer == 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "verified conversation and producer required");
  return cai_app_bridge_input(app != NULL ? app->bridge : NULL, attachment,
                              conversation, producer, out, error);
}
void cai_app_server_close(cai_app_server *self) {
  cai_app_server *app;
  size_t i;
  if (self == NULL)
    return;
  app = self;
  if (app->running)
    return;
  for (i = 0U; app->strings != NULL && i < app->string_count; i++)
    free(app->strings[i]);
  free(app->strings);
  free(app->settings);
  cai_app_bridge_close(app->bridge);
  free(app);
}
int cai_app_server_open(const cai_app_config *config, cai_app_server **out,
                        cai_error *error) {
  cai_app_config defaults;
  cai_config *schema = NULL;
  cai_app_server *app;
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
  app->config.setting_count = config->setting_count;
  app->string_count = base_count + 3U * config->setting_count;
  app->strings = calloc(app->string_count, sizeof(char *));
  if (app->strings == NULL) {
    cai_app_server_close(app);
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
    cai_app_server_close(app);
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
    cai_app_server_close(app);
    return rc;
  }
  rc = cai_app_bridge_open(&app->bridge, &app->config, error);
  if (rc != CAI_OK) {
    cai_app_server_close(app);
    return rc;
  }
  *out = app;
  return CAI_OK;
}
