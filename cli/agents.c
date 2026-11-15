#define _POSIX_C_SOURCE 200809L
#include "agents.h"
#include "pouch.h"
#include "stores.h"
#include <stdlib.h>
#include <string.h>

typedef struct agent_configuration {
  cai_config receiver;
  cai_config *base;
  cai_cli_pouch *repository;
  cai_cli_log *log;
} agent_configuration;

int cai_cli_agent_bootstrap_field(const char *section, const char *key) {
  if (strcmp(section, "env") == 0 && strncmp(key, "LOG_", 4U) == 0)
    return 1;
  if (strcmp(section, "ui") == 0 && strcmp(key, "temporary") == 0)
    return 1;
  if (strcmp(section, "runtime") == 0 &&
      (strcmp(key, "workspace_directory") == 0 ||
       strcmp(key, "session_scope") == 0 || strcmp(key, "session_id") == 0 ||
       strcmp(key, "resume_latest") == 0 ||
       strcmp(key, "resume_session_id") == 0 ||
       strcmp(key, "record_transcript") == 0 ||
       strcmp(key, "disable_default_session_store") == 0))
    return 1;
  if (strncmp(section, "store.", 6U) == 0 || strcmp(section, "server") == 0)
    return 1;
  if (strcmp(section, "storage") == 0)
    return strcmp(key, "conversation_retention_days") != 0;
  if (strcmp(section, "paths") != 0)
    return 0;
  return strcmp(key, "home_dir") == 0 || strcmp(key, "state_dir") == 0 ||
         strcmp(key, "cache_dir") == 0 || strcmp(key, "data_dir") == 0 ||
         strcmp(key, "runtime_dir") == 0 || strcmp(key, "config_dir") == 0 ||
         strcmp(key, "directory") == 0 || strcmp(key, "log_dir") == 0 ||
         strcmp(key, "clipboard_dir") == 0 || strcmp(key, "export_dir") == 0;
}

static void configuration_close(cai_config *self) {
  agent_configuration *owned = (agent_configuration *)self;
  if (owned->repository != NULL) {
    cai_cli_pouch_close(owned->repository);
    free(owned->repository);
  }
  cai_config_close(owned->base);
  (void)cai_cli_log_release(owned->log);
  free(owned);
}

cai_cli_log *cai_cli_agent_configuration_log(cai_config *configuration,
                                             int interactive) {
  if (configuration != NULL && configuration->close == configuration_close) {
    agent_configuration *owned = (agent_configuration *)configuration;
    if (owned->log != NULL && owned->log->initialized) {
      pthread_mutex_lock(&owned->log->lock);
      owned->log->interactive = interactive;
      pthread_mutex_unlock(&owned->log->lock);
    }
    return owned->log != NULL ? cai_cli_log_retain(owned->log->logger) : NULL;
  }
  return NULL;
}

void cai_cli_agent_configuration_detach(cai_config *configuration) {
  if (configuration != NULL && configuration->close == configuration_close) {
    agent_configuration *owned = (agent_configuration *)configuration;
    if (owned->repository != NULL) {
      cai_cli_pouch_close(owned->repository);
      free(owned->repository);
      owned->repository = NULL;
    }
    (void)cai_cli_log_release(owned->log);
    owned->log = NULL;
  }
}

typedef struct configuration_writer {
  cai_sink *sink;
  int globals;
} configuration_writer;

static int write_text(cai_sink *sink, const char *text, cai_error *error) {
  return cai_sink_write(sink, text, strlen(text), error);
}

/** Emit each owned setting directly; the transport bridge bounds output
 * backpressure, and no whole INI/document staging is introduced. */
