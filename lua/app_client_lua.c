/** @file Owner-thread Lua workspace client. Every callback copies one bounded
 * C span/metadata row under pcall; no logical result or journal is gathered. */
#define _POSIX_C_SOURCE 200809L
#include "app_client_lua.h"
#include <cai/app_client.h>
#include <errno.h>
#include <lauxlib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLIENT_MT "cai.app.client"
typedef struct client_lua {
  cai_app_client *client;
  lua_State *L;
  int pumping, self_ref;
} client_lua;
typedef struct callback_call {
  client_lua *client;
  const char *name;
  const void *event;
  void (*push)(lua_State *, const void *);
} callback_call;

static void text(lua_State *L, const char *key, const char *value) {
  lua_pushstring(L, value);
  lua_setfield(L, -2, key);
}
static void integer(lua_State *L, const char *key, lua_Integer value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}
static void boolean(lua_State *L, const char *key, int value) {
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}
static void number(lua_State *L, const char *key, double value) {
  lua_pushnumber(L, value);
  lua_setfield(L, -2, key);
}
static void identity(lua_State *L, const char *key, uint64_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
  text(L, key, buffer);
}
static int fail(lua_State *L, int code, const char *message) {
  lua_pushnil(L);
  lua_pushstring(L, message != NULL ? message : cai_status_string(code));
  lua_pushinteger(L, code);
  return 3;
}
static int result(lua_State *L, int rc, cai_error *error) {
  if (rc == CAI_OK)
    lua_pushboolean(L, 1);
  else
    fail(L, rc, error->message);
  cai_error_cleanup(error);
  return rc == CAI_OK ? 1 : 3;
}
/** Exact uint64 conversion; never pass identities through Lua floating point.
 */
