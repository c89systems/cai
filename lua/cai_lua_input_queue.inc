/* Public-core queue facade. No application, terminal or storage headers. */
#define CAI_LUA_PENDING_INPUT "cai.pending_input"
typedef struct cai_lua_pending_input {
  cai_input_queue *queue;
  cai_input_queue_client *client;
  unsigned long long serial;
} cai_lua_pending_input;

static void cai_lua_input_number(lua_State *L, const char *key,
                                 unsigned long long value) {
  char text[32];
  (void)snprintf(text, sizeof(text), "%llu", value);
  lua_pushstring(L, text);
  lua_setfield(L, -2, key);
}
static int cai_lua_input_field(lua_State *L, const char *key,
                               unsigned long long *value) {
  const char *text;
  size_t length, i;
  *value = 0U;
  lua_getfield(L, 2, key);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  text = lua_tolstring(L, -1, &length);
  if (text == NULL || length == 0U) {
    lua_pop(L, 1);
    return 0;
  }
  for (i = 0U; i < length; ++i) {
    unsigned int digit = (unsigned char)text[i] - (unsigned int)'0';
    if (digit > 9U || *value > (~(unsigned long long)0 - digit) / 10U) {
      lua_pop(L, 1);
      return 0;
    }
    *value = *value * 10U + digit;
  }
  lua_pop(L, 1);
  return 1;
}
static cai_lua_pending_input *cai_lua_input_check(lua_State *L) {
  return luaL_checkudata(L, 1, CAI_LUA_PENDING_INPUT);
}
static void cai_lua_input_entry(lua_State *L, const cai_input_entry *entry) {
  lua_createtable(L, 0, 12);
  cai_lua_input_number(L, "id", entry->id);
  cai_lua_input_number(L, "revision", entry->revision);
  cai_lua_input_number(L, "edit_token", entry->edit_token);
  lua_pushstring(L, entry->text);
  lua_setfield(L, -2, "text");
  lua_pushstring(L, entry->intent == CAI_INPUT_STEERING ? "steer" : "queued");
  lua_setfield(L, -2, "intent");
#define CAI_LUA_INPUT_FLAG(name)                                               \
  lua_pushboolean(L, entry->name);                                             \
  lua_setfield(L, -2, #name)
  CAI_LUA_INPUT_FLAG(held);
  CAI_LUA_INPUT_FLAG(edit_held);
  CAI_LUA_INPUT_FLAG(stopped);
  CAI_LUA_INPUT_FLAG(ready);
  CAI_LUA_INPUT_FLAG(action);
  CAI_LUA_INPUT_FLAG(independent);
#undef CAI_LUA_INPUT_FLAG
}
static int cai_lua_input_close(lua_State *L) {
  cai_lua_pending_input *self = cai_lua_input_check(L);
  cai_input_queue_client_close(self->client);
  self->client = NULL;
  self->queue = NULL;
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  return 0;
}
static int cai_lua_input_state(lua_State *L) {
  cai_lua_pending_input *self = cai_lua_input_check(L);
  cai_input_state state;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_input_queue_get_state(self->queue, &state, &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  lua_createtable(L, 0, 6);
  lua_pushinteger(L, (lua_Integer)state.count);
  lua_setfield(L, -2, "count");
  cai_lua_input_number(L, "revision", state.revision);
  cai_lua_input_number(L, "view_revision", state.view_revision);
  lua_pushboolean(L, state.stopped);
  lua_setfield(L, -2, "stopped");
  lua_pushboolean(L, state.auto_approval);
  lua_setfield(L, -2, "auto_approval");
  lua_pushboolean(L, state.uncertain);
  lua_setfield(L, -2, "uncertain");
  cai_error_cleanup(&error);
  return 1;
}
static int cai_lua_input_at(lua_State *L) {
  cai_lua_pending_input *self = cai_lua_input_check(L);
  lua_Integer index = luaL_checkinteger(L, 2);
  cai_input_entry entry;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = index > 0 && (unsigned long long)index <= (size_t)-1
           ? cai_input_queue_entry_at(self->client, (size_t)index - 1U, &entry,
                                      &error)
           : cai_lua_set_error(&error, CAI_ERR_INVALID,
                               "input index starts at 1");
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  cai_lua_input_entry(L, &entry);
  cai_error_cleanup(&error);
  return 1;
}
static int cai_lua_input_request(lua_State *L) {
  static const char *const operations[] = {
      "offer",         "begin_edit",  "end_edit", "abort_edit", "approve",
      "withdraw",      "set_intent",  "stop",     "resume",     "clear",
      "auto_approval", "synchronize", "resolve",  NULL};
  cai_lua_pending_input *self = cai_lua_input_check(L);
  cai_input_request request;
  cai_input_result result;
  cai_error error;
  const char *operation, *intent;
  size_t length;
  int i, valid, rc;
  luaL_checktype(L, 2, LUA_TTABLE);
  memset(&request, 0, sizeof(request));
  cai_error_init(&error);
  lua_getfield(L, 2, "operation");
  operation = lua_tolstring(L, -1, &length);
  for (i = 0; operations[i] != NULL; ++i)
    if (operation != NULL && strlen(operation) == length &&
        strcmp(operation, operations[i]) == 0)
      break;
  lua_pop(L, 1);
  request.operation = (cai_input_operation)i;
  valid = operations[i] != NULL;
#define CAI_LUA_INPUT_READ(name)                                               \
  valid = cai_lua_input_field(L, #name, &request.name) && valid
  CAI_LUA_INPUT_READ(request_id);
  CAI_LUA_INPUT_READ(item_id);
  CAI_LUA_INPUT_READ(item_revision);
  CAI_LUA_INPUT_READ(queue_revision);
  CAI_LUA_INPUT_READ(edit_token);
  CAI_LUA_INPUT_READ(target_request_id);
#undef CAI_LUA_INPUT_READ
  lua_getfield(L, 2, "intent");
  intent = lua_isnil(L, -1) ? "queued" : lua_tostring(L, -1);
  valid = valid && intent != NULL &&
          (strcmp(intent, "queued") == 0 || strcmp(intent, "steer") == 0);
  request.intent = intent != NULL && strcmp(intent, "steer") == 0
                       ? CAI_INPUT_STEERING
                       : CAI_INPUT_QUEUED;
  lua_pop(L, 1);
  lua_getfield(L, 2, "enabled");
  valid = valid && (lua_isnil(L, -1) || lua_isboolean(L, -1));
  request.enabled = lua_toboolean(L, -1);
  lua_pop(L, 1);
  lua_getfield(L, 2, "text");
  request.text = lua_isnil(L, -1) ? NULL : lua_tolstring(L, -1, &length);
  valid = valid && (lua_isnil(L, -1) ||
                    (request.text != NULL && strlen(request.text) == length));
  if (request.request_id == 0U) {
    if (self->serial == ~(unsigned long long)0)
      valid = 0;
    else
      request.request_id = ++self->serial;
  } else if (request.request_id > self->serial)
    self->serial = request.request_id;
  rc = valid ? cai_input_queue_request(self->client, &request, &result, &error)
             : cai_lua_set_error(&error, CAI_ERR_INVALID,
                                 "invalid pending-input request");
  lua_pop(L, 1);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  lua_createtable(L, 0, 7);
  cai_lua_input_number(L, "request_id", result.request_id);
  cai_lua_input_number(L, "item_id", result.item_id);
  cai_lua_input_number(L, "item_revision", result.item_revision);
  cai_lua_input_number(L, "edit_token", result.edit_token);
  lua_pushstring(L, result.outcome == CAI_INPUT_ACCEPTED   ? "accepted"
                    : result.outcome == CAI_INPUT_REJECTED ? "rejected"
                                                           : "unknown");
  lua_setfield(L, -2, "outcome");
  lua_pushinteger(L, result.code);
  lua_setfield(L, -2, "code");
  lua_pushstring(L, result.reason);
  lua_setfield(L, -2, "reason");
  cai_error_cleanup(&error);
  return 1;
}
static int cai_lua_input_consume(lua_State *L) {
  cai_lua_pending_input *self = cai_lua_input_check(L);
  cai_input_entry entry;
  cai_input_claim claim;
  cai_error error;
  lua_Integer index = luaL_checkinteger(L, 2);
  int rc;
  cai_error_init(&error);
  rc = index > 0 && (unsigned long long)index <= (size_t)-1
           ? cai_input_queue_entry_at(self->client, (size_t)index - 1U, &entry,
                                      &error)
           : cai_lua_set_error(&error, CAI_ERR_INVALID,
                               "input index starts at 1");
  if (rc == CAI_OK && lua_toboolean(L, 3))
    rc = cai_input_queue_claim(self->queue, entry.id, entry.revision, &claim,
                               &error);
  else if (rc == CAI_OK)
    rc = cai_input_queue_set_ready(self->queue, entry.id, lua_toboolean(L, 4),
                                   &error);
  if (rc != CAI_OK)
    return cai_lua_fail(L, rc, &error);
  if (lua_toboolean(L, 3)) {
    cai_lua_input_entry(L, &claim.entry);
    cai_lua_input_number(L, "position", claim.position);
    cai_input_claim_cleanup(&claim);
    cai_error_cleanup(&error);
    return 1;
  }
  return cai_lua_bool_result(L, rc, &error);
}
static int cai_lua_input_ready(lua_State *L) {
  luaL_checktype(L, 3, LUA_TBOOLEAN);
  lua_settop(L, 3);
  lua_pushvalue(L, 3);
  lua_pushboolean(L, 0);
  lua_replace(L, 3);
  return cai_lua_input_consume(L);
}
static int cai_lua_input_claim(lua_State *L) {
  lua_settop(L, 2);
  lua_pushboolean(L, 1);
  return cai_lua_input_consume(L);
}
static const luaL_Reg cai_lua_pending_input_methods[] = {
    {"state", cai_lua_input_state},
    {"entry", cai_lua_input_at},
    {"request", cai_lua_input_request},
    {"set_ready", cai_lua_input_ready},
    {"claim", cai_lua_input_claim},
    {"close", cai_lua_input_close},
    {NULL, NULL}};
static int cai_lua_runtime_pending_input(lua_State *L) {
  cai_lua_agent_runtime *runtime = cai_lua_check_agent_runtime(L, 1);
  cai_lua_pending_input *self;
  cai_error error;
  int rc;
  self = lua_newuserdatauv(L, sizeof(*self), 1);
  memset(self, 0, sizeof(*self));
  luaL_getmetatable(L, CAI_LUA_PENDING_INPUT);
  lua_setmetatable(L, -2);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  cai_error_init(&error);
  cai_lua_agent_runtime_enter(runtime);
  rc = cai_agent_runtime_pending_input(runtime->ptr, &self->queue, &error);
  if (rc == CAI_OK)
    rc = cai_input_queue_client_open(self->queue, &self->client, &error);
  cai_lua_agent_runtime_leave(runtime);
  if (rc != CAI_OK) {
    self->queue = NULL;
    lua_pop(L, 1);
    return cai_lua_fail(L, rc, &error);
  }
  cai_error_cleanup(&error);
  return 1;
}