static int write_entry(void *context, const char *section, const char *key,
                       const cai_config_field *field, const char *const *values,
                       size_t count, cai_error *error) {
  configuration_writer *writer = context;
  size_t i;
  int rc = CAI_OK;
  (void)field;
  if (cai_cli_agent_bootstrap_field(section, key))
    return CAI_OK;
  if ((section[0] == '\0') != writer->globals)
    return CAI_OK;
  /* Escaping can expand a valid decoded value beyond the decoder's 1 MiB
   * line contract. Reject before publishing an attachment we cannot reload. */
  for (i = 0U; i < count; ++i) {
    const unsigned char *p = (const unsigned char *)values[i];
    size_t encoded = strlen(key) + 6U;
    for (; *p != '\0'; ++p) {
      encoded +=
          *p == '\n' || *p == '\r' || *p == '\t' || *p == '\\' || *p == '"'
              ? 2U
              : 1U;
      if (encoded > 1024U * 1024U)
        return cai_cli_error(error, CAI_ERR_LIMIT,
                             "stored agent setting exceeds encoded INI line "
                             "limit");
    }
  }
  if (section[0] != '\0') {
    rc = write_text(writer->sink, "[", error);
    if (rc == CAI_OK)
      rc = write_text(writer->sink, section, error);
    if (rc == CAI_OK)
      rc = write_text(writer->sink, "]\n", error);
  }
  for (i = 0U; i < count && rc == CAI_OK; ++i) {
    const char *p = values[i];
    rc = write_text(writer->sink, key, error);
    if (rc == CAI_OK)
      rc = write_text(writer->sink, " = \"", error);
    while (*p != '\0' && rc == CAI_OK) {
      const char *begin = p;
      const char *escape;
      while (*p != '\0' && *p != '\n' && *p != '\r' && *p != '\t' &&
             *p != '\\' && *p != '"')
        ++p;
      if (p != begin)
        rc = cai_sink_write(writer->sink, begin, (size_t)(p - begin), error);
      if (*p == '\0' || rc != CAI_OK)
        break;
      escape = *p == '\n'   ? "\\n"
               : *p == '\r' ? "\\r"
               : *p == '\t' ? "\\t"
               : *p == '\\' ? "\\\\"
               : *p == '"'  ? "\\\""
                            : NULL;
      rc = write_text(writer->sink, escape, error);
      ++p;
    }
    if (rc == CAI_OK)
      rc = write_text(writer->sink, "\"\n", error);
  }
  return rc;
}

int cai_cli_agent_configuration_write(void *context, cai_sink *sink,
                                      cai_error *error) {
  configuration_writer writer;
  int rc;
  writer.sink = sink;
  writer.globals = 1;
  rc = cai_config_visit(context, write_entry, &writer, error);
  writer.globals = 0;
  return rc == CAI_OK ? cai_config_visit(context, write_entry, &writer, error)
                      : rc;
}

static int copy_host(void *context, const char *section, const char *key,
                     const cai_config_field *field, const char *const *values,
                     size_t count, cai_error *error) {
  size_t i;
  (void)field;
  if (!cai_cli_agent_bootstrap_field(section, key))
    return CAI_OK;
  for (i = 0U; i < count; ++i) {
    int rc = cai_config_set(context, section, key, values[i], error);
    if (rc != CAI_OK)
      return rc;
  }
  return CAI_OK;
}

static int validate_agent(void *context, const char *section, const char *key,
                          const cai_config_field *field,
                          const char *const *values, size_t count,
                          cai_error *error) {
  (void)context;
  (void)field;
  (void)values;
  (void)count;
  return cai_cli_agent_bootstrap_field(section, key)
             ? cai_cli_error(error, CAI_ERR_INVALID,
                             "stored agent contains host bootstrap settings")
             : CAI_OK;
}

static size_t empty_read(void *context, void *bytes, size_t count,
                         cai_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return 0U;
}