static int unsigned_value(lua_State *L, int index, uint64_t *value) {
  const char *s;
  size_t n, i;
  uint64_t next = 0U;
  if (lua_isinteger(L, index)) {
    lua_Integer v = lua_tointeger(L, index);
    if (v < 0)
      return 0;
    *value = (uint64_t)v;
    return 1;
  }
  if (lua_type(L, index) != LUA_TSTRING)
    return 0;
  s = lua_tolstring(L, index, &n);
  if (n == 0U || n > 20U)
    return 0;
  for (i = 0U; i < n; ++i) {
    unsigned int digit = (unsigned char)s[i] - (unsigned int)'0';
    if (digit > 9U || next > (UINT64_MAX - digit) / 10U)
      return 0;
    next = next * 10U + digit;
  }
  *value = next;
  return 1;
}
static int field_u64(lua_State *L, int table, const char *key, uint64_t *out,
                     int required) {
  int valid;
  lua_pushstring(L, key);
  lua_rawget(L, table);
  valid = lua_isnil(L, -1) ? !required : unsigned_value(L, -1, out);
  lua_pop(L, 1);
  return valid;
}
static int field_text(lua_State *L, int table, const char *key,
                      const char **out, int required) {
  size_t n;
  int valid = 1;
  lua_pushstring(L, key);
  lua_rawget(L, table);
  if (lua_isnil(L, -1))
    valid = !required;
  else if (lua_type(L, -1) != LUA_TSTRING)
    valid = 0;
  else {
    *out = lua_tolstring(L, -1, &n);
    valid = strlen(*out) == n;
  }
  lua_pop(L, 1);
  return valid;
}
static int field_bool(lua_State *L, int table, const char *key, int *out) {
  int valid;
  lua_pushstring(L, key);
  lua_rawget(L, table);
  valid = lua_isnil(L, -1) || lua_isboolean(L, -1);
  if (!lua_isnil(L, -1))
    *out = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return valid;
}
static uint64_t clock_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0U;
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
static client_lua *check(lua_State *L) {
  return luaL_checkudata(L, 1, CLIENT_MT);
}
static void push_queue(lua_State *L, const cai_input_state *q) {
  lua_newtable(L);
  integer(L, "count", (lua_Integer)q->count);
  identity(L, "revision", q->revision);
  identity(L, "view_revision", q->view_revision);
  boolean(L, "stopped", q->stopped);
  boolean(L, "auto_approval", q->auto_approval);
  boolean(L, "uncertain", q->uncertain);
}
static void push_entry(lua_State *L, const cai_input_entry *e) {
  lua_newtable(L);
#define ID(member) identity(L, #member, e->member)
  ID(id);
  ID(revision);
  ID(client_id);
  ID(request_id);
  ID(edit_owner);
  ID(edit_token);
#undef ID
  text(L, "text", e->text);
  text(L, "origin", e->origin);
  text(L, "intent", e->intent == CAI_INPUT_STEERING ? "steer" : "queued");
#define B(member) boolean(L, #member, e->member)
  B(held);
  B(edit_held);
  B(ready);
  B(stopped);
  B(action);
  B(independent);
#undef B
}
static void push_state(lua_State *L, const cai_app_client_state *s) {
  const cai_workspace_client_state *t = &s->transport;
  const cai_app_transcript_state *r = &s->transcript;
  lua_newtable(L);
  text(L, "session", s->session);
  identity(L, "conversation", s->conversation);
  identity(L, "producer", s->producer);
  identity(L, "last_request", s->last_request);
  boolean(L, "binding", s->binding);
  boolean(L, "bound", s->bound);
  boolean(L, "ready", s->ready);
  push_queue(L, &s->queue);
  lua_setfield(L, -2, "queue");
  lua_newtable(L);
  integer(L, "phase", t->phase);
  identity(L, "attachment", t->attachment);
  identity(L, "generation", t->generation);
  identity(L, "controller", t->controller);
  identity(L, "deadline_ms", t->deadline_ms);
  lua_pushlstring(L, (const char *)t->instance, sizeof(t->instance));
  lua_setfield(L, -2, "instance");
  boolean(L, "has_welcome", t->has_welcome);
  boolean(L, "persist", t->persist);
  boolean(L, "has_identity", t->has_identity);
  integer(L, "error_code", t->error_code);
  integer(L, "server_phase", t->server_phase);
  lua_setfield(L, -2, "transport");
  lua_newtable(L);
  text(L, "session", r->session);
  text(L, "collection", r->collection);
#define ID(member) identity(L, #member, r->member)
  ID(stream);
  ID(conversation);
  ID(sequence);
  ID(position);
  ID(watermark);
  ID(visibility);
#undef ID
  boolean(L, "subscribing", r->subscribing);
  boolean(L, "active", r->active);
  boolean(L, "replaying", r->replaying);
  lua_setfield(L, -2, "transcript");
}
static void push_settings(lua_State *L, const cai_agent_runtime_settings *s) {
  lua_newtable(L);
  integer(L, "present", s->present);
  text(L, "model", s->model);
  text(L, "reasoning_effort", s->reasoning_effort);
  text(L, "reasoning_summary", s->reasoning_summary);
  text(L, "service_tier", s->service_tier);
}
static void push_view(lua_State *L, const cai_app_view *v) {
  const cai_agent_runtime_metrics *m = &v->metrics;
  const cai_app_goal_view *g = &v->goal;
  lua_newtable(L);
#define ID(member) identity(L, #member, v->member)
  ID(revision);
  ID(conversation);
  ID(received_ms);
#undef ID
#define I(member) integer(L, #member, (lua_Integer)v->member)
  I(code);
  I(outcome);
  I(exit_code);
  I(runtime_state);
  I(yield_time_ms);
#undef I
#define B(member) boolean(L, #member, v->member)
  B(available);
  B(stale);
  B(running);
  B(runtime_ready);
  B(yolo);
  B(foreground);
#undef B
#define T(member) text(L, #member, v->member)
  T(reason);
  T(session_id);
  T(provider);
  T(last_response_id);
  T(provider_conversation_id);
  T(log_path);
#undef T
  push_settings(L, &v->effective);
  lua_setfield(L, -2, "effective");
  push_settings(L, &v->pending);
  lua_setfield(L, -2, "pending");
  lua_newtable(L);
  boolean(L, "has_goal", g->has_goal);
  boolean(L, "has_token_budget", g->has_token_budget);
  text(L, "status", g->status);
#define I(member) integer(L, #member, (lua_Integer)g->member)
  I(token_budget);
  I(tokens_used);
  I(remaining_tokens);
  I(elapsed_seconds);
  I(created_at);
  I(updated_at);
#undef I
  lua_setfield(L, -2, "goal");
  lua_newtable(L);
#define I(member) integer(L, #member, (lua_Integer)m->member)
  I(context_window_tokens);
  I(context_used_tokens);
  I(last_turn_finished_unix_seconds);
#undef I
  identity(L, "turn_elapsed_ms", m->turn_elapsed_ms);
  identity(L, "last_turn_duration_ms", m->last_turn_duration_ms);
  boolean(L, "has_context_usage", m->has_context_usage);
  boolean(L, "turn_active", m->turn_active);
  boolean(L, "has_last_turn", m->has_last_turn);
  lua_newtable(L);
#define I(member)                                                              \
  integer(L, #member, (lua_Integer)m->session_usage.usage.member)
  I(input_tokens);
  I(input_cached_tokens);
  I(output_tokens);
  I(output_reasoning_tokens);
  I(total_tokens);
#undef I
  number(L, "estimated_spend_usd", m->session_usage.estimated_spend_usd);
  number(L, "estimated_spend_eur", m->session_usage.estimated_spend_eur);
  boolean(L, "limit_exceeded", m->session_usage.limit_exceeded);
  lua_setfield(L, -2, "session_usage");
  lua_setfield(L, -2, "metrics");
  lua_newtable(L);
#define B(member) boolean(L, #member, v->quota.member)
  B(has_short_window);
  B(has_weekly);
  B(has_credit_balance);
  B(credits_unlimited);
#undef B
  integer(L, "short_window_seconds",
          (lua_Integer)v->quota.short_window_seconds);
#define N(member) number(L, #member, v->quota.member)
  N(short_window_remaining_percent);
  N(weekly_remaining_percent);
  N(credit_balance);
#undef N
  lua_setfield(L, -2, "quota");
}
static void push_transcript(lua_State *L, const void *data) {
  const cai_transcript_event *e = data;
  lua_newtable(L);
  integer(L, "kind", e->kind);
#define ID(member) identity(L, #member, e->member)
  ID(stream);
  ID(conversation);
  ID(sequence);
  ID(timestamp_ns);
  ID(position);
  ID(offset);
  ID(watermark);
  ID(visibility);
  ID(omitted);
#undef ID
  integer(L, "utc_offset_seconds", (lua_Integer)e->utc_offset_seconds);
  text(L, "session", e->session);
  text(L, "collection", e->collection);
  text(L, "type", e->type);
  text(L, "reason", e->reason);
  integer(L, "length", (lua_Integer)e->length);
  if (e->data != NULL) {
    lua_pushlstring(L, e->data, e->length);
    lua_setfield(L, -2, "data");
  }
  boolean(L, "has_data", e->has_data);
  integer(L, "code", e->code);
  integer(L, "failure", e->failure);
}
static void push_event(lua_State *L, const void *data) {
  const cai_app_client_event *e = data;
  const cai_input_observation *o = e->input;
  lua_newtable(L);
  integer(L, "type", e->type);
  identity(L, "request", e->request);
  push_state(L, e->state);
  lua_setfield(L, -2, "state");
  if (e->status != NULL) {
    push_view(L, e->status);
    lua_setfield(L, -2, "status");
  }
  if (o == NULL)
    return;
  lua_newtable(L);
  integer(L, "type", o->type);
  identity(L, "conversation", o->conversation);
  identity(L, "producer", o->producer);
  identity(L, "last_request", o->last_request);
  integer(L, "index", (lua_Integer)o->index);
  push_queue(L, &o->state);
  lua_setfield(L, -2, "state");
  push_entry(L, &o->entry);
  lua_setfield(L, -2, "entry");
  lua_newtable(L);
  integer(L, "type", o->change.type);
  integer(L, "index", (lua_Integer)o->change.index);
  identity(L, "previous_view_revision", o->change.previous_view_revision);
  identity(L, "client_id", o->change.client_id);
  identity(L, "request_id", o->change.request_id);
  push_entry(L, &o->change.entry);
  lua_setfield(L, -2, "entry");
  push_queue(L, &o->change.state);
  lua_setfield(L, -2, "state");
  lua_setfield(L, -2, "change");
  lua_newtable(L);
  identity(L, "request_id", o->result.request_id);
  identity(L, "item_id", o->result.item_id);
  identity(L, "item_revision", o->result.item_revision);
  identity(L, "edit_token", o->result.edit_token);
  integer(L, "outcome", o->result.outcome);
  integer(L, "code", o->result.code);
  text(L, "reason", o->result.reason);
  lua_setfield(L, -2, "result");
  lua_setfield(L, -2, "input");
}
static void push_execution(lua_State *L, const void *data) {
  const cai_app_execution_event *e = data;
  const cai_execution_record *r = e->record;
  lua_newtable(L);
  integer(L, "code", e->code);
  text(L, "reason", e->reason);
  boolean(L, "replay", e->replay);
  boolean(L, "subagent", e->subagent);
  if (e->boundary != NULL) {
    push_transcript(L, e->boundary);
    lua_setfield(L, -2, "boundary");
  }
  if (r == NULL)
    return;
  lua_newtable(L);
#define T(member) text(L, #member, r->member)
  T(kind);
  T(session_id);
  T(parent_session_id);
  T(parent_call_id);
  T(subagent);
  T(tool_name);
  T(call_id);
  T(field);
  T(model);
  T(base_url);
  T(last_response_id);
  T(data);
#undef T
  identity(L, "timestamp_ns", r->timestamp_ns);
  identity(L, "source_sequence", r->source_sequence);
  identity(L, "offset", r->offset);
  integer(L, "utc_offset_seconds", (lua_Integer)r->utc_offset_seconds);
  integer(L, "code", (lua_Integer)r->code);
  integer(L, "http_status", (lua_Integer)r->http_status);
  boolean(L, "final", r->final);
  boolean(L, "line_start", r->line_start);
  lua_setfield(L, -2, "record");
}
static void push_catalog(lua_State *L, const void *data) {
  const cai_app_catalog_event *e = data;
  lua_newtable(L);
  integer(L, "kind", e->kind);
  integer(L, "phase", e->phase);
  identity(L, "revision", e->revision);
  identity(L, "conversation", e->conversation);
  identity(L, "index", e->index);
  text(L, "text", e->text);
  integer(L, "code", e->code);
}
static int callback_dispatch(lua_State *L) {
  callback_call *call = lua_touserdata(L, 1);
  lua_rawgeti(L, LUA_REGISTRYINDEX, call->client->self_ref);
  lua_getiuservalue(L, -1, 1);
  lua_getfield(L, -1, call->name);
  if (lua_isnil(L, -1))
    return 0;
  call->push(L, call->event);
  lua_call(L, 1, 0);
  return 0;
}
static int callback(client_lua *ud, const char *name, const void *event,
                    void (*push)(lua_State *, const void *), cai_error *error) {
  callback_call call;
  lua_State *L = ud->L;
  int top = lua_gettop(L), rc = CAI_OK;
  call.client = ud;
  call.name = name;
  call.event = event;
  call.push = push;
  lua_pushcfunction(L, callback_dispatch);
  lua_pushlightuserdata(L, &call);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    size_t length = 0U;
    const char *message =
        lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &length) : NULL;
    if (message == NULL) {
      message = "Lua workspace callback failed";
      length = strlen(message);
    }
    if (length > 1024U)
      length = 1024U;
    cai_error_cleanup(error);
    error->message = malloc(length + 1U);
    if (error->message != NULL) {
      memcpy(error->message, message, length);
      error->message[length] = '\0';
    }
    rc = error->code = error->message != NULL ? CAI_ERR_INVALID : CAI_ERR_NOMEM;
  }
  lua_settop(L, top);
  return rc;
}
static int event_callback(void *ud, const cai_app_client_event *e,
                          cai_error *error) {
  return callback(ud, "on_event", e, push_event, error);
}
static int transcript_callback(void *ud, const cai_transcript_event *e,
                               cai_error *error) {
  return callback(ud, "on_transcript", e, push_transcript, error);
}
static int history_callback(void *ud, const cai_transcript_event *e,
                            cai_error *error) {
  return callback(ud, "on_prompt_history", e, push_transcript, error);
}
static int execution_callback(void *ud, const cai_app_execution_event *e,
                              cai_error *error) {
  return callback(ud, "on_execution", e, push_execution, error);
}
static int catalog_callback(void *ud, const cai_app_catalog_event *e,
                            cai_error *error) {
  return callback(ud, "on_catalog", e, push_catalog, error);
}

