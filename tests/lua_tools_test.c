#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "worker_stack.h"
#include <assert.h>
#include <cai/lua_tools.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct fixture_input {
  const char *text;
  size_t offset;
} fixture_input;
typedef struct fixture_output {
  char bytes[4096];
  size_t length;
} fixture_output;
static size_t fixture_read(void *context, void *bytes, size_t size,
                           cai_error *error) {
  fixture_input *input = context;
  size_t count = strlen(input->text) - input->offset;
  (void)error;
  if (count > size)
    count = size;
  memcpy(bytes, input->text + input->offset, count);
  input->offset += count;
  return count;
}
static int fixture_write(void *context, const void *bytes, size_t size,
                         cai_error *error) {
  fixture_output *output = context;
  (void)error;
  assert(size < sizeof(output->bytes) - output->length);
  memcpy(output->bytes + output->length, bytes, size);
  output->length += size;
  output->bytes[output->length] = '\0';
  return CAI_OK;
}
static int execute_optional_error(cai_plugins *plugins, const char *name,
                                  const char *input,
                                  const cai_plugin_call *call,
                                  const char *expected, int omit_error) {
  fixture_input reader;
  fixture_output writer;
  cai_source_callbacks read;
  cai_sink_callbacks write;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_error error;
  int rc;
  memset(&reader, 0, sizeof(reader));
  reader.text = input;
  memset(&writer, 0, sizeof(writer));
  memset(&read, 0, sizeof(read));
  memset(&write, 0, sizeof(write));
  read.read = fixture_read;
  read.context = &reader;
  write.write = fixture_write;
  write.context = &writer;
  cai_error_init(&error);
  assert(cai_source_from_callbacks(&read, &source, &error) == CAI_OK &&
         cai_sink_from_callbacks(&write, &sink, &error) == CAI_OK);
  rc = cai_plugins_run(plugins, name, source, sink, call,
                       omit_error ? NULL : &error);
  if (expected != NULL) {
    if (rc != CAI_OK)
      fprintf(stderr, "Lua invocation: %s\n",
              error.message != NULL ? error.message : "unknown error");
    assert(rc == CAI_OK && strcmp(writer.bytes, expected) == 0);
  }
  cai_source_close(source);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  return rc;
}
static int execute(cai_plugins *plugins, const char *name, const char *input,
                   const cai_plugin_call *call, const char *expected) {
  return execute_optional_error(plugins, name, input, call, expected, 0);
}
static void file_write(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  assert(file != NULL && fwrite(text, 1U, strlen(text), file) == strlen(text));
  assert(fclose(file) == 0);
}
typedef struct dispatcher {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  int pending, done, result;
  cai_lua_tools_operation operation;
  void *context;
  cai_error *error;
  cai_plugins *plugins;
  cai_plugin_call *call;
} dispatcher;
static int host_dispatch(void *context, cai_lua_tools_operation operation,
                         void *operation_context, cai_error *error) {
  dispatcher *host = context;
  int result;
  pthread_mutex_lock(&host->lock);
  host->operation = operation;
  host->context = operation_context;
  host->error = error;
  host->pending = 1;
  pthread_cond_signal(&host->changed);
  while (!host->done)
    pthread_cond_wait(&host->changed, &host->lock);
  result = host->result;
  pthread_mutex_unlock(&host->lock);
  return result;
}
static void *host_worker(void *context) {
  dispatcher *host = context;
  assert(execute_optional_error(host->plugins, "host_function", "{}",
                                host->call, "HOST", 1) == CAI_OK);
  return NULL;
}
static void trusted_host(cai_plugin_call *call) {
  lua_State *state = luaL_newstate();
  cai_plugins *plugins = NULL;
  cai_plugins_config config;
  cai_lua_tools *adapter = NULL;
  cai_lua_tools_config lua_config;
  cai_plugin_definition definition;
  cai_error error;
  dispatcher host;
  pthread_t worker;
  assert(state != NULL);
  luaL_openlibs(state);
  memset(&host, 0, sizeof(host));
  assert(pthread_mutex_init(&host.lock, NULL) == 0 &&
         pthread_cond_init(&host.changed, NULL) == 0);
  cai_error_init(&error);
  cai_plugins_config_init(&config);
  cai_lua_tools_config_init(&lua_config);
  lua_config.state = state;
  lua_config.dispatch = host_dispatch;
  lua_config.dispatch_context = &host;
  assert(cai_plugins_open(&config, &plugins, &error) == CAI_OK &&
         cai_lua_tools_open(&lua_config, &adapter, &error) == CAI_OK);
  memset(&definition, 0, sizeof(definition));
  definition.name = "host_function";
  definition.description = "trusted host handler";
  definition.parameters_json = "{\"type\":\"object\"}";
  assert(luaL_dostring(
             state, "return function(input, context) saved_input=input; "
                    "saved_context=context; assert(input:decode()); "
                    "assert(input:read()==nil); return 'HOST' end") == LUA_OK);
  assert(cai_lua_tools_define(adapter, plugins, &definition, -1, &error) ==
         CAI_OK);
  lua_pop(state, 1);
  host.plugins = plugins;
  host.call = call;
  assert(pthread_create(&worker, NULL, host_worker, &host) == 0);
  pthread_mutex_lock(&host.lock);
  while (!host.pending)
    pthread_cond_wait(&host.changed, &host.lock);
  pthread_mutex_unlock(&host.lock);
  host.result = host.operation(host.context, host.error);
  pthread_mutex_lock(&host.lock);
  host.done = 1;
  pthread_cond_signal(&host.changed);
  pthread_mutex_unlock(&host.lock);
  assert(pthread_join(worker, NULL) == 0);
  assert(luaL_dostring(
             state,
             "local v,e=saved_input:read(); assert(v==nil and e=='tool "
             "invocation has ended'); local v,e=saved_context.write('late'); "
             "assert(v==nil and e=='tool invocation has ended')") == LUA_OK);
  cai_plugins_close(plugins);
  cai_lua_tools_close(adapter);
  /* Closing an adapter must not close or corrupt the caller's VM. */
  assert(luaL_dostring(state, "assert(2+2==4)") == LUA_OK);
  lua_close(state);
  pthread_cond_destroy(&host.changed);
  pthread_mutex_destroy(&host.lock);
  cai_error_cleanup(&error);
}
static int capability(void *context, const char *name, const char *arguments,
                      cai_sink *output, cai_error *error) {
  (void)context;
  (void)arguments;
  if (strcmp(name, "read_file") != 0)
    return CAI_ERR_INVALID;
  return cai_sink_write(output, "capability", 10U, error);
}
static void test_files(const char *base) {
  char root[4096], package[4096], manifest[4096], source[4096], helper[4096];
  const char *directories[1];
  cai_plugins *plugins = NULL;
  cai_plugins_config config;
  cai_lua_tools *adapter = NULL;
  cai_lua_tools_config lua_config;
  cai_plugin_call call;
  cai_error error;
  assert(snprintf(root, sizeof(root), "%s/lua-tools-XXXXXX", base) <
             (int)sizeof(root) &&
         mkdtemp(root) != NULL);
  assert(snprintf(package, sizeof(package), "%s/package", root) <
             (int)sizeof(package) &&
         mkdir(package, 0700) == 0);
  assert(snprintf(manifest, sizeof(manifest), "%s/tool.json", package) <
         (int)sizeof(manifest));
  assert(snprintf(source, sizeof(source), "%s/main.lua", package) <
         (int)sizeof(source));
  assert(snprintf(helper, sizeof(helper), "%s/helper.lua", package) <
         (int)sizeof(helper));
  file_write(manifest, "{\"name\":\"lua_example\",\"description\":\"Lua "
                       "example\",\"executor\":\"lua\",\"entrypoint\":\"main."
                       "lua\",\"parameters\":{\"type\":\"object\"}}");
  file_write(helper, "return {value=42}");
  file_write(source, "return function(input,ctx) assert(not io and not os and "
                     "not debug and not package and not load and not loadfile "
                     "and not dofile); local args=assert(input:decode()); "
                     "assert(args.empty[1]==nil and args.nul==ctx.null); local "
                     "h=require('helper'); return {answer=h.value+args.n} end");
  cai_error_init(&error);
  cai_plugins_config_init(&config);
  cai_plugin_call_init(&call);
  cai_lua_tools_config_init(&lua_config);
  directories[0] = root;
  config.directories = directories;
  config.directory_count = 1U;
  call.workspace = root;
  assert(cai_plugins_open(&config, &plugins, &error) == CAI_OK &&
         cai_lua_tools_open(&lua_config, &adapter, &error) == CAI_OK &&
         cai_lua_tools_attach(adapter, plugins, &error) == CAI_OK);
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  execute(plugins, "lua_example", "{\"n\":1,\"empty\":[],\"nul\":null}", &call,
          "{\"answer\":43}");
  /* Error details are optional, including inside decode/read and host dispatch.
   */
  execute_optional_error(plugins, "lua_example",
                         "{\"n\":2,\"empty\":[],\"nul\":null}", &call,
                         "{\"answer\":44}", 1);
  assert(execute_optional_error(plugins, "lua_example", "{", &call, NULL, 1) ==
         CAI_ERR_INVALID);
  /* Exercise the accepted depth and its rejection boundary on the worker. */
  file_write(
      source,
      "return function(input,ctx) local a=assert(input:decode()); "
      "if a.mixed then return {1,{flag=true},ctx.null,a.empty,{},"
      "{{nested={false,2.5,'x'}}}} end; "
      "if a.cycle then local t={} t.value=t; return t end; "
      "if a.badkey then return {[true]=1} end; "
      "local t=ctx.null; for i=1,a.depth do t={value=t} end; return t end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  {
    char *expected = calloc(1U, 1024U);
    size_t i;
    assert(expected != NULL);
    for (i = 0U; i < 48U; i++)
      strcat(expected, "{\"value\":");
    strcat(expected, "null");
    for (i = 0U; i < 48U; i++)
      strcat(expected, "}");
    execute(plugins, "lua_example", "{\"depth\":48}", &call, expected);
    free(expected);
  }
  assert(execute(plugins, "lua_example", "{\"depth\":49}", &call, NULL) ==
         CAI_ERR_INVALID);
  execute(plugins, "lua_example", "{\"mixed\":true,\"empty\":[]}", &call,
          "[1,{\"flag\":true},null,[],{},[{\"nested\":[false,2.5,\"x\"]}]]");
  assert(execute(plugins, "lua_example", "{\"cycle\":true}", &call, NULL) ==
         CAI_ERR_INVALID);
  assert(execute(plugins, "lua_example", "{\"badkey\":true}", &call, NULL) ==
         CAI_ERR_INVALID);
  /* Dependencies must reject excessive parser/C-call depth on this worker too.
   */
  {
    char *syntax = calloc(1U, 2048U);
    size_t i, offset;
    assert(syntax != NULL);
    strcpy(syntax, "return function() return ");
    offset = strlen(syntax);
    for (i = 0U; i < 512U; i++)
      syntax[offset++] = '(';
    syntax[offset++] = '0';
    for (i = 0U; i < 512U; i++)
      syntax[offset++] = ')';
    strcpy(syntax + offset, " end");
    file_write(source, syntax);
    free(syntax);
    assert(cai_plugins_load(plugins, package, &error) == CAI_ERR_INVALID);
    cai_error_cleanup(&error);
    cai_error_init(&error);
  }
  /* Restore the original handler for the following reload-failure check. */
  file_write(source, "return function(input,ctx) return {answer=44} end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  /* Syntax failure preserves the loaded source; file edits alone do not reload.
   */
  file_write(source, "this is not Lua !");
  assert(cai_plugins_load(plugins, package, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  cai_error_init(&error);
  execute(plugins, "lua_example", "{\"n\":2,\"empty\":[],\"nul\":null}", &call,
          "{\"answer\":44}");
  file_write(source,
             "return function(input,ctx) if ctx.yolo then assert(io and os and "
             "debug and package); return 'YOLO' else assert(not io and not os "
             "and not debug and not package); return 'RESTRICTED' end end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  call.yolo = 1;
  execute(plugins, "lua_example", "{}", &call, "YOLO");
  call.yolo = 0;
  execute(plugins, "lua_example", "{}", &call, "RESTRICTED");
  file_write(source,
             "return function(input,ctx) local text=assert(input:read(2)); "
             "ctx.write(text); ctx.write(assert(input:read())); "
             "assert(input:read()==nil) end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  execute_optional_error(plugins, "lua_example", "read input", &call,
                         "read input", 1);
  call.call_tool = capability;
  file_write(
      source,
      "return function(input,ctx) "
      "assert(ctx.call_tool('read_file','{}',function(s) ctx.write(s:upper()) "
      "end)); local ok=ctx.call_tool('missing','{}'); assert(not ok) end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  execute(plugins, "lua_example", "{}", &call, "CAPABILITY");
  call.max_output_bytes = 2U;
  assert(execute(plugins, "lua_example", "{}", &call, NULL) == CAI_ERR_LIMIT);
  assert(execute_optional_error(plugins, "lua_example", "{}", &call, NULL, 1) ==
         CAI_ERR_LIMIT);
  call.max_output_bytes = 0U;
  file_write(helper, "return require('helper')");
  file_write(source, "return function() return require('helper') end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  assert(execute(plugins, "lua_example", "{}", &call, NULL) != CAI_OK);
  assert(unlink(helper) == 0 && symlink("/etc/passwd", helper) == 0);
  assert(execute(plugins, "lua_example", "{}", &call, NULL) != CAI_OK);
  file_write(source, "return function(input,ctx) while true do end end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  call.timeout_ms = 1L;
  assert(execute(plugins, "lua_example", "{}", &call, NULL) == CAI_ERR_LIMIT);
  call.timeout_ms = 120000L;
  file_write(source, "return function(input,ctx) local t={} while true do "
                     "t[#t+1]=string.rep('x',1048576) end end");
  assert(cai_plugins_load(plugins, package, &error) == CAI_OK);
  assert(execute(plugins, "lua_example", "{}", &call, NULL) != CAI_OK);
  cai_plugins_close(plugins);
  cai_lua_tools_close(adapter);
  cai_error_cleanup(&error);
  trusted_host(&call);
  assert(unlink(source) == 0 && unlink(helper) == 0 && unlink(manifest) == 0 &&
         rmdir(package) == 0 && rmdir(root) == 0);
}
static void *test_worker(void *context) {
  test_files(context);
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t worker;
  assert(argc == 2);
  assert(test_worker_start(&worker, test_worker, argv[1]) == 0);
  assert(pthread_join(worker, NULL) == 0);
  return 0;
}
