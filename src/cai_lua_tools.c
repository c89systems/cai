#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <cai/lua_tools.h>
#include <lauxlib.h>
#include <lonejson.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INPUT_MT "cai.lua_tools.input"
#define ARRAY_MT "cai.lua_tools.array"
#define SOURCE_LIMIT (1024U * 1024U)
#define MEMORY_LIMIT (32U * 1024U * 1024U)
typedef struct lua_handler lua_handler;
typedef struct lua_job lua_job;
typedef struct lua_decode lua_decode;
struct cai_lua_tools {
  cai_lua_tools_config config;
  pthread_t owner;
  pthread_mutex_t lock;
  lua_handler *retired;
};
struct lua_handler {
  lua_handler *next;
  cai_lua_tools *tools;
  char *source, *directory;
  size_t length;
  int reference;
};
typedef struct lua_borrow {
  lua_job *job;
} lua_borrow;
typedef struct decode_frame {
  int table, array;
  lua_Integer next;
} decode_frame;
struct lua_decode {
  lua_decode *next;
  lua_job *job;
  lua_State *state;
  decode_frame frames[64];
  size_t depth, bytes;
  char *token;
  size_t length, capacity;
};
struct lua_job {
  lua_handler *handler;
  cai_source *input;
  cai_sink *output;
  const cai_plugin_call *call;
  cai_error *error;
  lonejson *json;
  lua_State *state;
  lua_decode *decoders;
  lua_borrow *borrow;
  size_t written, allocated;
  int code, borrow_reference;
  double deadline;
};
static char job_key, null_key;