static int close_client(lua_State *L) {
  client_lua *ud = check(L);
  if (ud->pumping)
    return fail(L, CAI_ERR_INVALID, "cannot close a client from its callback");
  cai_app_client_close(ud->client);
  ud->client = NULL;
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  lua_pushboolean(L, 1);
  return 1;
}
static int prepare(lua_State *L) {
  client_lua *ud = check(L);
  struct pollfd descriptor;
  uint64_t deadline;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_app_client_prepare(ud->client, &descriptor, &deadline, &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  lua_newtable(L);
  integer(L, "fd", descriptor.fd);
  integer(L, "events", descriptor.events);
  identity(L, "deadline_ms", deadline);
  return 1;
}
static int progress(lua_State *L, int waiting) {
  client_lua *ud = check(L);
  cai_error error;
  uint64_t now = clock_ms();
  int rc;
  if (ud->pumping)
    return fail(L, CAI_ERR_INVALID, "workspace client pump is not reentrant");
  if (ud->client == NULL)
    return fail(L, CAI_ERR_INVALID, "workspace client is closed");
  cai_error_init(&error);
  if (waiting) {
    struct pollfd descriptor;
    uint64_t deadline, end = 0U;
    lua_Integer timeout = -1;
    if (!lua_isnoneornil(L, 2)) {
      if (!lua_isinteger(L, 2) || (timeout = lua_tointeger(L, 2)) < -1 ||
          timeout > INT_MAX)
        return fail(L, CAI_ERR_INVALID,
                    "wait timeout must be -1 or 0..INT_MAX milliseconds");
    }
    rc = cai_app_client_prepare(ud->client, &descriptor, &deadline, &error);
    if (rc != CAI_OK)
      return result(L, rc, &error);
    if (timeout >= 0)
      end = now + (uint64_t)timeout;
    if (deadline != 0U && (end == 0U || deadline < end))
      end = deadline;
    do {
      int milliseconds = end == 0U             ? -1
                         : end <= now          ? 0
                         : end - now > INT_MAX ? INT_MAX
                                               : (int)(end - now);
      rc = descriptor.fd < 0 ? 0 : poll(&descriptor, 1U, milliseconds);
      now = clock_ms();
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
      return fail(L, CAI_ERR_TRANSPORT, "poll workspace attachment");
  } else if (!lua_isnoneornil(L, 2) && !unsigned_value(L, 2, &now))
    return fail(L, CAI_ERR_INVALID, "now_ms must be an unsigned integer");
  ud->L = L;
  lua_pushvalue(L, 1);
  ud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  ud->pumping = 1;
  rc = cai_app_client_pump(ud->client, now, &error);
  ud->pumping = 0;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->self_ref);
  ud->self_ref = LUA_NOREF;
  return result(L, rc, &error);
}
static int pump(lua_State *L) { return progress(L, 0); }
static int wait_client(lua_State *L) { return progress(L, 1); }
static int state(lua_State *L) {
  cai_app_client_state snapshot;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_app_client_get_state(check(L)->client, &snapshot, &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  push_state(L, &snapshot);
  return 1;
}
static int view(lua_State *L) {
  const cai_app_view *snapshot;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_app_client_get_view(check(L)->client, &snapshot, &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  if (snapshot == NULL)
    lua_pushnil(L);
  else
    push_view(L, snapshot);
  return 1;
}
static int directory(lua_State *L) {
  const char *value = cai_app_client_directory(check(L)->client);
  if (value == NULL)
    return fail(L, CAI_ERR_INVALID, "workspace client is closed");
  lua_pushstring(L, value);
  return 1;
}
/** All scope/ID fields come from the caller. No generation refresh or implicit
 * request numbering can silently turn an old mutation into a new request. */
#define READ(target, key, required)                                            \
  do {                                                                         \
    uint64_t parsed = 0U;                                                      \
    if (!field_u64(L, 2, key, &parsed, required))                              \
      return fail(L, CAI_ERR_INVALID, key " must be an unsigned integer");     \
    target = parsed;                                                           \
  } while (0)
static int bind_client(lua_State *L) {
  client_lua *ud = check(L);
  uint64_t request, epoch, producer;
  const char *session = NULL;
  cai_error error;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID, "bind requires a request table");
  READ(request, "request_id", 1);
  READ(epoch, "conversation", 0);
  READ(producer, "producer", 0);
  if (!field_text(L, 2, "session", &session, 0))
    return fail(L, CAI_ERR_INVALID, "session must be text without NUL");
  cai_error_init(&error);
  return result(L,
                cai_app_client_bind(ud->client, request, epoch, producer,
                                    session, &error),
                &error);
}
static int manage_delivery(lua_State *L) {
  client_lua *ud = check(L);
  cai_error error;
  cai_error_init(&error);
  return result(L, cai_app_client_manage_delivery(ud->client, &error), &error);
}
static int settled(lua_State *L) {
  client_lua *ud = check(L);
  cai_input_scope scope;
  uint64_t after;
  cai_error error;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID, "settlement requires a request table");
  READ(scope.conversation, "conversation", 1);
  READ(scope.generation, "generation", 1);
  READ(after, "after_request", 1);
  cai_error_init(&error);
  return result(L, cai_app_client_settled(ud->client, &scope, after, &error),
                &error);
}
static int shell_result(lua_State *L) {
  cai_app_shell_result r;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_app_client_get_shell_result(check(L)->client, &r, &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  lua_newtable(L);
  identity(L, "request", r.request);
  identity(L, "recording", r.recording);
  identity(L, "first_sequence", r.first_sequence);
  identity(L, "last_sequence", r.last_sequence);
  integer(L, "code", r.code);
  text(L, "reason", r.reason);
  return 1;
}
static int shell_request(lua_State *L) {
  client_lua *ud = check(L);
  cai_app_shell_request r;
  cai_input_scope scope;
  uint64_t id, code = 0U;
  const char *operation = NULL;
  cai_error error;
  if (!lua_istable(L, 2) || !field_text(L, 2, "operation", &operation, 1))
    return fail(L, CAI_ERR_INVALID, "shell operation is required");
  memset(&r, 0, sizeof(r));
  if (strcmp(operation, "begin") == 0)
    r.operation = CAI_APP_SHELL_BEGIN;
  else if (strcmp(operation, "write") == 0)
    r.operation = CAI_APP_SHELL_WRITE;
  else if (strcmp(operation, "finish") == 0)
    r.operation = CAI_APP_SHELL_FINISH;
  else
    return fail(L, CAI_ERR_INVALID, "unknown shell operation");
  READ(id, "request_id", 1);
  READ(scope.conversation, "conversation", 1);
  READ(scope.generation, "generation", 1);
  READ(r.recording, "recording", 0);
  READ(r.sequence, "sequence", 0);
  READ(r.duration_ms, "duration_ms", 0);
  READ(code, "exit_code", 0);
  if (code > 255U)
    return fail(L, CAI_ERR_INVALID, "shell exit_code exceeds 255");
  r.exit_code = (unsigned int)code;
  lua_getfield(L, 2, "data");
  if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
    return fail(L, CAI_ERR_INVALID, "shell data must be a byte string");
  if (!lua_isnil(L, -1))
    r.data = lua_tolstring(L, -1, &r.length);
  cai_error_init(&error);
  return result(L, cai_app_client_shell(ud->client, id, &scope, &r, &error),
                &error);
}
static int request(lua_State *L) {
  static const char *const operations[] = {
      "offer",         "begin_edit",  "end_edit", "abort_edit", "approve",
      "withdraw",      "set_intent",  "stop",     "resume",     "clear",
      "auto_approval", "synchronize", "resolve"};
  client_lua *ud = check(L);
  cai_input_request r;
  cai_input_scope scope;
  cai_error error;
  const char *operation = NULL, *intent = NULL, *origin = NULL;
  size_t i;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID, "queue request requires a table");
  memset(&r, 0, sizeof(r));
  memset(&scope, 0, sizeof(scope));
  if (!field_text(L, 2, "operation", &operation, 1))
    return fail(L, CAI_ERR_INVALID, "request operation is required");
  for (i = 0U; i < sizeof(operations) / sizeof(operations[0]); ++i)
    if (strcmp(operation, operations[i]) == 0)
      break;
  if (i == sizeof(operations) / sizeof(operations[0]))
    return fail(L, CAI_ERR_INVALID, "unknown frontend queue operation");
  r.operation = (cai_input_operation)i;
  READ(scope.conversation, "conversation", 1);
  READ(scope.generation, "generation", 1);
#define R(member, required) READ(r.member, #member, required)
  R(request_id, 1);
  R(item_id, 0);
  R(item_revision, 0);
  R(queue_revision, 0);
  R(edit_token, 0);
  R(target_request_id, 0);
#undef R
  if (!field_text(L, 2, "text", &r.text, 0) ||
      !field_text(L, 2, "intent", &intent, 0) ||
      !field_text(L, 2, "origin", &origin, 0) ||
      !field_bool(L, 2, "enabled", &r.enabled))
    return fail(L, CAI_ERR_INVALID,
                "queue text/intent/origin must be text without NUL; enabled "
                "must be boolean");
  if (intent != NULL && strcmp(intent, "steer") != 0 &&
      strcmp(intent, "queued") != 0)
    return fail(L, CAI_ERR_INVALID, "intent must be queued or steer");
  r.intent = intent != NULL && strcmp(intent, "steer") == 0 ? CAI_INPUT_STEERING
                                                            : CAI_INPUT_QUEUED;
  if (origin != NULL) {
    if (strlen(origin) >= sizeof(r.origin))
      return fail(L, CAI_ERR_INVALID, "queue origin exceeds 127 bytes");
    strcpy(r.origin, origin);
  }
  cai_error_init(&error);
  return result(L, cai_app_client_request(ud->client, &scope, &r, &error),
                &error);
}
static int connection_control(lua_State *L, int operation) {
  client_lua *ud = check(L);
  uint64_t id, generation;
  const char *value = NULL;
  cai_error error;
  int rc;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID,
                "connection control requires a request table");
  READ(id, "request_id", 1);
  READ(generation, "generation", 1);
  if (operation == 3 && !field_text(L, 2, "text", &value, 1))
    return fail(L, CAI_ERR_INVALID, "prompt text is required without NUL");
  cai_error_init(&error);
  if (operation == 0)
    rc = cai_app_client_takeover(ud->client, id, generation, &error);
  else if (operation == 1)
    rc = cai_app_client_shutdown(ud->client, id, generation, &error);
  else if (operation == 2)
    rc = cai_app_client_cancel(ud->client, id, generation, &error);
  else
    rc = cai_app_client_save_prompt(ud->client, id, generation, value, &error);
  return result(L, rc, &error);
}
static int takeover(lua_State *L) { return connection_control(L, 0); }
static int shutdown_client(lua_State *L) { return connection_control(L, 1); }
static int cancel(lua_State *L) { return connection_control(L, 2); }
static int save_prompt(lua_State *L) { return connection_control(L, 3); }
static int subscribe(lua_State *L) {
  client_lua *ud = check(L);
  uint64_t id, epoch, position, sequence;
  long long turns = -1;
  const char *session = NULL, *collection = NULL;
  cai_error error;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID, "subscribe requires a request table");
  READ(id, "request_id", 1);
  READ(epoch, "conversation", 0);
  READ(position, "after_position", 0);
  READ(sequence, "after_sequence", 0);
  if (!field_text(L, 2, "session", &session, 1) ||
      !field_text(L, 2, "collection", &collection, 0))
    return fail(L, CAI_ERR_INVALID,
                "subscription session/collection must be text without NUL");
  lua_getfield(L, 2, "replay_turns");
  if (!lua_isnil(L, -1)) {
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < -1)
      return fail(L, CAI_ERR_INVALID,
                  "replay_turns must be -1 or a nonnegative integer");
    turns = (long long)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);
  cai_error_init(&error);
  return result(L,
                cai_app_client_subscribe(ud->client, id, epoch, session,
                                         collection, position, sequence, turns,
                                         &error),
                &error);
}
static int prompt_history(lua_State *L) {
  client_lua *ud = check(L);
  uint64_t id, epoch, limit;
  const char *session = NULL;
  cai_error error;
  if (!lua_istable(L, 2))
    return fail(L, CAI_ERR_INVALID, "prompt history requires a request table");
  READ(id, "request_id", 1);
  READ(epoch, "conversation", 0);
  READ(limit, "limit", 0);
  if (limit > UINT_MAX || !field_text(L, 2, "session", &session, 1))
    return fail(L, CAI_ERR_INVALID,
                "history needs session text and a limit in 0..UINT_MAX");
  cai_error_init(&error);
  return result(L,
                cai_app_client_prompt_history(ud->client, id, epoch, session,
                                              (unsigned int)limit, &error),
                &error);
}
#undef READ
static int end_stream(lua_State *L, int detach) {
  client_lua *ud = check(L);
  cai_error error;
  int rc;
  if (ud->pumping)
    return fail(L, CAI_ERR_INVALID,
                "cannot end an attachment or stream from its callback");
  cai_error_init(&error);
  rc = detach ? cai_app_client_detach(ud->client, &error)
              : cai_app_client_unsubscribe(ud->client, &error);
  return result(L, rc, &error);
}
static int detach(lua_State *L) { return end_stream(L, 1); }
static int unsubscribe(lua_State *L) { return end_stream(L, 0); }

