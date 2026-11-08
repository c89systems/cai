#define _POSIX_C_SOURCE 200809L
#include "worker_stack.h"
#include <assert.h>
#include <cai/cai.h>
#include <cai/tools/exec.h>
#include <cai/tools/read.h>
#include <lonejson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct invocation {
  const char *mode, *root, *sandbox;
} invocation;

static int result_value(void *context, const void *params, void *result,
                        cai_error *error) {
  int *calls = context;
  (void)params;
  (void)error;
  *(char **)result = strdup("leaf");
  (*calls)++;
  return *(char **)result != NULL ? CAI_OK : CAI_ERR_NOMEM;
}
static int count_output(void *context, const void *bytes, size_t length,
                        cai_error *error) {
  size_t *count = context;
  (void)bytes;
  (void)error;
  *count += length;
  return CAI_OK;
}

static void schema(void) {
  lonejson_map *maps;
  lonejson_map *root;
  lonejson_field *fields;
  cai_tool_schema *result = NULL;
  cai_tool_registry *registry;
  cai_sink *sink;
  cai_sink_callbacks callbacks;
  lonejson_map empty;
  cai_error error;
  size_t i, bytes = 0U;
  int calls = 0, rc;
  cai_error_init(&error);
  maps = calloc(129U, sizeof(*maps));
  fields = calloc(129U, sizeof(*fields));
  assert(maps != NULL && fields != NULL);
  for (i = 0U; i < 129U; i++) {
    maps[i].name = "nested";
    maps[i].struct_size = sizeof(char *);
    maps[i].field_count = 1U;
    maps[i].fields = &fields[i];
    fields[i].json_key = "value";
    fields[i].json_key_len = 5U;
    fields[i].json_key_first = 'v';
    fields[i].json_key_last = 'e';
    fields[i].flags = LONEJSON_FIELD_REQUIRED;
    fields[i].storage =
        i == 128U ? LONEJSON_STORAGE_DYNAMIC : LONEJSON_STORAGE_FIXED;
    fields[i].kind =
        i == 128U ? LONEJSON_FIELD_KIND_STRING : LONEJSON_FIELD_KIND_OBJECT;
    fields[i].submap = i == 128U ? NULL : &maps[i + 1U];
  }
  root = &maps[120]; /* A valid schema within the parser workspace budget. */
  rc = cai_tool_schema_from_map(root, &result, &error);
  if (rc != CAI_OK)
    fprintf(stderr, "schema: %s; %s\n",
            error.message != NULL ? error.message : "failed",
            error.detail != NULL ? error.detail : "");
  assert(rc == CAI_OK);
  assert(strstr(cai_tool_schema_json(result), "string") != NULL);
  cai_tool_schema_destroy(result);
  result = NULL;
  memset(&empty, 0, sizeof(empty));
  empty.name = "empty";
  empty.struct_size = 1U;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = count_output;
  callbacks.context = &bytes;
  assert(cai_sink_from_callbacks(&callbacks, &sink, NULL) == CAI_OK);
  assert(cai_tool_registry_new(&registry, NULL) == CAI_OK);
  assert(cai_tool_registry_register_lonejson(
             registry, "nested", "nested result", &empty, root, result_value,
             &calls, NULL) == CAI_OK);
  assert(cai_tool_registry_run(registry, "nested", "{}", sink, NULL) == CAI_OK);
  assert(calls == 1 && bytes > 60U);
  fields[128].kind = LONEJSON_FIELD_KIND_OBJECT;
  fields[128].storage = LONEJSON_STORAGE_FIXED;
  fields[128].submap = root;
  assert(cai_tool_registry_run(registry, "nested", "{}", sink, NULL) ==
         CAI_ERR_INVALID);
  assert(calls == 1);
  assert(cai_tool_schema_from_map(root, &result, NULL) == CAI_ERR_INVALID);
  cai_tool_registry_destroy(registry);
  cai_sink_close(sink);
  assert(result == NULL);
  fields[128].submap = NULL;
  assert(cai_tool_schema_from_map(root, &result, NULL) == CAI_ERR_INVALID);
  fields[128].kind = LONEJSON_FIELD_KIND_STRING;
  fields[128].storage = LONEJSON_STORAGE_DYNAMIC;
  /* Excessive JSON nesting fails cleanly after walking the maps on the heap. */
  assert(cai_tool_schema_from_map(maps, &result, NULL) != CAI_OK);
  assert(result == NULL);
  free(fields);
  free(maps);
  cai_error_cleanup(&error);
}

static void *run(void *context) {
  invocation *args = context;
  cai_tool_registry *registry;
  cai_sink *sink;
  cai_read_tool_config read_config;
  cai_exec_tool_config exec_config;
  cai_error error;
  int rc;
  if (strcmp(args->mode, "schema") == 0) {
    schema();
    return NULL;
  }
  cai_error_init(&error);
  assert(cai_tool_registry_new(&registry, &error) == CAI_OK);
  assert(cai_sink_file(stdout, 0, &sink, &error) == CAI_OK);
  if (strcmp(args->mode, "list") == 0) {
    memset(&read_config, 0, sizeof(read_config));
    read_config.root_path = args->root;
    assert(cai_tool_registry_register_list_files_tool(registry, &read_config,
                                                      &error) == CAI_OK);
    rc = cai_tool_registry_run(
        registry, "list_files",
        "{\"path\":\".\",\"recursive\":true,\"max_entries\":2000}", sink,
        &error);
  } else {
    memset(&exec_config, 0, sizeof(exec_config));
    exec_config.root_path = args->root;
    exec_config.bwrap_path = args->sandbox;
    exec_config.yolo = strcmp(args->mode, "plain") == 0;
    assert(cai_tool_registry_register_exec_tool(registry, &exec_config,
                                                &error) == CAI_OK);
    rc = cai_tool_registry_run(registry, "exec_command",
                               "{\"cmd\":\"printf WORKER_EXEC_OK\"}", sink,
                               &error);
  }
  if (rc != CAI_OK)
    fprintf(stderr, "worker: %s\n",
            error.message != NULL ? error.message : "failed");
  assert(rc == CAI_OK);
  cai_sink_close(sink);
  cai_tool_registry_destroy(registry);
  cai_error_cleanup(&error);
  return NULL;
}
int main(int argc, char **argv) {
  invocation args;
  pthread_t worker;
  assert(argc == 4);
  args.mode = argv[1];
  args.root = argv[2];
  args.sandbox = argv[3];
  assert(test_worker_start(&worker, run, &args) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
