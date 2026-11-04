#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "cai_internal.h"
#include "cai_plugins_internal.h"
#include <cai/environment.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct plugin_entry {
  struct plugin_entry *next;
  size_t references;
  cai_plugin_definition definition;
  cai_plugin_handler handler;
  char **argv;
  char *strings[6];
} plugin_entry;
typedef struct plugin_executor {
  struct plugin_executor *next;
  char *name;
  cai_plugin_executor factory;
} plugin_executor;
struct cai_plugins {
  pthread_mutex_t lock;
  size_t references;
  unsigned long generation;
  plugin_entry *entries;
  plugin_executor *executors;
  char **directories;
  size_t directory_count;
  char **reserved;
  size_t reserved_count;
};
typedef struct plugin_proxy {
  cai_agent_impl *agent;
  plugin_entry *entry;
} plugin_proxy;
typedef struct plugin_manifest {
  char *name, *description, *executor, *entrypoint;
  lonejson_json_value parameters;
  lonejson_string_array argv;
  int strict;
} plugin_manifest;
static const lonejson_field manifest_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(plugin_manifest, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(plugin_manifest, description,
                                    "description"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(plugin_manifest, executor, "executor"),
    LONEJSON_FIELD_STRING_ALLOC(plugin_manifest, entrypoint, "entrypoint"),
    {"parameters", 10U, 'p', 's', offsetof(plugin_manifest, parameters),
     LONEJSON_FIELD_KIND_JSON_VALUE, LONEJSON_STORAGE_FIXED,
     LONEJSON_OVERFLOW_FAIL,
     LONEJSON_FIELD_REQUIRED | LONEJSON_FIELD_JSON_VALUE_DEFAULT_CAPTURE, 0U,
     0U, NULL, NULL, 0U, LONEJSON_SPOOL_CLASS_DEFAULT},
    LONEJSON_FIELD_STRING_ARRAY(plugin_manifest, argv, "argv",
                                LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_BOOL(plugin_manifest, strict, "strict")};
LONEJSON_MAP_DEFINE(manifest_map, plugin_manifest, manifest_fields);
typedef struct plugin_schema {
  char *type;
} plugin_schema;
static const lonejson_field schema_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(plugin_schema, type, "type")};
LONEJSON_MAP_DEFINE(schema_map, plugin_schema, schema_fields);

void cai_plugins_config_init(cai_plugins_config *config) {
  if (config != NULL)
    memset(config, 0, sizeof(*config));
}
void cai_plugin_call_init(cai_plugin_call *call) {
  if (call != NULL) {
    memset(call, 0, sizeof(*call));
    call->timeout_ms = 120000L;
    call->max_output_bytes = 4U * 1024U * 1024U;
  }
}
static int plugin_name(const char *name) {
  size_t i;
  if (name == NULL || !((name[0] >= 'A' && name[0] <= 'Z') ||
                        (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
    return 0;
  for (i = 0U; name[i] != '\0'; i++)
    if (i >= 64U || !((name[i] >= 'A' && name[i] <= 'Z') ||
                      (name[i] >= 'a' && name[i] <= 'z') ||
                      (name[i] >= '0' && name[i] <= '9') || name[i] == '_' ||
                      name[i] == '-'))
      return 0;
  return 1;
}
static void entry_free(plugin_entry *entry, int handler) {
  size_t i;
  if (handler && entry->handler.destroy != NULL)
    entry->handler.destroy(entry->handler.context);
  for (i = 0U; i < 6U; i++)
    cai_free_mem(NULL, entry->strings[i]);
  for (i = 0U; i < entry->definition.argc; i++)
    cai_free_mem(NULL, entry->argv[i]);
  cai_free_mem(NULL, entry->argv);
  cai_free_mem(NULL, entry);
}
static void entry_release(cai_plugins *plugins, plugin_entry *entry) {
  int last;
  pthread_mutex_lock(&plugins->lock);
  last = --entry->references == 0U;
  pthread_mutex_unlock(&plugins->lock);
  if (last)
    entry_free(entry, 1);
}
static plugin_entry *entry_find(cai_plugins *plugins, const char *name) {
  plugin_entry *entry;
  for (entry = plugins->entries; entry != NULL; entry = entry->next)
    if (strcmp(entry->definition.name, name) == 0)
      return entry;
  return NULL;
}
static int plugins_reserve(cai_plugins *plugins, const char *name,
                           cai_error *error) {
  char **grown;
  char *copy;
  size_t i;
  pthread_mutex_lock(&plugins->lock);
  if (entry_find(plugins, name) != NULL) {
    pthread_mutex_unlock(&plugins->lock);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin conflicts with a native tool");
  }
  for (i = 0U; i < plugins->reserved_count; i++)
    if (strcmp(plugins->reserved[i], name) == 0) {
      pthread_mutex_unlock(&plugins->lock);
      return CAI_OK;
    }
  copy = cai_strdup(NULL, name);
  grown = cai_realloc_mem(NULL, plugins->reserved,
                          (plugins->reserved_count + 1U) * sizeof(*grown));
  if (copy == NULL || grown == NULL) {
    if (grown != NULL)
      plugins->reserved = grown;
    cai_free_mem(NULL, copy);
    pthread_mutex_unlock(&plugins->lock);
    return cai_set_error(error, CAI_ERR_NOMEM, "reserve native tool name");
  }
  plugins->reserved = grown;
  grown[plugins->reserved_count++] = copy;
  pthread_mutex_unlock(&plugins->lock);
  return CAI_OK;
}
int cai_plugins_register_executor(cai_plugins *plugins, const char *name,
                                  const cai_plugin_executor *executor,
                                  cai_error *error) {
  plugin_executor *item, *next;
  if (plugins == NULL || !plugin_name(name) || executor == NULL ||
      executor->prepare == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin executor is required");
  next = cai_alloc(NULL, sizeof(*next));
  if (next == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate executor");
  memset(next, 0, sizeof(*next));
  next->name = cai_strdup(NULL, name);
  if (next->name == NULL) {
    cai_free_mem(NULL, next);
    return cai_set_error(error, CAI_ERR_NOMEM, "copy executor name");
  }
  next->factory = *executor;
  pthread_mutex_lock(&plugins->lock);
  for (item = plugins->executors; item != NULL; item = item->next)
    if (strcmp(name, item->name) == 0)
      break;
  if (item == NULL) {
    next->next = plugins->executors;
    plugins->executors = next;
    plugins->generation++;
  }
  pthread_mutex_unlock(&plugins->lock);
  if (item != NULL) {
    cai_free_mem(NULL, next->name);
    cai_free_mem(NULL, next);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "executor is already registered");
  }
  return CAI_OK;
}
int cai_plugins_open(const cai_plugins_config *config, cai_plugins **out,
                     cai_error *error) {
  cai_plugins *plugins;
  cai_plugin_executor process;
  static const char *const builtin_names[] = {
      "reload_tools",   "unload_tool",        "exec_command",
      "write_stdin",    "read_file",          "list_files",
      "apply_patch",    "view_image",         "get_goal",
      "create_goal",    "update_goal",        "clear_goal",
      "run_review",     "run_review_and_fix", "set_reasoning",
      "chrome",         "todo_kanban",        "agent_message",
      "searxng_search", "reverse_geocode"};
  size_t i;
  int rc;
  if (out == NULL || config == NULL ||
      (config->directory_count != 0U && config->directories == NULL))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin configuration required");
  *out = NULL;
  plugins = cai_alloc(NULL, sizeof(*plugins));
  if (plugins == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate plugins");
  memset(plugins, 0, sizeof(*plugins));
  if (pthread_mutex_init(&plugins->lock, NULL) != 0) {
    cai_free_mem(NULL, plugins);
    return cai_set_error(error, CAI_ERR_TRANSPORT, "initialize plugin lock");
  }
  plugins->references = 1U;
  plugins->directories =
      cai_alloc(NULL, (config->directory_count + 1U) * sizeof(char *));
  if (plugins->directories == NULL) {
    cai_plugins_close(plugins);
    return cai_set_error(error, CAI_ERR_NOMEM, "copy plugin roots");
  }
  memset(plugins->directories, 0,
         (config->directory_count + 1U) * sizeof(char *));
  for (i = 0U; i < config->directory_count; i++) {
    const char *root = config->directories[i];
    if (root == NULL || root[0] != '/') {
      cai_plugins_close(plugins);
      return cai_set_error(error, CAI_ERR_INVALID,
                           "plugin roots must be absolute");
    }
    plugins->directories[i] = cai_strdup(NULL, root);
    if (plugins->directories[i] == NULL) {
      cai_plugins_close(plugins);
      return cai_set_error(error, CAI_ERR_NOMEM, "copy plugin root");
    }
    plugins->directory_count++;
  }
  memset(&process, 0, sizeof(process));
  process.prepare = cai_plugin_process_prepare;
  rc = cai_plugins_register_executor(plugins, "process", &process, error);
  for (i = 0U;
       rc == CAI_OK && i < sizeof(builtin_names) / sizeof(builtin_names[0]);
       i++)
    rc = plugins_reserve(plugins, builtin_names[i], error);
  if (rc != CAI_OK) {
    cai_plugins_close(plugins);
    return rc;
  }
  *out = plugins;
  return CAI_OK;
}
static void plugins_retain(cai_plugins *plugins) {
  pthread_mutex_lock(&plugins->lock);
  plugins->references++;
  pthread_mutex_unlock(&plugins->lock);
}
void cai_plugins_close(cai_plugins *plugins) {
  plugin_entry *entry;
  plugin_executor *executor;
  size_t i;
  int last;
  if (plugins == NULL)
    return;
  pthread_mutex_lock(&plugins->lock);
  last = --plugins->references == 0U;
  pthread_mutex_unlock(&plugins->lock);
  if (!last)
    return;
  while ((entry = plugins->entries) != NULL) {
    plugins->entries = entry->next;
    entry_release(plugins, entry);
  }
  while ((executor = plugins->executors) != NULL) {
    plugins->executors = executor->next;
    cai_free_mem(NULL, executor->name);
    cai_free_mem(NULL, executor);
  }
  for (i = 0U; i < plugins->directory_count; i++)
    cai_free_mem(NULL, plugins->directories[i]);
  for (i = 0U; i < plugins->reserved_count; i++)
    cai_free_mem(NULL, plugins->reserved[i]);
  cai_free_mem(NULL, plugins->directories);
  cai_free_mem(NULL, plugins->reserved);
  pthread_mutex_destroy(&plugins->lock);
  cai_free_mem(NULL, plugins);
}
int cai_plugins_define(cai_plugins *plugins,
                       const cai_plugin_definition *definition,
                       const cai_plugin_handler *handler, cai_error *error) {
  plugin_entry *entry, **link, *old = NULL;
  plugin_schema schema;
  lonejson_error local;
  size_t i;
  int rc;
  if (plugins == NULL || definition == NULL || !plugin_name(definition->name) ||
      definition->description == NULL || definition->parameters_json == NULL ||
      handler == NULL || handler->invoke == NULL || definition->argc > 256U ||
      (definition->argc != 0U && definition->argv == NULL))
    return cai_set_error(error, CAI_ERR_INVALID, "invalid plugin definition");
  memset(&schema, 0, sizeof(schema));
  lonejson_error_init(&local);
  rc = CAI_LJ->parse_cstr(CAI_LJ, &schema_map, &schema,
                          definition->parameters_json, &local);
  if (rc != LONEJSON_STATUS_OK || schema.type == NULL ||
      strcmp(schema.type, "object") != 0) {
    CAI_LJ->cleanup(CAI_LJ, &schema_map, &schema);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin parameters must be an object JSON schema");
  }
  CAI_LJ->cleanup(CAI_LJ, &schema_map, &schema);
  entry = cai_alloc(NULL, sizeof(*entry));
  if (entry == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "allocate plugin");
  memset(entry, 0, sizeof(*entry));
  entry->references = 1U;
  entry->handler = *handler;
#define COPY_FIELD(field, index)                                               \
  entry->definition.field = entry->strings[index] =                            \
      cai_strdup(NULL, definition->field != NULL ? definition->field : "")
  COPY_FIELD(name, 0);
  COPY_FIELD(description, 1);
  COPY_FIELD(parameters_json, 2);
  COPY_FIELD(executor, 3);
  COPY_FIELD(directory, 4);
  COPY_FIELD(entrypoint, 5);
#undef COPY_FIELD
  entry->definition.strict = definition->strict;
  entry->argv = cai_alloc(NULL, (definition->argc + 1U) * sizeof(char *));
  if (entry->argv != NULL) {
    memset(entry->argv, 0, (definition->argc + 1U) * sizeof(char *));
    for (i = 0U; i < definition->argc; i++) {
      if (definition->argv[i] == NULL)
        break;
      entry->argv[i] = cai_strdup(NULL, definition->argv[i]);
      if (entry->argv[i] == NULL)
        break;
      entry->definition.argc++;
    }
  }
  entry->definition.argv = (const char *const *)entry->argv;
  if (entry->definition.name == NULL || entry->definition.description == NULL ||
      entry->definition.parameters_json == NULL ||
      entry->definition.executor == NULL ||
      entry->definition.directory == NULL ||
      entry->definition.entrypoint == NULL || entry->argv == NULL ||
      entry->definition.argc != definition->argc) {
    entry_free(entry, 0);
    return cai_set_error(error, CAI_ERR_NOMEM, "copy plugin definition");
  }
  pthread_mutex_lock(&plugins->lock);
  for (i = 0U; i < plugins->reserved_count; i++)
    if (strcmp(plugins->reserved[i], definition->name) == 0)
      break;
  if (i != plugins->reserved_count) {
    pthread_mutex_unlock(&plugins->lock);
    entry_free(entry, 0);
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin name is reserved by a native tool");
  }
  for (link = &plugins->entries; *link != NULL; link = &(*link)->next)
    if (strcmp((*link)->definition.name, definition->name) == 0)
      break;
  old = *link;
  entry->next = old != NULL ? old->next : NULL;
  *link = entry;
  plugins->generation++;
  pthread_mutex_unlock(&plugins->lock);
  if (old != NULL)
    entry_release(plugins, old);
  return CAI_OK;
}
static int approved_directory(cai_plugins *plugins, const char *directory,
                              char path[4096], cai_error *error) {
  char root[4096];
  size_t i, length;
  if (directory == NULL || realpath(directory, path) == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin directory does not exist");
  for (i = 0U; i < plugins->directory_count; i++) {
    if (realpath(plugins->directories[i], root) == NULL)
      continue;
    length = strlen(root);
    if (strncmp(path, root, length) == 0 &&
        (root[length - 1U] == '/' || path[length] == '/' ||
         path[length] == '\0'))
      return CAI_OK;
  }
  return cai_set_error(error, CAI_ERR_INVALID,
                       "plugin package is outside configured tool roots");
}
int cai_plugins_load(cai_plugins *plugins, const char *directory,
                     cai_error *error) {
  char path[4096], manifest_path[4096];
  char *bytes;
  FILE *file;
  size_t length;
  plugin_manifest manifest;
  cai_plugin_definition definition;
  cai_plugin_executor factory;
  cai_plugin_handler handler;
  plugin_executor *executor;
  lonejson_error local;
  int rc;
  if (plugins == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin catalog required");
  rc = approved_directory(plugins, directory, path, error);
  if (rc != CAI_OK)
    return rc;
  if (snprintf(manifest_path, sizeof(manifest_path), "%s/tool.json", path) >=
      (int)sizeof(manifest_path))
    return cai_set_error(error, CAI_ERR_INVALID, "plugin path too long");
  file = fopen(manifest_path, "rb");
  if (file == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "cannot open plugin tool.json");
  bytes = cai_alloc(NULL, 65538U);
  if (bytes == NULL) {
    fclose(file);
    return cai_set_error(error, CAI_ERR_NOMEM, "read plugin manifest");
  }
  length = fread(bytes, 1U, 65537U, file);
  rc = ferror(file) || length > 65536U ? CAI_ERR_INVALID : CAI_OK;
  fclose(file);
  bytes[length] = '\0';
  if (strlen(bytes) != length)
    rc = CAI_ERR_INVALID;
  memset(&manifest, 0, sizeof(manifest));
  lonejson_error_init(&local);
  if (rc == CAI_OK && CAI_LJ->parse_cstr(CAI_LJ, &manifest_map, &manifest,
                                         bytes, &local) != LONEJSON_STATUS_OK)
    rc = CAI_ERR_INVALID;
  cai_free_mem(NULL, bytes);
  if (rc != CAI_OK || manifest.parameters.json == NULL) {
    CAI_LJ->cleanup(CAI_LJ, &manifest_map, &manifest);
    return cai_set_error_detail(error, CAI_ERR_INVALID,
                                "invalid plugin manifest (maximum 64 KiB)",
                                local.message);
  }
  memset(&definition, 0, sizeof(definition));
  definition.name = manifest.name;
  definition.description = manifest.description;
  definition.parameters_json = manifest.parameters.json;
  definition.executor = manifest.executor;
  definition.directory = path;
  definition.entrypoint = manifest.entrypoint;
  definition.argv = (const char *const *)manifest.argv.items;
  definition.argc = manifest.argv.count;
  definition.strict = manifest.strict;
  memset(&factory, 0, sizeof(factory));
  memset(&handler, 0, sizeof(handler));
  pthread_mutex_lock(&plugins->lock);
  for (executor = plugins->executors; executor != NULL;
       executor = executor->next)
    if (strcmp(executor->name, definition.executor) == 0) {
      factory = executor->factory;
      break;
    }
  pthread_mutex_unlock(&plugins->lock);
  if (factory.prepare == NULL)
    rc =
        cai_set_error(error, CAI_ERR_INVALID, "plugin executor is unavailable");
  else
    rc = factory.prepare(factory.context, &definition, &handler, error);
  if (rc == CAI_OK)
    rc = cai_plugins_define(plugins, &definition, &handler, error);
  if (rc != CAI_OK && handler.destroy != NULL)
    handler.destroy(handler.context);
  CAI_LJ->cleanup(CAI_LJ, &manifest_map, &manifest);
  return rc;
}
int cai_plugins_reload(cai_plugins *plugins, cai_error *error) {
  DIR *directory;
  struct dirent *item;
  struct stat info;
  char path[4096], manifest[4096];
  size_t i;
  int rc = CAI_OK;
  if (plugins == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin catalog required");
  for (i = 0U; rc == CAI_OK && i < plugins->directory_count; i++) {
    directory = opendir(plugins->directories[i]);
    if (directory == NULL) {
      if (errno == ENOENT)
        continue;
      return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "cannot read plugin root",
                                  plugins->directories[i]);
    }
    while (rc == CAI_OK && (item = readdir(directory)) != NULL) {
      if (item->d_name[0] == '.')
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", plugins->directories[i],
                   item->d_name) >= (int)sizeof(path) ||
          snprintf(manifest, sizeof(manifest), "%s/tool.json", path) >=
              (int)sizeof(manifest)) {
        rc = cai_set_error(error, CAI_ERR_INVALID, "plugin path too long");
        break;
      }
      if (stat(manifest, &info) == 0 && S_ISREG(info.st_mode))
        rc = cai_plugins_load(plugins, path, error);
    }
    closedir(directory);
  }
  return rc;
}
int cai_plugins_unload(cai_plugins *plugins, const char *name,
                       cai_error *error) {
  plugin_entry **link, *entry;
  if (plugins == NULL || name == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin name required");
  pthread_mutex_lock(&plugins->lock);
  for (link = &plugins->entries; *link != NULL; link = &(*link)->next)
    if (strcmp((*link)->definition.name, name) == 0)
      break;
  entry = *link;
  if (entry != NULL) {
    *link = entry->next;
    plugins->generation++;
  }
  pthread_mutex_unlock(&plugins->lock);
  if (entry == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin is not loaded");
  entry_release(plugins, entry);
  return CAI_OK;
}
static int plugins_snapshot(cai_plugins *plugins, plugin_entry ***out,
                            size_t *count, unsigned long *generation,
                            cai_error *error) {
  plugin_entry *entry;
  size_t i = 0U, n = 0U;
  pthread_mutex_lock(&plugins->lock);
  for (entry = plugins->entries; entry != NULL; entry = entry->next)
    n++;
  *out = cai_alloc(NULL, (n + 1U) * sizeof(**out));
  if (*out != NULL)
    for (entry = plugins->entries; entry != NULL; entry = entry->next) {
      entry->references++;
      (*out)[i++] = entry;
    }
  *count = n;
  *generation = plugins->generation;
  pthread_mutex_unlock(&plugins->lock);
  return *out != NULL
             ? CAI_OK
             : cai_set_error(error, CAI_ERR_NOMEM, "snapshot plugin catalog");
}
int cai_plugins_visit(cai_plugins *plugins, cai_plugin_visitor visitor,
                      void *context, cai_error *error) {
  plugin_entry **entries;
  size_t i, count;
  unsigned long generation;
  int rc;
  if (plugins == NULL || visitor == NULL)
    return cai_set_error(error, CAI_ERR_INVALID, "plugin visitor required");
  plugins_retain(plugins);
  rc = plugins_snapshot(plugins, &entries, &count, &generation, error);
  if (rc == CAI_OK) {
    for (i = 0U; i < count; i++) {
      if (rc == CAI_OK)
        rc = visitor(context, &entries[i]->definition, error);
      entry_release(plugins, entries[i]);
    }
    cai_free_mem(NULL, entries);
  }
  cai_plugins_close(plugins);
  return rc;
}
int cai_plugins_run(cai_plugins *plugins, const char *name,
                    cai_source *arguments, cai_sink *output,
                    const cai_plugin_call *call, cai_error *error) {
  plugin_entry *entry;
  int rc;
  if (plugins == NULL || name == NULL || arguments == NULL || output == NULL ||
      call == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "plugin invocation is incomplete");
  plugins_retain(plugins);
  pthread_mutex_lock(&plugins->lock);
  entry = entry_find(plugins, name);
  if (entry != NULL)
    entry->references++;
  pthread_mutex_unlock(&plugins->lock);
  if (entry == NULL)
    rc = cai_set_error(error, CAI_ERR_INVALID, "plugin is not loaded");
  else {
    rc = entry->handler.invoke(entry->handler.context, arguments, output, call,
                               error);
    entry_release(plugins, entry);
  }
  cai_plugins_close(plugins);
  return rc;
}

static size_t proxy_read(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  lonejson_spooled *input = context;
  lonejson_read_result read = input->read(input, bytes, capacity);
  if (read.error_code != LONEJSON_STATUS_OK) {
    cai_set_error(error, CAI_ERR_TRANSPORT, "read plugin arguments");
    return 0U;
  }
  return read.bytes_read;
}
static int proxy_run(void *context, lonejson_spooled *input, cai_sink *output,
                     cai_error *error) {
  plugin_proxy *proxy = context;
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  lonejson_error local;
  cai_plugin_call call;
  int rc;
  lonejson_error_init(&local);
  if (input->rewind(input, &local) != LONEJSON_STATUS_OK)
    return cai_set_error(error, CAI_ERR_TRANSPORT, "rewind plugin arguments");
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = input;
  callbacks.read = proxy_read;
  rc = cai_source_from_callbacks(&callbacks, &source, error);
  if (rc == CAI_OK) {
    pthread_mutex_lock(&proxy->agent->plugins_lock);
    call = proxy->agent->plugin_call;
    pthread_mutex_unlock(&proxy->agent->plugins_lock);
    rc = proxy->entry->handler.invoke(proxy->entry->handler.context, source,
                                      output, &call, error);
  }
  cai_source_close(source);
  return rc;
}
static void proxy_close(void *context) {
  plugin_proxy *proxy = context;
  entry_release(proxy->agent->plugins, proxy->entry);
  cai_free_mem(NULL, proxy);
}
typedef struct plugin_control {
  char *path, *name;
} plugin_control;
static const lonejson_field control_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC(plugin_control, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC(plugin_control, name, "name")};
LONEJSON_MAP_DEFINE(control_map, plugin_control, control_fields);
static int plugin_control_execute(void *context, const char *arguments,
                                  cai_sink *output, int unload,
                                  cai_error *error) {
  cai_agent_impl *agent = context;
  plugin_control request;
  lonejson_error local;
  int rc;
  memset(&request, 0, sizeof(request));
  lonejson_error_init(&local);
  if (CAI_LJ->parse_cstr(CAI_LJ, &control_map, &request, arguments, &local) !=
      LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_INVALID,
                       "invalid plugin control arguments");
  else if ((unload && (request.name == NULL || request.path != NULL)) ||
           (!unload && request.name != NULL))
    rc = cai_set_error(error, CAI_ERR_INVALID, "invalid plugin control fields");
  else if (unload)
    rc = cai_plugins_unload(agent->plugins, request.name, error);
  else if (request.path != NULL)
    rc = cai_plugins_load(agent->plugins, request.path, error);
  else
    rc = cai_plugins_reload(agent->plugins, error);
  CAI_LJ->cleanup(CAI_LJ, &control_map, &request);
  if (rc == CAI_OK)
    rc = cai_sink_write(
        output, "Tool catalog updated for the next model request.",
        strlen("Tool catalog updated for the next model request."), error);
  return rc;
}
static int plugin_control_run(void *context, const char *arguments,
                              cai_sink *output, cai_error *error) {
  return plugin_control_execute(context, arguments, output, 0, error);
}
static int plugin_unload_run(void *context, const char *arguments,
                             cai_sink *output, cai_error *error) {
  return plugin_control_execute(context, arguments, output, 1, error);
}
static int plugins_control_description(cai_plugins *plugins, char **out,
                                       cai_error *error) {
  cai_buffer_builder text;
  plugin_executor *executor;
  size_t i;
  int rc;
  memset(&text, 0, sizeof(text));
  rc = cai_buffer_append_cstr(
      &text,
      "Load/reload actual tools, separate from skills. Create and test a "
      "package directory with tool.json: "
      "{\"name\":\"tool_name\",\"description\":\"Purpose\",\"parameters\":{"
      "\"type\":\"object\",\"properties\":{}},"
      "\"executor\":\"process\",\"argv\":[\"python3\"],\"entrypoint\":\"main."
      "py\"}. "
      "Process tools receive one JSON value on stdin and write UTF-8 results "
      "to stdout; stderr reports failure diagnostics. "
      "argv is literal (no shell expansion); use sh, python3, node, or a "
      "compiled Go executable as appropriate. ",
      error);
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(
        &text,
        "Lua executor: entrypoint main.lua returns function(input,ctx); "
        "input:read() streams JSON, "
        "input:decode() explicitly decodes bounded JSON, ctx.write(text) "
        "streams results, or return a JSON table. "
        "Restricted Lua offers safe libraries, package-local require, "
        "ctx.workspace, ctx.yolo, ctx.cancelled(), "
        "and ctx.call_tool(name,json_arguments[,on_chunk]) for enabled native "
        "workspace/command tools. "
        "Without on_chunk their result streams to output; on_chunk can "
        "transform streamed chunks. ",
        error);
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(
        &text,
        "Tool policies still apply. Call reload_tools with the package path "
        "after testing; omit path to reload roots. "
        "Changes publish on the next model request after current calls finish. "
        "Approved roots: ",
        error);
  for (i = 0U; rc == CAI_OK && i < plugins->directory_count; i++) {
    if (i != 0U)
      rc = cai_buffer_append_cstr(&text, ", ", error);
    if (rc == CAI_OK)
      rc = cai_buffer_append_json_string(&text, plugins->directories[i], error);
  }
  if (rc == CAI_OK)
    rc = cai_buffer_append_cstr(&text, ". Available executors: ", error);
  pthread_mutex_lock(&plugins->lock);
  for (executor = plugins->executors; rc == CAI_OK && executor != NULL;
       executor = executor->next) {
    rc = cai_buffer_append_json_string(&text, executor->name, error);
    if (rc == CAI_OK && executor->next != NULL)
      rc = cai_buffer_append_cstr(&text, ", ", error);
  }
  pthread_mutex_unlock(&plugins->lock);
  if (rc == CAI_OK)
    *out = text.data;
  else
    cai_free_mem(NULL, text.data);
  return rc;
}
static int plugins_sync_agent_update(cai_agent *agent, cai_error *error) {
  cai_agent_impl *impl = CAI_AGENT_IMPL(agent);
  cai_tool_registry *next = NULL;
  plugin_entry **entries = NULL;
  plugin_proxy *proxy;
  size_t i, count;
  unsigned long generation;
  int rc;
  char *description = NULL;
  if (impl->plugins == NULL)
    return CAI_OK;
  pthread_mutex_lock(&impl->plugins->lock);
  generation = impl->plugins->generation;
  pthread_mutex_unlock(&impl->plugins->lock);
  if (impl->plugins_bound && generation == impl->plugins_generation)
    return CAI_OK;
  rc = plugins_snapshot(impl->plugins, &entries, &count, &generation, error);
  if (rc == CAI_OK)
    rc = cai_tool_registry_new(&next, error);
  if (rc == CAI_OK)
    rc = plugins_control_description(impl->plugins, &description, error);
  if (rc == CAI_OK)
    rc = cai_tool_registry_register_raw(
        next, "reload_tools", description,
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"additionalProperties\":false}",
        0, plugin_control_run, impl, error);
  cai_free_mem(NULL, description);
  if (rc == CAI_OK)
    rc = cai_tool_registry_register_raw(
        next, "unload_tool",
        "Remove a plugin tool by name after current response calls finish.",
        "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
        "\"required\":[\"name\"],\"additionalProperties\":false}",
        0, plugin_unload_run, impl, error);
  for (i = 0U; rc == CAI_OK && i < count; i++) {
    proxy = cai_alloc(NULL, sizeof(*proxy));
    if (proxy == NULL) {
      rc = cai_set_error(error, CAI_ERR_NOMEM, "allocate plugin binding");
      break;
    }
    proxy->agent = impl;
    proxy->entry = entries[i];
    rc = cai_tool_registry_register_raw_spooled_owned(
        next, entries[i]->definition.name, entries[i]->definition.description,
        entries[i]->definition.parameters_json, entries[i]->definition.strict,
        proxy_run, proxy, proxy_close, error);
    if (rc != CAI_OK)
      cai_free_mem(NULL, proxy);
    else
      entries[i] = NULL;
  }
  pthread_mutex_lock(&impl->plugins_lock);
  if (rc == CAI_OK)
    rc = cai_tool_registry_replace_range_internal(
        impl->tools, impl->plugins_base,
        cai_tool_registry_count(impl->tools) - impl->plugins_base, next, error);
  if (rc == CAI_OK) {
    impl->plugins_generation = generation;
    impl->plugins_bound = 1;
  }
  pthread_mutex_unlock(&impl->plugins_lock);
  cai_tool_registry_destroy(next);
  if (entries != NULL) {
    for (i = 0U; i < count; i++)
      if (entries[i] != NULL)
        entry_release(impl->plugins, entries[i]);
    cai_free_mem(NULL, entries);
  }
  return rc;
}
int cai_plugins_sync_agent(cai_agent *agent, cai_error *error) {
  cai_agent_impl *impl = CAI_AGENT_IMPL(agent);
  if (impl->plugins == NULL)
    return CAI_OK;
  return plugins_sync_agent_update(agent, error);
}
int cai_agent_use_plugins(cai_agent *agent, cai_plugins *plugins,
                          const cai_plugin_call *call, cai_error *error) {
  cai_agent_impl *impl;
  size_t i, base;
  int rc = CAI_OK;
  char *workspace = NULL;
  cai_environment *environment;
  if (agent == NULL || agent->impl == NULL || (plugins != NULL && call == NULL))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent and plugin call configuration required");
  impl = CAI_AGENT_IMPL(agent);
  if (impl->plugins != NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "agent already has a plugin catalog");
  if (plugins == NULL)
    return CAI_OK;
  if (!impl->plugins_lock_initialized) {
    if (pthread_mutex_init(&impl->plugins_lock, NULL) != 0)
      return cai_set_error(error, CAI_ERR_TRANSPORT,
                           "initialize plugin publication lock");
    impl->plugins_lock_initialized = 1;
  }
  workspace = cai_strdup(NULL, call->workspace != NULL ? call->workspace : ".");
  if (workspace == NULL)
    return cai_set_error(error, CAI_ERR_NOMEM, "copy plugin workspace");
  environment = call->environment != NULL
                    ? call->environment
                    : cai_client_environment(impl->client);
  rc = cai_environment_retain(environment, error);
  if (rc != CAI_OK) {
    cai_free_mem(NULL, workspace);
    return rc;
  }
  base = cai_tool_registry_count(impl->tools);
  for (i = 0U; rc == CAI_OK && i < base; i++)
    rc = plugins_reserve(plugins, cai_tool_registry_name_at(impl->tools, i),
                         error);
  if (rc == CAI_OK) {
    plugins_retain(plugins);
    impl->plugins = plugins;
    impl->plugin_call = *call;
    impl->plugin_call.environment = environment;
    impl->plugin_workspace = workspace;
    impl->plugin_call.workspace = workspace;
    impl->plugins_base = base;
    rc = cai_plugins_sync_agent(agent, error);
    if (rc != CAI_OK) {
      impl->plugins = NULL;
      impl->plugin_workspace = NULL;
      impl->plugin_call.environment = NULL;
      cai_plugins_close(plugins);
    }
  }
  if (rc != CAI_OK) {
    cai_environment_close(environment);
    cai_free_mem(NULL, workspace);
  }
  return rc;
}
