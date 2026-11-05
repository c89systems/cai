/** @file Lua owner lifecycle, built solely on the public native C APIs.
 * Streaming observations belong to app.client; native owners never enter Lua.
 * Included after the facade's argument/status conversion helpers. */

static int owner_unsigned(lua_State *L, int index, uint64_t *out) {
  const char *value;
  size_t i, length;
  uint64_t number = 0U;
  if (lua_isinteger(L, index)) {
    lua_Integer v = lua_tointeger(L, index);
    if (v < 0)
      return 0;
    *out = (uint64_t)v;
    return 1;
  }
  if (lua_type(L, index) != LUA_TSTRING)
    return 0;
  value = lua_tolstring(L, index, &length);
  if (length == 0U || length > 20U)
    return 0;
  for (i = 0U; i < length; ++i) {
    unsigned int digit = (unsigned char)value[i] - (unsigned int)'0';
    if (digit > 9U || number > (UINT64_MAX - digit) / 10U)
      return 0;
    number = number * 10U + digit;
  }
  *out = number;
  return 1;
}
static void owner_field(lua_State *L, int index, const char *name) {
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
/** Validate metadata tables without invoking __index or accepting typo keys. */
static int owner_keys(lua_State *L, int index, const char *first,
                      const char *second) {
  index = lua_absindex(L, index);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    const char *name =
        lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
    if (name == NULL ||
        (strcmp(name, first) != 0 && strcmp(name, second) != 0)) {
      lua_pop(L, 2);
      return 0;
    }
    lua_pop(L, 1);
  }
  return 1;
}
/** Return a Lua-rooted explicit environment vector, not a process mutation.
 * NULL selects native capture; a vector containing only NULL is empty. */