static int open_client(lua_State *L) {
  static const char *const fields[] = {
      "workspace",         "runtime_directory",
      "expected_instance", "handshake_timeout_ms",
      "backlog_limit",     "requirements",
      "on_event",          "on_transcript",
      "on_execution",      "on_prompt_history",
      "on_catalog"};
  static const char *const callbacks[] = {"on_event", "on_transcript",
                                          "on_execution", "on_prompt_history",
                                          "on_catalog"};
  cai_app_client_config config;
  client_lua *ud;
  cai_error error;
  uint64_t value = 0U;
  size_t i, n;
  int rc, user;
  unsigned int callback_mask = 0U;
  if (!lua_istable(L, 1))
    return fail(L, CAI_ERR_INVALID,
                "workspace client configuration must be a table");
  cai_app_client_config_init(&config);
  if (!field_text(L, 1, "workspace", &config.workspace, 1) ||
      !field_text(L, 1, "runtime_directory", &config.runtime_directory, 0) ||
      !field_u64(L, 1, "handshake_timeout_ms", &config.handshake_timeout_ms,
                 0) ||
      !field_u64(L, 1, "backlog_limit", &value, 0) || value > (size_t)-1)
    return fail(L, CAI_ERR_INVALID, "invalid workspace client configuration");
  config.backlog_limit = (size_t)value;
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    size_t key_length = 0U;
    const char *key = lua_type(L, -2) == LUA_TSTRING
                          ? lua_tolstring(L, -2, &key_length)
                          : NULL;
    if (key != NULL && strlen(key) != key_length)
      return fail(L, CAI_ERR_INVALID,
                  "configuration field names cannot contain NUL");
    for (i = 0U; key != NULL && i < sizeof(fields) / sizeof(fields[0]); ++i)
      if (strcmp(key, fields[i]) == 0)
        break;
    if (key == NULL || i == sizeof(fields) / sizeof(fields[0]))
      return fail(L, CAI_ERR_INVALID,
                  "unknown workspace client configuration field");
    lua_pop(L, 1);
  }
  lua_pushliteral(L, "expected_instance");
  lua_rawget(L, 1);
  if (!lua_isnil(L, -1)) {
    const char *bytes =
        lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : NULL;
    if (bytes == NULL || n != CAI_WORKSPACE_INSTANCE_BYTES)
      return fail(L, CAI_ERR_INVALID,
                  "expected_instance needs exactly 16 bytes");
    config.expected_instance = (const unsigned char *)bytes;
  }
  lua_pop(L, 1);
  lua_getfield(L, 1, "requirements");
  if (!lua_isnil(L, -1)) {
    cai_workspace_claim *claims;
    size_t count;
    int table = lua_gettop(L);
    if (!lua_istable(L, table) ||
        (count = lua_rawlen(L, table)) > CAI_WORKSPACE_CLAIM_LIMIT)
      return fail(L, CAI_ERR_INVALID, "requirements must be a bounded array");
    claims = lua_newuserdatauv(L, count * sizeof(*claims), 0);
    memset(claims, 0, count * sizeof(*claims));
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
      lua_Integer index;
      if (!lua_isinteger(L, -2) || (index = lua_tointeger(L, -2)) < 1 ||
          (lua_Unsigned)index > count)
        return fail(L, CAI_ERR_INVALID, "requirements must be a dense array");
      lua_pop(L, 1);
    }
    for (i = 0U; i < count; ++i) {
      const char *digest;
      lua_rawgeti(L, table, (lua_Integer)i + 1);
      if (!lua_istable(L, -1) ||
          !field_text(L, lua_gettop(L), "name", &claims[i].name, 1))
        return fail(L, CAI_ERR_INVALID, "requirement name must be text");
      lua_getfield(L, -1, "digest");
      digest = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : NULL;
      if (digest == NULL || n != sizeof(claims[i].digest))
        return fail(L, CAI_ERR_INVALID,
                    "requirement digest needs exactly 32 bytes");
      memcpy(claims[i].digest, digest, n);
      lua_pop(L, 2);
    }
    config.requirements = claims;
    config.requirement_count = count;
  }
  ud = lua_newuserdatauv(L, sizeof(*ud), 1);
  memset(ud, 0, sizeof(*ud));
  ud->self_ref = LUA_NOREF;
  luaL_setmetatable(L, CLIENT_MT);
  user = lua_gettop(L);
  lua_newtable(L);
  for (i = 0U; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i) {
    lua_getfield(L, 1, callbacks[i]);
    if (!lua_isnil(L, -1) && !lua_isfunction(L, -1))
      return fail(L, CAI_ERR_INVALID, "workspace callbacks must be functions");
    if (lua_isfunction(L, -1))
      callback_mask |= 1U << i;
    lua_setfield(L, -2, callbacks[i]);
  }
  lua_setiuservalue(L, user, 1);
  config.event = (callback_mask & 1U) != 0U ? event_callback : NULL;
  config.context = ud;
  config.transcript = (callback_mask & 2U) != 0U ? transcript_callback : NULL;
  config.transcript_context = ud;
  config.execution = (callback_mask & 4U) != 0U ? execution_callback : NULL;
  config.execution_context = ud;
  config.prompt_history = (callback_mask & 8U) != 0U ? history_callback : NULL;
  config.prompt_history_context = ud;
  config.catalog = (callback_mask & 16U) != 0U ? catalog_callback : NULL;
  config.catalog_context = ud;
  cai_error_init(&error);
  rc = cai_app_client_open(&config, clock_ms(), &ud->client, &error);
  if (rc != CAI_OK)
    return result(L, rc, &error);
  cai_error_cleanup(&error);
  return 1;
}

