/* Public, allocation-free DTG API facade. Years remain 00..99. */
static int cai_lua_dtg_failure(lua_State *L, int rc) {
  cai_error error;
  int code = rc == CAI_DTG_SPACE ? CAI_ERR_LIMIT : CAI_ERR_INVALID;
  cai_error_init(&error);
  cai_lua_set_error(&error, code, "invalid DTG or unrepresentable time");
  return cai_lua_fail(L, code, &error);
}
static int cai_lua_dtg_parse(lua_State *L) {
  cai_dtg value;
  size_t n;
  const char *text = luaL_checklstring(L, 1, &n);
  int rc;
  if (memchr(text, '\0', n) != NULL)
    return cai_lua_dtg_failure(L, CAI_DTG_INVALID);
  rc = cai_dtg_parse(text, &value);
  if (rc != CAI_DTG_OK)
    return cai_lua_dtg_failure(L, rc);
  lua_newtable(L);
#define DTG_INTEGER(name)                                                      \
  lua_pushinteger(L, (lua_Integer)value.name);                                 \
  lua_setfield(L, -2, #name)
#define DTG_BOOLEAN(name)                                                      \
  lua_pushboolean(L, value.name);                                              \
  lua_setfield(L, -2, #name)
  DTG_INTEGER(day);
  DTG_INTEGER(hour);
  DTG_INTEGER(minute);
  DTG_INTEGER(month);
  DTG_INTEGER(year);
  DTG_INTEGER(offset_seconds);
  DTG_BOOLEAN(has_month_year);
  DTG_BOOLEAN(has_zone);
  DTG_BOOLEAN(offset_known);
#undef DTG_INTEGER
#undef DTG_BOOLEAN
  lua_pushstring(L, value.zone);
  lua_setfield(L, -2, "zone");
  return 1;
}
static int cai_lua_dtg_integer(lua_State *L, const char *field, int *out) {
  lua_Integer value = 0;
  lua_getfield(L, 1, field);
  if (!lua_isnil(L, -1))
    value = luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  if (value < INT_MIN || value > INT_MAX)
    return 0;
  *out = (int)value;
  return 1;
}
static int cai_lua_dtg_format(lua_State *L) {
  cai_dtg value;
  const char *zone;
  size_t zone_length;
  char text[CAI_DTG_CAPACITY];
  int rc;
  luaL_checktype(L, 1, LUA_TTABLE);
  memset(&value, 0, sizeof(value));
  if (!cai_lua_dtg_integer(L, "day", &value.day) ||
      !cai_lua_dtg_integer(L, "hour", &value.hour) ||
      !cai_lua_dtg_integer(L, "minute", &value.minute) ||
      !cai_lua_dtg_integer(L, "month", &value.month) ||
      !cai_lua_dtg_integer(L, "year", &value.year))
    return cai_lua_dtg_failure(L, CAI_DTG_INVALID);
  value.has_month_year = cai_lua_opt_bool_field(L, 1, "has_month_year", 0);
  value.has_zone = cai_lua_opt_bool_field(L, 1, "has_zone", 0);
  lua_getfield(L, 1, "zone");
  zone = luaL_optlstring(L, -1, "", &zone_length);
  if (zone_length >= sizeof(value.zone) ||
      memchr(zone, '\0', zone_length) != NULL) {
    lua_pop(L, 1);
    return cai_lua_dtg_failure(L, CAI_DTG_INVALID);
  }
  strcpy(value.zone, zone);
  lua_pop(L, 1);
  rc = cai_dtg_format(&value, text, sizeof(text));
  if (rc != CAI_DTG_OK)
    return cai_lua_dtg_failure(L, rc);
  lua_pushstring(L, text);
  return 1;
}
static int cai_lua_dtg_format_time(lua_State *L) {
  lua_Integer stamp = luaL_checkinteger(L, 1);
  lua_Integer reference = luaL_checkinteger(L, 2);
  time_t when = (time_t)stamp, now = (time_t)reference;
  char text[CAI_DTG_CAPACITY];
  int rc;
  if ((lua_Integer)when != stamp || (lua_Integer)now != reference)
    return cai_lua_dtg_failure(L, CAI_DTG_RANGE);
  rc = cai_dtg_format_time(when, now, lua_toboolean(L, 3), text, sizeof(text));
  if (rc != CAI_DTG_OK)
    return cai_lua_dtg_failure(L, rc);
  lua_pushstring(L, text);
  return 1;
}
static int cai_lua_dtg_zone(lua_State *L) {
  lua_Integer offset = luaL_checkinteger(L, 1);
  char text[CAI_DTG_ZONE_CAPACITY];
  int rc;
  if (offset <= -86400 || offset >= 86400)
    return cai_lua_dtg_failure(L, CAI_DTG_RANGE);
  rc = cai_dtg_zone((long)offset, text, sizeof(text));
  if (rc != CAI_DTG_OK)
    return cai_lua_dtg_failure(L, rc);
  lua_pushstring(L, text);
  return 1;
}