int cai_cli_agent_bootstrap_configuration(const cai_config *bootstrap,
                                          const cai_config_context *startup,
                                          cai_config **out, cai_error *error) {
  cai_config_context context = {0};
  cai_source_callbacks callbacks = {0};
  cai_config_field *fields;
  cai_source *origin = NULL;
  size_t i, count = cai_config_field_count(bootstrap);
  int rc;
  *out = NULL;
  fields = calloc(count, sizeof(*fields));
  if (fields == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate bootstrap schema");
  for (i = 0U; i < count; ++i)
    fields[i] = *cai_config_field_at(bootstrap, i);
  context.directory = cai_config_directory(bootstrap);
  context.environment = startup != NULL ? startup->environment : NULL;
  rc = cai_config_open_schema(fields, count, &context, out, error);
  free(fields);
  /* Establish the diagnostic/source identity without rereading a file. All
   * copied host paths have already resolved against the original source. */
  if (rc == CAI_OK && cai_config_origin(bootstrap) != NULL) {
    callbacks.read = empty_read;
    rc = cai_source_from_callbacks(&callbacks, &origin, error);
    if (rc == CAI_OK)
      rc = cai_config_load(*out, origin, cai_config_origin(bootstrap), error);
    cai_source_close(origin);
  }
  if (rc == CAI_OK)
    rc = cai_config_visit(bootstrap, copy_host, *out, error);
  if (rc != CAI_OK) {
    cai_config_close(*out);
    *out = NULL;
  }
  return rc;
}

int cai_cli_agent_configuration(cai_config *bootstrap, cai_config *seed,
                                const cai_cli_options *options,
                                const cai_app_config *application,
                                const cai_config_context *startup,
                                cai_config **out, cai_error *error) {
  agent_configuration *owned = calloc(1U, sizeof(*owned));
  cai_config_field *fields = NULL;
  cai_config_context context = {0};
  cai_environment *logging_environment = NULL;
  cai_cli_options selected;
  cai_source *source = NULL;
  size_t i, count = cai_config_field_count(bootstrap);
  int rc;
  *out = NULL;
  if (owned == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate stored agent");
  owned->repository = calloc(1U, sizeof(*owned->repository));
  fields = calloc(count, sizeof(*fields));
  if (owned->repository == NULL || fields == NULL) {
    rc = cai_cli_error(error, CAI_ERR_NOMEM, "allocate agent repository");
    goto done;
  }
  for (i = 0U; i < count; ++i)
    fields[i] = *cai_config_field_at(bootstrap, i);
  context.directory = cai_config_directory(bootstrap);
  if (startup != NULL && startup->environment != NULL) {
    context.environment = startup->environment;
    rc = cai_environment_retain(context.environment, error);
    if (rc != CAI_OK)
      context.environment = NULL;
  } else
    rc = cai_environment_open(NULL, &context.environment, error);
  if (rc == CAI_OK)
    rc = cai_config_open_schema(fields, count, &context, &owned->base, error);
  if (rc == CAI_OK)
    rc = cai_config_environment(bootstrap, &logging_environment, error);
  if (rc == CAI_OK)
    rc = cai_cli_log_new_at(&owned->log, PSLOG_LEVEL_TRACE, 1,
                            application != NULL ? application->logger : NULL,
                            logging_environment, options->directory, error);
  if (rc == CAI_OK)
    rc = cai_cli_store_options(bootstrap, options, options->agents_store,
                               &selected, error);
  if (rc == CAI_OK)
    rc = cai_cli_pouch_open_options(owned->repository, &selected,
                                    owned->log->logger, error);
  /* Persist declared paths against their source, not incidental startup
   * defaults such as an optional models.json discovered on this invocation. */
  for (i = 0U; rc == CAI_OK && i < cai_config_field_count(seed); ++i) {
    const cai_config_field *field = cai_config_field_at(seed, i);
    const char *resolved;
    if ((field->flags & CAI_CONFIG_PATH) != 0U &&
        strchr(field->section, '*') == NULL &&
        !cai_cli_agent_bootstrap_field(field->section, field->key) &&
        cai_config_get(seed, field->section, field->key) != NULL)
      rc = cai_config_resolve_path(seed, field->section, field->key, &resolved,
                                   error);
  }
  if (rc == CAI_OK)
    rc = cai_cli_pouch_default_agent(owned->repository,
                                     cai_cli_agent_configuration_write, seed,
                                     &source, error);
  if (rc == CAI_OK)
    rc = cai_config_load(owned->base, source, cai_config_origin(bootstrap),
                         error);
  cai_source_close(source);
  if (rc == CAI_OK)
    rc = cai_config_visit(owned->base, validate_agent, NULL, error);
  if (rc == CAI_OK)
    rc = cai_config_visit(bootstrap, copy_host, owned->base, error);
done:
  cai_environment_close(logging_environment);
  cai_environment_close(context.environment);
  free(fields);
  if (rc != CAI_OK) {
    if (owned->repository != NULL) {
      cai_cli_pouch_close(owned->repository);
      free(owned->repository);
    }
    cai_config_close(owned->base);
    (void)cai_cli_log_release(owned->log);
    free(owned);
    return rc;
  }
  owned->receiver = *owned->base;
  owned->receiver.close = configuration_close;
  *out = &owned->receiver;
  return CAI_OK;
}