void cai_lua_app_client_register(lua_State *L) {
  static const luaL_Reg methods[] = {{"prepare", prepare},
                                     {"pump", pump},
                                     {"wait", wait_client},
                                     {"get_state", state},
                                     {"get_view", view},
                                     {"directory", directory},
                                     {"bind", bind_client},
                                     {"request", request},
                                     {"takeover", takeover},
                                     {"shutdown", shutdown_client},
                                     {"cancel", cancel},
                                     {"save_prompt", save_prompt},
                                     {"manage_delivery", manage_delivery},
                                     {"settled", settled},
                                     {"shell", shell_request},
                                     {"get_shell_result", shell_result},
                                     {"prompt_history", prompt_history},
                                     {"subscribe", subscribe},
                                     {"unsubscribe", unsubscribe},
                                     {"detach", detach},
                                     {"close", close_client},
                                     {"__gc", close_client},
                                     {"__close", close_client},
                                     {NULL, NULL}};
  static const struct {
    const char *name;
    lua_Integer value;
  } constants[] = {
#define C(name)                                                                \
  {                                                                            \
    #name, CAI_##name                                                          \
  }
      {"CLIENT_CONNECTION", CAI_APP_CLIENT_CONNECTION},
      {"CLIENT_INPUT", CAI_APP_CLIENT_INPUT},
      {"CLIENT_BIND_REJECTED", CAI_APP_CLIENT_BIND_REJECTED},
      {"CLIENT_STATUS", CAI_APP_CLIENT_STATUS},
      {"CLIENT_CONTROL_RESULT", CAI_APP_CLIENT_CONTROL_RESULT},
      {"CLIENT_HISTORY_RESULT", CAI_APP_CLIENT_HISTORY_RESULT},
      {"CLIENT_SHELL_RESULT", CAI_APP_CLIENT_SHELL_RESULT},
      C(WORKSPACE_CONNECTING),
      C(WORKSPACE_GREETING),
      C(WORKSPACE_CONNECTED),
      C(WORKSPACE_DETACHING),
      C(WORKSPACE_DISCONNECTED),
      C(WORKSPACE_FAILED),
      C(WORKSPACE_IDENTIFIED),
      C(ATTACHMENT_STARTING),
      C(ATTACHMENT_READY),
      C(ATTACHMENT_STOPPING),
      C(ATTACHMENT_NOT_STOPPED),
      C(ATTACHMENT_LAST_DETACHED),
      C(ATTACHMENT_STARTUP_ABANDONED),
      C(ATTACHMENT_STOP_REQUESTED),
      C(TRANSCRIPT_REPLAY_BEGIN),
      C(TRANSCRIPT_EVENT_BEGIN),
      C(TRANSCRIPT_DATA),
      C(TRANSCRIPT_EVENT_END),
      C(TRANSCRIPT_REPLAY_END),
      C(TRANSCRIPT_ERROR),
      C(TRANSCRIPT_SHELL_OUTPUT),
      C(TRANSCRIPT_SHELL_OUTPUT_END),
      C(TRANSCRIPT_FAILED),
      C(TRANSCRIPT_SLOW),
      C(TRANSCRIPT_HISTORY_CHANGED),
      C(TRANSCRIPT_CONTEXT_CLOSED),
      C(INPUT_OBSERVATION_SNAPSHOT_BEGIN),
      C(INPUT_OBSERVATION_SNAPSHOT_ITEM),
      C(INPUT_OBSERVATION_SNAPSHOT_END),
      C(INPUT_OBSERVATION_CHANGE),
      C(INPUT_OBSERVATION_RESULT),
      C(INPUT_OBSERVATION_DISCONNECTED),
      C(INPUT_ACCEPTED),
      C(INPUT_REJECTED),
      C(INPUT_UNKNOWN),
      {"CATALOG_SKILLS", CAI_APP_CATALOG_SKILLS},
      {"CATALOG_BEGIN", CAI_APP_CATALOG_BEGIN},
      {"CATALOG_ITEM", CAI_APP_CATALOG_ITEM},
      {"CATALOG_END", CAI_APP_CATALOG_END},
      {"CATALOG_ERROR", CAI_APP_CATALOG_ERROR}
#undef C
  };
  size_t i;
  luaL_newmetatable(L, CLIENT_MT);
  luaL_setfuncs(L, methods, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_pushcfunction(L, open_client);
  lua_setfield(L, -2, "client");
  for (i = 0U; i < sizeof(constants) / sizeof(constants[0]); ++i)
    integer(L, constants[i].name, constants[i].value);
}
