/* Public plugin API facade. No runtime or application private headers. */
#define CAI_LUA_PLUGINS "cai.plugins"
typedef struct cai_lua_plugins {
  cai_plugins *ptr;
  size_t users;
  size_t workers;
#ifdef CAI_LUA_TOOLS
  cai_lua_tools *adapter;
  int trusted;
#endif
} cai_lua_plugins;
static cai_lua_plugins *plugins_check(lua_State *L, int index) {
  cai_lua_plugins *p = luaL_checkudata(L, index, CAI_LUA_PLUGINS);
  luaL_argcheck(L, p->ptr != NULL, index, "tool catalog is closed");
  return p;
}
static void plugins_release_ref(lua_State *L, int *reference) {
  cai_lua_plugins *p;
  if (*reference == 0)
    return;
  lua_rawgeti(L, LUA_REGISTRYINDEX, *reference);
  p = luaL_checkudata(L, -1, CAI_LUA_PLUGINS);
  if (p->users != 0U)
    p->users--;
  lua_pop(L, 1);
  luaL_unref(L, LUA_REGISTRYINDEX, *reference);
  *reference = 0;
}
static int plugins_hold(lua_State *L, int index) {
  cai_lua_plugins *p = plugins_check(L, index);
  p->users++;
  lua_pushvalue(L, index);
  return luaL_ref(L, LUA_REGISTRYINDEX);
}
static void plugins_release_runtime_ref(lua_State *L, int *reference) {
  cai_lua_plugins *p;
  if (*reference == 0)
    return;
  lua_rawgeti(L, LUA_REGISTRYINDEX, *reference);
  p = luaL_checkudata(L, -1, CAI_LUA_PLUGINS);
  if (p->workers != 0U)
    p->workers--;
  lua_pop(L, 1);
  plugins_release_ref(L, reference);
}
static int plugins_close(lua_State *L) {
  cai_lua_plugins *p = luaL_checkudata(L, 1, CAI_LUA_PLUGINS);
  if (p->users != 0U) {
    lua_pushnil(L);
    lua_pushliteral(L, "tool catalog is attached to an agent or runtime");
    return 2;
  }
  cai_plugins_close(p->ptr);
  p->ptr = NULL;
#ifdef CAI_LUA_TOOLS
  cai_lua_tools_close(p->adapter);
  p->adapter = NULL;
#endif
  lua_pushboolean(L, 1);
  return 1;
}
static int plugins_new(lua_State *L) {
  cai_lua_plugins *p;
  cai_plugins_config config;
  cai_error error;
  const char **roots;
  size_t i, count;
  int rc;
  luaL_checktype(L, 1, LUA_TTABLE);
  count = lua_rawlen(L, 1);
  for (i = 0U; i < count; i++) {
    lua_rawgeti(L, 1, (lua_Integer)i + 1);
    luaL_checkstring(L, -1);
    lua_pop(L, 1);
  }
  p = lua_newuserdata(L, sizeof(*p));
  memset(p, 0, sizeof(*p));
  luaL_getmetatable(L, CAI_LUA_PLUGINS);
  lua_setmetatable(L, -2);
  roots = calloc(count + 1U, sizeof(*roots));
  if (roots == NULL)
    return luaL_error(L, "allocate tool roots");
  for (i = 0U; i < count; i++) {
    lua_rawgeti(L, 1, (lua_Integer)i + 1);
    roots[i] = lua_tostring(L, -1);
    lua_pop(L, 1);
  }
  cai_plugins_config_init(&config);
  config.directories = roots;
  config.directory_count = count;
  cai_error_init(&error);
  rc = cai_plugins_open(&config, &p->ptr, &error);
  free(roots);
#ifdef CAI_LUA_TOOLS
  if (rc == CAI_OK) {
    cai_lua_tools_config adapter;
    cai_lua_tools_config_init(&adapter);
    adapter.state = L;
    rc = cai_lua_tools_open(&adapter, &p->adapter, &error);
    if (rc == CAI_OK)
      rc = cai_lua_tools_attach(p->adapter, p->ptr, &error);
  }
#endif
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_error_cleanup(&error);
  return 1;
}
static int plugins_load(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  const char *path = luaL_checkstring(L, 2);
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_plugins_load(p->ptr, path, &error);
  return cai_lua_bool_result(L, rc, &error);
}
static int plugins_reload(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_plugins_reload(p->ptr, &error);
  return cai_lua_bool_result(L, rc, &error);
}
static int plugins_unload(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_plugins_unload(p->ptr, name, &error);
  return cai_lua_bool_result(L, rc, &error);
}
static int plugins_visit_one(void *context, const cai_plugin_definition *d,
                             cai_error *error) {
  lua_State *L = context;
  (void)error;
  lua_newtable(L);
  lua_pushstring(L, d->name);
  lua_setfield(L, -2, "name");
  lua_pushstring(L, d->description);
  lua_setfield(L, -2, "description");
  lua_pushstring(L, d->parameters_json);
  lua_setfield(L, -2, "parameters_json");
  lua_pushstring(L, d->executor);
  lua_setfield(L, -2, "executor");
  lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
  return CAI_OK;
}
static int plugins_list(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  cai_error error;
  int rc;
  cai_error_init(&error);
  lua_newtable(L);
  rc = cai_plugins_visit(p->ptr, plugins_visit_one, L, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_error_cleanup(&error);
  return 1;
}
static int plugins_run(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  const char *name = luaL_checkstring(L, 2);
  cai_plugin_call call;
  cai_lua_source_ctx input;
  cai_lua_sink_ctx output;
  cai_source *source = NULL;
  cai_sink *sink = NULL;
  cai_error error;
  int rc;
  cai_plugin_call_init(&call);
  call.workspace = luaL_optstring(L, 5, ".");
  call.yolo = lua_toboolean(L, 6);
  memset(&input, 0, sizeof(input));
  memset(&output, 0, sizeof(output));
  cai_error_init(&error);
  rc = cai_lua_make_source(L, 3, &input, &source, &error);
  if (rc == CAI_OK)
    rc = cai_lua_make_sink(L, 4, &output, &sink, &error);
  /* Pin against close/reentry from a stream callback. */
  p->users++;
  if (rc == CAI_OK)
    rc = cai_plugins_run(p->ptr, name, source, sink, &call, &error);
  p->users--;
  cai_source_close(source);
  cai_sink_close(sink);
  if (input.callback_ref != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, input.callback_ref);
  if (output.callback_ref != 0)
    luaL_unref(L, LUA_REGISTRYINDEX, output.callback_ref);
  return cai_lua_bool_result(L, rc, &error);
}
#ifdef CAI_LUA_TOOLS
static int plugins_define(lua_State *L) {
  cai_lua_plugins *p = plugins_check(L, 1);
  cai_plugin_definition d;
  cai_error error;
  int rc;
  luaL_checktype(L, 2, LUA_TTABLE);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (p->workers != 0U)
    return luaL_error(L, "host Lua functions require a C owner-thread "
                         "dispatcher; catalog is attached to a worker runtime");
  memset(&d, 0, sizeof(d));
  d.name = cai_lua_opt_string_field(L, 2, "name", NULL);
  d.description = cai_lua_opt_string_field(L, 2, "description", NULL);
  d.parameters_json = cai_lua_opt_string_field(L, 2, "parameters_json", NULL);
  d.executor = "host_lua";
  cai_error_init(&error);
  rc = cai_lua_tools_define(p->adapter, p->ptr, &d, 3, &error);
  if (rc == CAI_OK)
    p->trusted = 1;
  return cai_lua_bool_result(L, rc, &error);
}
#endif
static int cai_lua_agent_use_plugins(lua_State *L) {
  cai_lua_agent *agent = cai_lua_check_agent(L, 1);
  cai_lua_plugins *p = plugins_check(L, 2);
  cai_plugin_call call;
  cai_error error;
  int rc;
  if (agent->active_calls != 0U || agent->child_sessions != 0U)
    return luaL_error(L, "attach plugins before using the agent");
  cai_plugin_call_init(&call);
  call.workspace = luaL_optstring(L, 3, ".");
  call.yolo = lua_toboolean(L, 4);
  cai_error_init(&error);
  rc = cai_agent_use_plugins(agent->ptr, p->ptr, &call, &error);
  if (rc == CAI_OK)
    agent->plugins_ref = plugins_hold(L, 2);
  return cai_lua_bool_result(L, rc, &error);
}
static const luaL_Reg plugins_methods[] = {{"load", plugins_load},
                                           {"reload", plugins_reload},
                                           {"unload", plugins_unload},
                                           {"list", plugins_list},
                                           {"run", plugins_run},
                                           {"close", plugins_close},
#ifdef CAI_LUA_TOOLS
                                           {"define", plugins_define},
#endif
                                           {NULL, NULL}};
