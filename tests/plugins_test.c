#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <cai/plugins.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct capture {
  char bytes[8192];
  size_t length;
  int cancel;
} capture;
typedef struct input_value {
  const char *text;
  size_t offset;
} input_value;
static size_t input_read(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  input_value *input = context;
  size_t left = strlen(input->text) - input->offset;
  (void)error;
  if (left > capacity)
    left = capacity;
  memcpy(bytes, input->text + input->offset, left);
  input->offset += left;
  return left;
}
static int input_open(input_value *input, const char *text, cai_source **source,
                      cai_error *error) {
  cai_source_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  input->text = text;
  input->offset = 0U;
  callbacks.context = input;
  callbacks.read = input_read;
  return cai_source_from_callbacks(&callbacks, source, error);
}
static int capture_write(void *context, const void *bytes, size_t count,
                         cai_error *error) {
  capture *out = context;
  (void)error;
  assert(count < sizeof(out->bytes) - out->length);
  memcpy(out->bytes + out->length, bytes, count);
  out->length += count;
  out->bytes[out->length] = '\0';
  return CAI_OK;
}
static int is_cancelled(void *context) { return *(int *)context; }
static void run(cai_plugins *plugins, const char *name, const char *input,
                const cai_plugin_call *call, const char *expected) {
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_error error;
  capture out;
  input_value value;
  memset(&out, 0, sizeof(out));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = capture_write;
  callbacks.context = &out;
  cai_error_init(&error);
  assert(input_open(&value, input, &source, &error) == CAI_OK);
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  if (cai_plugins_run(plugins, name, source, sink, call, &error) != CAI_OK) {
    fprintf(stderr, "%s: %s: %s\n", name,
            error.message != NULL ? error.message : "",
            error.detail != NULL ? error.detail : "");
    abort();
  }
  assert(strcmp(out.bytes, expected) == 0);
  cai_source_close(source);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
}
static void file_write(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  assert(file != NULL && fwrite(text, 1U, strlen(text), file) == strlen(text));
  assert(fclose(file) == 0);
}
static int say(void *context, cai_source *input, cai_sink *output,
               const cai_plugin_call *call, cai_error *error) {
  const char *text = context;
  (void)input;
  (void)call;
  return cai_sink_write(output, text, strlen(text), error);
}
typedef struct blocked {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int entered, release, destroyed;
} blocked;
static int wait_handler(void *context, cai_source *input, cai_sink *output,
                        const cai_plugin_call *call, cai_error *error) {
  blocked *state = context;
  (void)input;
  (void)call;
  pthread_mutex_lock(&state->mutex);
  state->entered = 1;
  pthread_cond_signal(&state->changed);
  while (!state->release)
    pthread_cond_wait(&state->changed, &state->mutex);
  pthread_mutex_unlock(&state->mutex);
  return cai_sink_write(output, "old", 3U, error);
}
static void retired(void *context) { ((blocked *)context)->destroyed++; }
typedef struct job {
  cai_plugins *plugins;
  cai_plugin_call *call;
} job;
static void *old_call(void *context) {
  job *request = context;
  run(request->plugins, "replace_me", "{}", request->call, "old");
  return NULL;
}
int main(int argc, char **argv) {
  cai_plugins *plugins = NULL;
  cai_plugins_config config;
  cai_plugin_call call;
  cai_plugin_definition definition;
  cai_plugin_handler handler;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  cai_error error;
  char root[4096], package[4096], manifest[4096], path[4096];
  const char *roots[1];
  capture out;
  input_value value;
  blocked old;
  job request;
  pthread_t thread;
  int cancelled = 0;
  char new_text[] = "new";
  assert(argc == 2);
  assert(snprintf(root, sizeof(root), "%s/plugins-XXXXXX", argv[1]) <
         (int)sizeof(root));
  assert(mkdtemp(root) != NULL);
  assert(snprintf(package, sizeof(package), "%s/echo", root) <
         (int)sizeof(package));
  assert(mkdir(package, 0700) == 0);
  assert(snprintf(manifest, sizeof(manifest), "%s/tool.json", package) <
         (int)sizeof(manifest));
  file_write(manifest, "{\"name\":\"echo\",\"description\":\"Echo "
                       "input\",\"executor\":\"process\",\"parameters\":{"
                       "\"type\":\"object\"},\"argv\":[\"/bin/cat\"]}");
  cai_error_init(&error);
  cai_plugins_config_init(&config);
  cai_plugin_call_init(&call);
  roots[0] = root;
  config.directories = roots;
  config.directory_count = 1U;
  call.workspace = root;
  call.cancelled = is_cancelled;
  call.cancel_context = &cancelled;
  assert(cai_plugins_open(&config, &plugins, &error) == CAI_OK);
  assert(cai_plugins_reload(plugins, &error) == CAI_OK);
  run(plugins, "echo", "{\"hello\":\"world\"}", &call, "{\"hello\":\"world\"}");
  /* A failed candidate cannot erase a working tool. */
  file_write(
      manifest,
      "{\"name\":\"echo\",\"description\":\"bad\",\"executor\":\"process\","
      "\"parameters\":{\"type\":\"object\"},\"entrypoint\":\"missing\"}");
  assert(cai_plugins_load(plugins, package, &error) != CAI_OK);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  run(plugins, "echo", "{}", &call, "{}");
  assert(cai_plugins_load(plugins, argv[1], &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  /* The callback remains pinned while another thread replaces its definition.
   */
  memset(&definition, 0, sizeof(definition));
  memset(&handler, 0, sizeof(handler));
  memset(&old, 0, sizeof(old));
  assert(pthread_mutex_init(&old.mutex, NULL) == 0 &&
         pthread_cond_init(&old.changed, NULL) == 0);
  definition.name = "replace_me";
  definition.description = "replacement test";
  definition.parameters_json = "{\"type\":\"object\"}";
  handler.context = &old;
  handler.invoke = wait_handler;
  handler.destroy = retired;
  assert(cai_plugins_define(plugins, &definition, &handler, &error) == CAI_OK);
  request.plugins = plugins;
  request.call = &call;
  assert(pthread_create(&thread, NULL, old_call, &request) == 0);
  pthread_mutex_lock(&old.mutex);
  while (!old.entered)
    pthread_cond_wait(&old.changed, &old.mutex);
  pthread_mutex_unlock(&old.mutex);
  handler.context = new_text;
  handler.invoke = say;
  handler.destroy = NULL;
  assert(cai_plugins_define(plugins, &definition, &handler, &error) == CAI_OK &&
         old.destroyed == 0);
  run(plugins, "replace_me", "{}", &call, "new");
  pthread_mutex_lock(&old.mutex);
  old.release = 1;
  pthread_cond_signal(&old.changed);
  pthread_mutex_unlock(&old.mutex);
  assert(pthread_join(thread, NULL) == 0 && old.destroyed == 1);
  pthread_cond_destroy(&old.changed);
  pthread_mutex_destroy(&old.mutex);
  definition.name = "set_reasoning";
  assert(cai_plugins_define(plugins, &definition, &handler, &error) ==
         CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  file_write(manifest, "{\"name\":\"echo\",\"description\":\"bad "
                       "executor\",\"executor\":\"unavailable\",\"parameters\":"
                       "{\"type\":\"object\"}}");
  assert(cai_plugins_load(plugins, package, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(snprintf(path, sizeof(path), "%s/outside", package) <
         (int)sizeof(path));
  assert(symlink("/bin/sh", path) == 0);
  file_write(manifest, "{\"name\":\"echo\",\"description\":\"bad "
                       "entrypoint\",\"executor\":\"process\",\"entrypoint\":"
                       "\"outside\",\"parameters\":{\"type\":\"object\"}}");
  assert(cai_plugins_load(plugins, package, &error) == CAI_ERR_INVALID);
  assert(unlink(path) == 0);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  run(plugins, "echo", "{}", &call, "{}");
  /* Pipes reject limits/cancellation without SIGPIPE or changing host signals.
   */
  memset(&out, 0, sizeof(out));
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = capture_write;
  callbacks.context = &out;
  assert(cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK);
  assert(input_open(&value, "{\"x\":1}", &source, &error) == CAI_OK);
  call.max_output_bytes = 2U;
  assert(cai_plugins_run(plugins, "echo", source, sink, &call, &error) ==
         CAI_ERR_LIMIT);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  cancelled = 1;
  assert(cai_plugins_run(plugins, "echo", source, sink, &call, &error) ==
         CAI_ERR_CANCELLED);
  cai_source_close(source);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  assert(cai_plugins_unload(plugins, "replace_me", &error) == CAI_OK);
  cai_plugins_close(plugins);
  cai_error_cleanup(&error);
  assert(unlink(manifest) == 0 && rmdir(package) == 0);
  snprintf(path, sizeof(path), "%s", root);
  assert(rmdir(path) == 0);
  return 0;
}
