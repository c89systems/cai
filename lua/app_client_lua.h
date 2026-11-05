/** @file Lua facade registration; implementation uses installed public C APIs.
 */
#ifndef CAI_APP_CLIENT_LUA_H
#define CAI_APP_CLIENT_LUA_H
#include <lua.h>
void cai_lua_app_client_register(lua_State *L);
#endif