static int tools_error(cai_error *error, int code, const char *message) {
  if (error != NULL) {
    char *copy = strdup(message != NULL ? message : "Lua tool failed");
    cai_error_cleanup(error);
    error->code = code;
    error->message = copy;
  }
  return code;
}
static double tools_time(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1.0;
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}
static void *tools_alloc(void *context, void *pointer, size_t old,
                         size_t size) {
  lua_job *job = context;
  void *next;
  if (pointer == NULL)
    old = 0U;
  if (size == 0U) {
    free(pointer);
    job->allocated -= old;
    return NULL;
  }
  if (!job->call->yolo &&
      (size > MEMORY_LIMIT || job->allocated - old > MEMORY_LIMIT - size))
    return NULL;
  next = realloc(pointer, size);
  if (next != NULL)
    job->allocated = job->allocated - old + size;
  return next;
}
static int job_cancelled(lua_job *job) {
  if (job->call->cancelled != NULL &&
      job->call->cancelled(job->call->cancel_context)) {
    job->code =
        tools_error(job->error, CAI_ERR_CANCELLED, "Lua tool cancelled");
    return 1;
  }
  if (tools_time() < 0.0 || tools_time() >= job->deadline) {
    job->code =
        tools_error(job->error, CAI_ERR_LIMIT, "Lua tool deadline exceeded");
    return 1;
  }
  return 0;
}
static void tool_hook(lua_State *state, lua_Debug *debug) {
  lua_job *job;
  (void)debug;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &job_key);
  job = lua_touserdata(state, -1);
  lua_pop(state, 1);
  if (job != NULL && job_cancelled(job))
    luaL_error(state, "%s", "Lua tool cancelled or deadline exceeded");
}
static lua_job *borrow_job(lua_State *state, int index, int upvalue) {
  lua_borrow *borrow = upvalue ? lua_touserdata(state, lua_upvalueindex(index))
                               : luaL_checkudata(state, index, INPUT_MT);
  return borrow != NULL ? borrow->job : NULL;
}
static int dead_borrow(lua_State *state) {
  lua_pushnil(state);
  lua_pushliteral(state, "tool invocation has ended");
  return 2;
}
static int job_write(lua_job *job, const void *bytes, size_t size) {
  size_t limit = job->call->max_output_bytes != 0U ? job->call->max_output_bytes
                                                   : 4U * 1024U * 1024U;
  if (job->code != CAI_OK)
    return job->code;
  if (size > limit - job->written)
    return job->code = tools_error(job->error, CAI_ERR_LIMIT,
                                   "Lua tool output exceeds configured limit");
  job->code = cai_sink_write(job->output, bytes, size, job->error);
  if (job->code == CAI_OK)
    job->written += size;
  return job->code;
}
static lonejson_status json_write(void *context, const void *bytes, size_t size,
                                  lonejson_error *error) {
  (void)error;
  return job_write(context, bytes, size) == CAI_OK
             ? LONEJSON_STATUS_OK
             : LONEJSON_STATUS_CALLBACK_FAILED;
}
static int context_write(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 1);
  size_t size;
  const char *text;
  if (job == NULL)
    return dead_borrow(state);
  text = luaL_checklstring(state, 1, &size);
  if (job_write(job, text, size) != CAI_OK) {
    lua_pushnil(state);
    lua_pushstring(state, job->error->message);
    return 2;
  }
  lua_pushboolean(state, 1);
  return 1;
}
static int context_cancelled(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 1);
  if (job == NULL)
    return dead_borrow(state);
  lua_pushboolean(state, job_cancelled(job));
  return 1;
}
static int input_read(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 0);
  unsigned char bytes[4096];
  lua_Integer requested = luaL_optinteger(state, 2, 4096);
  size_t count;
  if (job == NULL)
    return dead_borrow(state);
  if (requested < 1 || requested > 4096) {
    lua_pushnil(state);
    lua_pushliteral(state, "read size must be 1..4096");
    return 2;
  }
  count = cai_source_read(job->input, bytes, (size_t)requested, job->error);
  if (job->error->code != CAI_OK) {
    job->code = job->error->code;
    lua_pushnil(state);
    lua_pushstring(state, job->error->message);
    return 2;
  }
  if (count == 0U)
    lua_pushnil(state);
  else
    lua_pushlstring(state, (const char *)bytes, count);
  return 1;
}
static lonejson_read_result decode_read(void *context, unsigned char *bytes,
                                        size_t capacity) {
  lua_decode *decode = context;
  lonejson_read_result result = lonejson_default_read_result();
  size_t limit = decode->job->handler->tools->config.decode_limit;
  if (job_cancelled(decode->job)) {
    result.error_code = LONEJSON_STATUS_CALLBACK_FAILED;
    return result;
  }
  result.bytes_read =
      cai_source_read(decode->job->input, bytes, capacity, decode->job->error);
  if (result.bytes_read > limit - decode->bytes) {
    decode->job->code = tools_error(decode->job->error, CAI_ERR_LIMIT,
                                    "Lua input decode limit exceeded");
    result.bytes_read = 0U;
    result.error_code = LONEJSON_STATUS_CALLBACK_FAILED;
  } else if (decode->job->error->code != CAI_OK) {
    decode->job->code = decode->job->error->code;
    result.error_code = LONEJSON_STATUS_CALLBACK_FAILED;
  } else {
    decode->bytes += result.bytes_read;
    result.eof = result.bytes_read == 0U;
  }
  return result;
}
static lonejson_status token_begin(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  decode->length = 0U;
  return LONEJSON_STATUS_OK;
}
static lonejson_status token_chunk(void *context, const char *bytes,
                                   size_t size, lonejson_error *error) {
  lua_decode *decode = context;
  char *grown;
  size_t capacity;
  (void)error;
  if (size > decode->job->handler->tools->config.decode_limit - decode->length)
    return LONEJSON_STATUS_OVERFLOW;
  if (decode->length + size + 1U > decode->capacity) {
    capacity = decode->length + size + 1U;
    grown = realloc(decode->token, capacity);
    if (grown == NULL)
      return LONEJSON_STATUS_ALLOCATION_FAILED;
    decode->token = grown;
    decode->capacity = capacity;
  }
  memcpy(decode->token + decode->length, bytes, size);
  decode->length += size;
  decode->token[decode->length] = '\0';
  return LONEJSON_STATUS_OK;
}
static void decoded_value(lua_decode *decode) {
  decode_frame *frame;
  if (decode->depth == 0U)
    return;
  frame = &decode->frames[decode->depth - 1U];
  if (frame->array)
    lua_rawseti(decode->state, frame->table, frame->next++);
  else
    lua_rawset(decode->state, frame->table);
}
static lonejson_status key_end(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  lua_pushlstring(decode->state, decode->token != NULL ? decode->token : "",
                  decode->length);
  return LONEJSON_STATUS_OK;
}
static lonejson_status string_end(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  lua_pushlstring(decode->state, decode->token != NULL ? decode->token : "",
                  decode->length);
  decoded_value(decode);
  return LONEJSON_STATUS_OK;
}
static lonejson_status number_end(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  if (decode->token == NULL ||
      lua_stringtonumber(decode->state, decode->token) == 0U)
    return LONEJSON_STATUS_INVALID_JSON;
  decoded_value(decode);
  return LONEJSON_STATUS_OK;
}
static lonejson_status boolean_value(void *context, int value,
                                     lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  lua_pushboolean(decode->state, value);
  decoded_value(decode);
  return LONEJSON_STATUS_OK;
}
static lonejson_status null_value(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  lua_pushlightuserdata(decode->state, &null_key);
  decoded_value(decode);
  return LONEJSON_STATUS_OK;
}
static lonejson_status container_begin(lua_decode *decode, int array) {
  decode_frame *frame;
  if (decode->depth >= 64U || !lua_checkstack(decode->state, 8))
    return LONEJSON_STATUS_OVERFLOW;
  lua_newtable(decode->state);
  if (array) {
    luaL_getmetatable(decode->state, ARRAY_MT);
    lua_setmetatable(decode->state, -2);
  }
  frame = &decode->frames[decode->depth++];
  frame->table = lua_gettop(decode->state);
  frame->array = array;
  frame->next = 1;
  return LONEJSON_STATUS_OK;
}
static lonejson_status object_begin(void *context, lonejson_error *error) {
  (void)error;
  return container_begin(context, 0);
}
static lonejson_status array_begin(void *context, lonejson_error *error) {
  (void)error;
  return container_begin(context, 1);
}
static lonejson_status container_end(void *context, lonejson_error *error) {
  lua_decode *decode = context;
  (void)error;
  if (decode->depth == 0U)
    return LONEJSON_STATUS_INVALID_JSON;
  decode->depth--;
  decoded_value(decode);
  return LONEJSON_STATUS_OK;
}
static int input_decode(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 0);
  lua_decode *decode;
  lonejson_value_visitor visitor;
  lonejson_error error;
  lonejson_status status;
  int top = lua_gettop(state);
  if (job == NULL)
    return dead_borrow(state);
  decode = calloc(1U, sizeof(*decode));
  if (decode == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "allocate JSON decoder");
    return 2;
  }
  decode->job = job;
  decode->state = state;
  decode->next = job->decoders;
  job->decoders = decode;
  visitor = lonejson_default_value_visitor();
  visitor.object_begin = object_begin;
  visitor.array_begin = array_begin;
  visitor.object_end = container_end;
  visitor.array_end = container_end;
  visitor.object_key_begin = token_begin;
  visitor.object_key_chunk = token_chunk;
  visitor.object_key_end = key_end;
  visitor.string_begin = token_begin;
  visitor.string_chunk = token_chunk;
  visitor.string_end = string_end;
  visitor.number_begin = token_begin;
  visitor.number_chunk = token_chunk;
  visitor.number_end = number_end;
  visitor.boolean_value = boolean_value;
  visitor.null_value = null_value;
  lonejson_error_init(&error);
  status = job->json->visit_value_reader(job->json, decode_read, decode,
                                         &visitor, decode, &error);
  if (status != LONEJSON_STATUS_OK) {
    lua_settop(state, top);
    lua_pushnil(state);
    lua_pushstring(state,
                   job->code != CAI_OK ? job->error->message : error.message);
    return 2;
  }
  return 1;
}
static lonejson_status encode_value(lua_State *state, int index,
                                    lonejson_writer *writer, unsigned int depth,
                                    lonejson_error *error) {
  size_t length, count = 0U;
  lua_Integer i, n;
  const char *text;
  int array = 1, absolute = lua_absindex(state, index);
  lonejson_status status = LONEJSON_STATUS_OK;
  if (depth > 48U || !lua_checkstack(state, 8))
    return LONEJSON_STATUS_OVERFLOW;
  switch (lua_type(state, absolute)) {
  case LUA_TNIL:
    return writer->null_fn(writer, error);
  case LUA_TBOOLEAN:
    return writer->bool_fn(writer, lua_toboolean(state, absolute), error);
  case LUA_TNUMBER:
    return lua_isinteger(state, absolute)
               ? writer->i64(writer,
                             (lonejson_int64)lua_tointeger(state, absolute),
                             error)
               : writer->f64(writer, (double)lua_tonumber(state, absolute),
                             error);
  case LUA_TSTRING:
    text = lua_tolstring(state, absolute, &length);
    return writer->string(writer, text, length, error);
  case LUA_TLIGHTUSERDATA:
    if (lua_touserdata(state, absolute) == &null_key)
      return writer->null_fn(writer, error);
    return LONEJSON_STATUS_TYPE_MISMATCH;
  case LUA_TTABLE:
    break;
  default:
    return LONEJSON_STATUS_TYPE_MISMATCH;
  }
  n = (lua_Integer)lua_rawlen(state, absolute);
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    count++;
    if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
        lua_tointeger(state, -2) > n)
      array = 0;
    lua_pop(state, 1);
  }
  if (count == 0U) {
    array = 0;
    if (lua_getmetatable(state, absolute)) {
      luaL_getmetatable(state, ARRAY_MT);
      array = lua_rawequal(state, -1, -2);
      lua_pop(state, 2);
    }
  }
  if (array && count == (size_t)n) {
    status = writer->begin_array(writer, error);
    for (i = 1; status == LONEJSON_STATUS_OK && i <= n; i++) {
      lua_rawgeti(state, absolute, i);
      status = encode_value(state, -1, writer, depth + 1U, error);
      lua_pop(state, 1);
    }
    if (status == LONEJSON_STATUS_OK)
      status = writer->end_array(writer, error);
  } else {
    status = writer->begin_object(writer, error);
    lua_pushnil(state);
    while (status == LONEJSON_STATUS_OK && lua_next(state, absolute) != 0) {
      if (lua_type(state, -2) != LUA_TSTRING) {
        lua_pop(state, 2);
        return LONEJSON_STATUS_TYPE_MISMATCH;
      }
      text = lua_tolstring(state, -2, &length);
      status = writer->key(writer, text, length, error);
      if (status == LONEJSON_STATUS_OK)
        status = encode_value(state, -1, writer, depth + 1U, error);
      lua_pop(state, 1);
    }
    if (status != LONEJSON_STATUS_OK)
      lua_pop(state, 1);
    if (status == LONEJSON_STATUS_OK)
      status = writer->end_object(writer, error);
  }
  return status;
}
static int encode_result(lua_job *job, lua_State *state, int index) {
  lonejson_writer writer;
  lonejson_error error;
  lonejson_status status;
  lonejson_error_init(&error);
  status =
      job->json->writer_init_sink(job->json, &writer, json_write, job, &error);
  if (status == LONEJSON_STATUS_OK) {
    status = encode_value(state, index, &writer, 0U, &error);
    writer.cleanup(&writer);
  }
  if (status != LONEJSON_STATUS_OK && job->code == CAI_OK)
    job->code =
        tools_error(job->error, CAI_ERR_INVALID,
                    "Lua tool returned an unsupported or cyclic JSON value");
  return job->code;
}
typedef struct capability_output {
  lua_job *job;
  lua_State *state;
  int callback;
  size_t read;
} capability_output;
static int capability_write(void *context, const void *bytes, size_t size,
                            cai_error *error) {
  capability_output *output = context;
  lua_job *job = output->job;
  size_t limit = job->call->max_output_bytes != 0U ? job->call->max_output_bytes
                                                   : 4U * 1024U * 1024U;
  (void)error;
  if (!output->callback)
    return job_write(job, bytes, size);
  if (size > limit - output->read)
    return job->code =
               tools_error(job->error, CAI_ERR_LIMIT,
                           "Lua capability output exceeds configured limit");
  output->read += size;
  lua_pushvalue(output->state, output->callback);
  lua_pushlstring(output->state, bytes, size);
  if (lua_pcall(output->state, 1, 0, 0) != LUA_OK) {
    job->code = tools_error(job->error, CAI_ERR_INVALID,
                            lua_tostring(output->state, -1));
    lua_pop(output->state, 1);
    return job->code;
  }
  return CAI_OK;
}
static int context_call(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 1);
  const char *name, *arguments;
  int rc;
  cai_sink *sink = NULL;
  cai_sink_callbacks callbacks;
  capability_output target;
  if (job == NULL)
    return dead_borrow(state);
  name = luaL_checkstring(state, 1);
  arguments = luaL_checkstring(state, 2);
  memset(&target, 0, sizeof(target));
  target.job = job;
  target.state = state;
  if (!lua_isnoneornil(state, 3)) {
    luaL_checktype(state, 3, LUA_TFUNCTION);
    target.callback = 3;
  }
  if (job->call->call_tool == NULL) {
    lua_pushnil(state);
    lua_pushliteral(state, "host tool capabilities are unavailable");
    return 2;
  }
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = &target;
  callbacks.write = capability_write;
  rc = cai_sink_from_callbacks(&callbacks, &sink, job->error);
  if (rc == CAI_OK)
    rc = job->call->call_tool(job->call->tool_context, name, arguments, sink,
                              job->error);
  cai_sink_close(sink);
  if (rc != CAI_OK) {
    lua_pushnil(state);
    lua_pushstring(state, job->error != NULL && job->error->message != NULL
                              ? job->error->message
                              : "host tool failed");
    if (job->code == CAI_OK)
      cai_error_cleanup(job->error);
    return 2;
  }
  lua_pushboolean(state, 1);
  return 1;
}
static int context_print(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 1);
  int i, n = lua_gettop(state);
  size_t length;
  const char *text;
  if (job == NULL)
    return dead_borrow(state);
  for (i = 1; i <= n; i++) {
    text = luaL_tolstring(state, i, &length);
    if (i > 1 && job_write(job, "\t", 1U) != CAI_OK) {
      lua_pop(state, 1);
      break;
    }
    if (job_write(job, text, length) != CAI_OK) {
      lua_pop(state, 1);
      break;
    }
    lua_pop(state, 1);
  }
  if (job->code == CAI_OK)
    (void)job_write(job, "\n", 1U);
  return 0;
}
static int local_require(lua_State *state) {
  lua_job *job = borrow_job(state, 1, 1);
  const char *name;
  struct module_paths {
    char relative[512], path[4096], canonical[4096];
  } *paths;
  FILE *file;
  char *source;
  size_t i, length, root_length;
  int status, cache;
  if (job == NULL)
    return dead_borrow(state);
  name = luaL_checkstring(state, 1);
  length = strlen(name);
  if (length == 0U || length > 480U)
    return luaL_error(state, "%s", "invalid local Lua module name");
  for (i = 0U; i < length; i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' ||
          (c == '.' && i > 0U && name[i - 1U] != '.' && i + 1U < length)))
      return luaL_error(state, "%s", "invalid local Lua module name");
  }
  lua_getfield(state, LUA_REGISTRYINDEX, "cai.lua_tools.loaded");
  cache = lua_gettop(state);
  lua_getfield(state, -1, name);
  if (lua_islightuserdata(state, -1) && lua_touserdata(state, -1) == &job_key)
    return luaL_error(state, "%s", "cyclic local Lua module");
  if (!lua_isnil(state, -1))
    return 1;
  lua_pop(state, 1);
  paths = malloc(sizeof(*paths));
  if (paths == NULL)
    return luaL_error(state, "%s", "allocate local module path");
  for (i = 0U; i < length; i++)
    paths->relative[i] = name[i] == '.' ? '/' : name[i];
  paths->relative[length] = '\0';
  if (snprintf(paths->path, sizeof(paths->path), "%s/%s.lua",
               job->handler->directory,
               paths->relative) >= (int)sizeof(paths->path) ||
      realpath(paths->path, paths->canonical) == NULL) {
    free(paths);
    return luaL_error(state, "%s", "local Lua module is unavailable");
  }
  root_length = strlen(job->handler->directory);
  if (strncmp(paths->canonical, job->handler->directory, root_length) != 0 ||
      paths->canonical[root_length] != '/') {
    free(paths);
    return luaL_error(state, "%s", "Lua module escapes package");
  }
  file = fopen(paths->canonical, "rb");
  if (file == NULL) {
    free(paths);
    return luaL_error(state, "%s", "open local Lua module");
  }
  source = malloc(SOURCE_LIMIT + 1U);
  if (source == NULL) {
    free(paths);
    fclose(file);
    return luaL_error(state, "%s", "allocate local Lua module");
  }
  length = fread(source, 1U, SOURCE_LIMIT + 1U, file);
  status = ferror(file) || length > SOURCE_LIMIT;
  fclose(file);
  if (status) {
    free(paths);
    free(source);
    return luaL_error(state, "%s", "Lua module exceeds 1 MiB");
  }
  status = luaL_loadbufferx(state, source, length, paths->canonical, "t");
  free(paths);
  free(source);
  if (status != LUA_OK)
    return lua_error(state);
  lua_pushlightuserdata(state, &job_key);
  lua_setfield(state, cache, name);
  if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
    lua_pushnil(state);
    lua_setfield(state, cache, name);
    return lua_error(state);
  }
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    lua_pushboolean(state, 1);
  }
  lua_pushvalue(state, -1);
  lua_setfield(state, cache, name);
  return 1;
}
static void push_closure(lua_State *state, int reference,
                         lua_CFunction function) {
  lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
  lua_pushcclosure(state, function, 1);
}
static int invoke_body(lua_State *state) {
  lua_job *job = lua_touserdata(state, lua_upvalueindex(1));
  int reference, status;
  const char *message;
  size_t length;
  lua_pushlightuserdata(state, job);
  lua_rawsetp(state, LUA_REGISTRYINDEX, &job_key);
  job->borrow = lua_newuserdatauv(state, sizeof(*job->borrow), 0);
  job->borrow->job = job;
  luaL_newmetatable(state, INPUT_MT);
  lua_pushcfunction(state, input_read);
  lua_setfield(state, -2, "read");
  lua_pushcfunction(state, input_decode);
  lua_setfield(state, -2, "decode");
  lua_pushvalue(state, -1);
  lua_setfield(state, -2, "__index");
  lua_setmetatable(state, -2);
  reference = luaL_ref(state, LUA_REGISTRYINDEX);
  job->borrow_reference = reference;
  luaL_newmetatable(state, ARRAY_MT);
  lua_pop(state, 1);
  if (job->handler->reference == LUA_NOREF) {
    if (job->call->yolo)
      luaL_openlibs(state);
    else {
      luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
      lua_pop(state, 1);
      luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
      lua_pop(state, 1);
      luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
      lua_pop(state, 1);
      luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
      lua_pop(state, 1);
      luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
      lua_pop(state, 1);
      luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
      lua_pop(state, 1);
      lua_pushnil(state);
      lua_setglobal(state, "dofile");
      lua_pushnil(state);
      lua_setglobal(state, "loadfile");
      lua_pushnil(state);
      lua_setglobal(state, "load");
      push_closure(state, reference, local_require);
      lua_setglobal(state, "require");
      lua_newtable(state);
      lua_setfield(state, LUA_REGISTRYINDEX, "cai.lua_tools.loaded");
    }
    push_closure(state, reference, context_print);
    lua_setglobal(state, "print");
    lua_sethook(state, tool_hook, LUA_MASKCOUNT, 1000);
    status = luaL_loadbufferx(state, job->handler->source, job->handler->length,
                              job->handler->directory, "t");
    if (status == LUA_OK)
      status = lua_pcall(state, 0, 1, 0);
    if (status == LUA_OK && lua_istable(state, -1))
      lua_getfield(state, -1, "run");
  } else {
    lua_rawgeti(state, LUA_REGISTRYINDEX, job->handler->reference);
    status = LUA_OK;
  }
  if (status == LUA_OK && !lua_isfunction(state, -1)) {
    lua_pushliteral(state,
                    "Lua package must return a function or a table with run");
    status = LUA_ERRRUN;
  }
  if (status == LUA_OK) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    lua_newtable(state);
    lua_pushstring(state, job->call->workspace);
    lua_setfield(state, -2, "workspace");
    lua_pushboolean(state, job->call->yolo);
    lua_setfield(state, -2, "yolo");
    lua_pushlightuserdata(state, &null_key);
    lua_setfield(state, -2, "null");
    push_closure(state, reference, context_write);
    lua_setfield(state, -2, "write");
    push_closure(state, reference, context_cancelled);
    lua_setfield(state, -2, "cancelled");
    push_closure(state, reference, context_call);
    lua_setfield(state, -2, "call_tool");
    status = lua_pcall(state, 2, 1, 0);
    if (status == LUA_OK && !lua_isnil(state, -1)) {
      if (lua_type(state, -1) == LUA_TSTRING) {
        message = lua_tolstring(state, -1, &length);
        (void)job_write(job, message, length);
      } else
        (void)encode_result(job, state, -1);
    }
  }
  if (status != LUA_OK && job->code == CAI_OK) {
    message = lua_tostring(state, -1);
    job->code = tools_error(job->error, CAI_ERR_INVALID,
                            message != NULL ? message : "Lua tool failed");
  }
  return 0;
}
static int invoke_lua(void *context, cai_error *error) {
  lua_job *job = context;
  lua_State *state = job->state;
  int top = lua_gettop(state), status;
  void *previous;
  (void)error;
  lua_rawgetp(state, LUA_REGISTRYINDEX, &job_key);
  previous = lua_touserdata(state, -1);
  lua_pop(state, 1);
  job->borrow_reference = LUA_NOREF;
  lua_pushlightuserdata(state, job);
  lua_pushcclosure(state, invoke_body, 1);
  status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK && job->code == CAI_OK)
    job->code = tools_error(
        job->error, status == LUA_ERRMEM ? CAI_ERR_NOMEM : CAI_ERR_INVALID,
        lua_tostring(state, -1));
  if (job->borrow != NULL)
    job->borrow->job = NULL;
  luaL_unref(state, LUA_REGISTRYINDEX, job->borrow_reference);
  if (previous != NULL)
    lua_pushlightuserdata(state, previous);
  else
    lua_pushnil(state);
  lua_rawsetp(state, LUA_REGISTRYINDEX, &job_key);
  lua_settop(state, top);
  return job->code;
}
static int handler_run(void *context, cai_source *arguments, cai_sink *output,
                       const cai_plugin_call *call, cai_error *error) {
  lua_handler *handler = context;
  lua_job *job;
  lua_decode *decode;
  lonejson_error local;
  int rc;
  job = calloc(1U, sizeof(*job));
  if (job == NULL)
    return tools_error(error, CAI_ERR_NOMEM, "allocate Lua invocation");
  job->handler = handler;
  job->input = arguments;
  job->output = output;
  job->call = call;
  job->error = error;
  job->deadline =
      tools_time() +
      (double)(call->timeout_ms > 0L ? call->timeout_ms : 120000L) / 1000.0;
  lonejson_error_init(&local);
  job->json = lonejson_new(NULL, &local);
  if (job->json == NULL) {
    free(job);
    return tools_error(error, CAI_ERR_NOMEM, "allocate Lua JSON codec");
  }
  if (job_cancelled(job))
    rc = job->code;
  else if (handler->reference != LUA_NOREF) {
    job->state = handler->tools->config.state;
    if (pthread_equal(pthread_self(), handler->tools->owner))
      rc = invoke_lua(job, error);
    else if (handler->tools->config.dispatch != NULL)
      rc = handler->tools->config.dispatch(
          handler->tools->config.dispatch_context, invoke_lua, job, error);
    else
      rc = tools_error(error, CAI_ERR_INVALID,
                       "host Lua tool requires its owner-thread dispatcher");
  } else {
    job->state = lua_newstate(tools_alloc, job, 0U);
    if (job->state == NULL)
      rc = tools_error(error, CAI_ERR_NOMEM, "allocate private Lua state");
    else {
      rc = invoke_lua(job, error);
      lua_close(job->state);
    }
  }
  while ((decode = job->decoders) != NULL) {
    job->decoders = decode->next;
    free(decode->token);
    free(decode);
  }
  lonejson_free(job->json);
  free(job);
  return rc;
}
static int release_reference(void *context, cai_error *error) {
  lua_handler *handler = context;
  (void)error;
  luaL_unref(handler->tools->config.state, LUA_REGISTRYINDEX,
             handler->reference);
  return CAI_OK;
}
static void handler_destroy(void *context) {
  lua_handler *handler = context;
  int released = 1;
  if (handler->reference != LUA_NOREF) {
    if (pthread_equal(pthread_self(), handler->tools->owner))
      (void)release_reference(handler, NULL);
    else if (handler->tools->config.dispatch != NULL)
      released = handler->tools->config.dispatch(
                     handler->tools->config.dispatch_context, release_reference,
                     handler, NULL) == CAI_OK;
    else
      released = 0;
  }
  if (!released) {
    pthread_mutex_lock(&handler->tools->lock);
    handler->next = handler->tools->retired;
    handler->tools->retired = handler;
    pthread_mutex_unlock(&handler->tools->lock);
    return;
  }
  free(handler->source);
  free(handler->directory);
  free(handler);
}
static int file_prepare(void *context, const cai_plugin_definition *definition,
                        cai_plugin_handler *out, cai_error *error) {
  cai_lua_tools *tools = context;
  lua_handler *handler;
  lua_State *check;
  char path[4096], canonical[4096];
  size_t root_length;
  FILE *file;
  int status;
  if (definition->entrypoint == NULL || definition->entrypoint[0] == '/' ||
      snprintf(path, sizeof(path), "%s/%s", definition->directory,
               definition->entrypoint) >= (int)sizeof(path) ||
      realpath(path, canonical) == NULL)
    return tools_error(error, CAI_ERR_INVALID,
                       "Lua tool entrypoint is unavailable");
  root_length = strlen(definition->directory);
  if (strncmp(canonical, definition->directory, root_length) != 0 ||
      canonical[root_length] != '/')
    return tools_error(error, CAI_ERR_INVALID,
                       "Lua entrypoint escapes package");
  file = fopen(canonical, "rb");
  if (file == NULL)
    return tools_error(error, CAI_ERR_INVALID, "open Lua entrypoint");
  handler = calloc(1U, sizeof(*handler));
  if (handler == NULL) {
    fclose(file);
    return tools_error(error, CAI_ERR_NOMEM, "allocate Lua tool");
  }
  handler->tools = tools;
  handler->reference = LUA_NOREF;
  handler->source = malloc(SOURCE_LIMIT + 1U);
  handler->directory = strdup(definition->directory);
  if (handler->source == NULL || handler->directory == NULL) {
    fclose(file);
    handler_destroy(handler);
    return tools_error(error, CAI_ERR_NOMEM, "read Lua tool");
  }
  handler->length = fread(handler->source, 1U, SOURCE_LIMIT + 1U, file);
  status = ferror(file) || handler->length > SOURCE_LIMIT;
  fclose(file);
  if (status) {
    handler_destroy(handler);
    return tools_error(error, CAI_ERR_LIMIT, "Lua entrypoint exceeds 1 MiB");
  }
  check = luaL_newstate();
  if (check == NULL) {
    handler_destroy(handler);
    return tools_error(error, CAI_ERR_NOMEM, "validate Lua entrypoint");
  }
  status =
      luaL_loadbufferx(check, handler->source, handler->length, canonical, "t");
  if (status != LUA_OK)
    (void)tools_error(error, CAI_ERR_INVALID, lua_tostring(check, -1));
  lua_close(check);
  if (status != LUA_OK) {
    handler_destroy(handler);
    return CAI_ERR_INVALID;
  }
  out->context = handler;
  out->invoke = handler_run;
  out->destroy = handler_destroy;
  return CAI_OK;
}
void cai_lua_tools_config_init(cai_lua_tools_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
    config->decode_limit = 4U * 1024U * 1024U;
  }
}
int cai_lua_tools_open(const cai_lua_tools_config *config, cai_lua_tools **out,
                       cai_error *error) {
  cai_lua_tools *tools;
  if (config == NULL || out == NULL)
    return tools_error(error, CAI_ERR_INVALID,
                       "Lua adapter configuration required");
  *out = NULL;
  tools = calloc(1U, sizeof(*tools));
  if (tools == NULL)
    return tools_error(error, CAI_ERR_NOMEM, "allocate Lua adapter");
  tools->config = *config;
  if (tools->config.decode_limit == 0U)
    tools->config.decode_limit = 4U * 1024U * 1024U;
  tools->owner = pthread_self();
  if (pthread_mutex_init(&tools->lock, NULL) != 0) {
    free(tools);
    return tools_error(error, CAI_ERR_TRANSPORT, "initialize Lua adapter");
  }
  *out = tools;
  return CAI_OK;
}
void cai_lua_tools_close(cai_lua_tools *tools) {
  lua_handler *retired;
  if (tools == NULL)
    return;
  if (tools->config.state != NULL &&
      !pthread_equal(pthread_self(), tools->owner))
    return;
  while ((retired = tools->retired) != NULL) {
    tools->retired = retired->next;
    handler_destroy(retired);
  }
  pthread_mutex_destroy(&tools->lock);
  free(tools);
}
int cai_lua_tools_attach(cai_lua_tools *tools, cai_plugins *plugins,
                         cai_error *error) {
  cai_plugin_executor executor;
  if (tools == NULL)
    return tools_error(error, CAI_ERR_INVALID, "Lua adapter required");
  memset(&executor, 0, sizeof(executor));
  executor.context = tools;
  executor.prepare = file_prepare;
  return cai_plugins_register_executor(plugins, "lua", &executor, error);
}
int cai_lua_tools_define(cai_lua_tools *tools, cai_plugins *plugins,
                         const cai_plugin_definition *definition,
                         int function_index, cai_error *error) {
  lua_handler *handler;
  cai_plugin_handler callback;
  int rc;
  if (tools == NULL || tools->config.state == NULL ||
      !pthread_equal(pthread_self(), tools->owner) ||
      !lua_isfunction(tools->config.state, function_index))
    return tools_error(error, CAI_ERR_INVALID,
                       "Lua owner thread and handler function required");
  handler = calloc(1U, sizeof(*handler));
  if (handler == NULL)
    return tools_error(error, CAI_ERR_NOMEM, "allocate host Lua handler");
  handler->tools = tools;
  lua_pushvalue(tools->config.state, function_index);
  handler->reference = luaL_ref(tools->config.state, LUA_REGISTRYINDEX);
  callback.context = handler;
  callback.invoke = handler_run;
  callback.destroy = handler_destroy;
  rc = cai_plugins_define(plugins, definition, &callback, error);
  if (rc != CAI_OK)
    handler_destroy(handler);
  return rc;
}