static int owner_context(lua_State *L, cai_config_context *context,
                         const char *const **entries) {
  size_t count = 0U, i = 0U;
  const char **vector;
  int source, values;
  *entries = NULL;
  memset(context, 0, sizeof(*context));
  if (lua_isnoneornil(L, 3))
    return 1;
  if (!lua_istable(L, 3) || !owner_keys(L, 3, "directory", "environment")) {
    failure(L, "owner context accepts directory and environment");
    return 0;
  }
  owner_field(L, 3, "directory");
  if (!lua_isnil(L, -1)) {
    context->directory =
        lua_type(L, -1) == LUA_TSTRING ? plain_string(L, -1) : NULL;
    if (context->directory == NULL || context->directory[0] != '/') {
      failure(L, "owner directory must be absolute text without NUL");
      return 0;
    }
  }
  lua_pop(L, 1);
  owner_field(L, 3, "environment");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  if (!lua_istable(L, -1)) {
    failure(L, "owner environment must be a map of string names and values");
    return 0;
  }
  source = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, source)) {
    const char *name =
        lua_type(L, -2) == LUA_TSTRING ? plain_string(L, -2) : NULL;
    const char *value =
        lua_type(L, -1) == LUA_TSTRING ? plain_string(L, -1) : NULL;
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL ||
        value == NULL || count >= (size_t)-1 / sizeof(*vector) - 1U) {
      failure(L, "owner environment needs nonempty names without '=' or NUL "
                 "and string values without NUL");
      return 0;
    }
    ++count;
    lua_pop(L, 1);
  }
  vector = lua_newuserdatauv(L, (count + 1U) * sizeof(*vector), 0);
  lua_newtable(L);
  values = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, source)) {
    lua_pushfstring(L, "%s=%s", lua_tostring(L, -2), lua_tostring(L, -1));
    vector[i] = lua_tostring(L, -1);
    lua_rawseti(L, values, (lua_Integer)++i);
    lua_pop(L, 1);
  }
  vector[i] = NULL;
  *entries = vector;
  return 1;
}
static int owner_lifetime(lua_State *L, cai_attachment_config *lifetime,
                          const cai_attachment_config **out) {
  *out = NULL;
  if (lua_isnoneornil(L, 4))
    return 1;
  if (!lua_istable(L, 4) ||
      !owner_keys(L, 4, "persist", "first_attachment_timeout_ms")) {
    failure(L,
            "owner lifetime accepts persist and first_attachment_timeout_ms");
    return 0;
  }
  cai_attachment_config_init(lifetime);
  owner_field(L, 4, "persist");
  if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) {
    failure(L, "owner persist must be boolean");
    return 0;
  }
  lifetime->persist = lua_toboolean(L, -1);
  lua_pop(L, 1);
  owner_field(L, 4, "first_attachment_timeout_ms");
  if (!lua_isnil(L, -1) &&
      !owner_unsigned(L, -1, &lifetime->first_attachment_timeout_ms)) {
    failure(L, "first_attachment_timeout_ms must be an unsigned integer");
    return 0;
  }
  lua_pop(L, 1);
  *out = lifetime;
  return 1;
}
static int start_owner(lua_State *L, int server) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  cai_config_context context;
  cai_attachment_config lifetime;
  const cai_attachment_config *selected = NULL;
  const char *const *entries;
  const char **arguments;
  size_t count;
  cai_error error;
  int rc;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  if (ud->running)
    return failure(
        L,
        ud->asynchronous
            ? "application has an unjoined owner; call join before restarting"
            : "application is already running");
  if (ud->has_observers)
    return failure(L, "asynchronous owners use cai.app.client observations; "
                      "omit on_event/on_replay or use synchronous run");
  if (!server && !lua_isnoneornil(L, 4))
    return failure(L, "owner lifetime requires start_server");
  /* Temporary argument/environment userdata must not occupy absent optional
   * context/lifetime slots. The public inputs keep stable stack indices. */
  lua_settop(L, 4);
  if (!read_arguments(L, &arguments, &count) ||
      (server && !owner_lifetime(L, &lifetime, &selected)) ||
      !owner_context(L, &context, &entries))
    return 2;
  cai_error_init(&error);
  rc = entries != NULL
           ? cai_environment_open(entries, &context.environment, &error)
           : CAI_OK;
  if (rc == CAI_OK)
    rc = server
             ? cai_app_start_server(ud->app, &context, (int)count, arguments,
                                    selected, &error)
             : cai_app_start(ud->app, &context, (int)count, arguments, &error);
  cai_environment_close(context.environment);
  if (rc == CAI_OK) {
    ud->running = ud->asynchronous = 1;
    lua_pushboolean(L, 1);
  } else
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
static int start_app(lua_State *L) { return start_owner(L, 0); }
static int start_server(lua_State *L) { return start_owner(L, 1); }
static int join_app(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  cai_error error;
  int rc, code;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  if (!ud->asynchronous)
    return failure(L, "application has no asynchronous owner to join");
  cai_error_init(&error);
  rc = cai_app_join(ud->app, &code, &error);
  if (rc == CAI_OK) {
    ud->running = ud->asynchronous = 0;
    lua_pushinteger(L, code);
  } else
    failure(L, error.message != NULL ? error.message : cai_status_string(rc));
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 1 : 2;
}
static int status_fd(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  lua_pushinteger(L, cai_app_status_fd(ud->app));
  return 1;
}
static uint64_t owner_clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static int wait_status(lua_State *L) {
  app_lua *ud = luaL_testudata(L, 1, APP_MT);
  lua_Integer timeout = -1;
  struct pollfd descriptor;
  uint64_t now, until = 0U;
  int rc, saved_errno;
  if (ud == NULL || ud->app == NULL)
    return failure(L, "application is closed");
  if (!lua_isnoneornil(L, 2) &&
      (!lua_isinteger(L, 2) || (timeout = lua_tointeger(L, 2)) < -1 ||
       timeout > INT_MAX))
    return failure(L, "status timeout must be -1 or 0..INT_MAX milliseconds");
  descriptor.fd = cai_app_status_fd(ud->app);
  descriptor.events = POLLIN;
  descriptor.revents = 0;
  now = owner_clock_ms();
  if (timeout >= 0)
    until = now + (uint64_t)timeout;
  do {
    int remaining = until == 0U ? -1 : until <= now ? 0 : (int)(until - now);
    rc = poll(&descriptor, 1U, remaining);
    saved_errno = errno;
    now = owner_clock_ms();
  } while (rc < 0 && saved_errno == EINTR);
  if (rc < 0 || (descriptor.revents & (POLLNVAL | POLLERR)) != 0)
    return failure(L, "wait for owner status failed");
  if (status_read(L, 1) != 1)
    return 2;
  lua_pushboolean(L, rc > 0);
  return 2;
}
