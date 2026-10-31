#ifndef CAI_LUA_TOOLS_H
#define CAI_LUA_TOOLS_H
#include <cai/plugins.h>

#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
typedef struct cai_lua_tools cai_lua_tools;
/** Run operation on the host Lua owner thread and return its completed result.
 * A host may enqueue it on its event loop and wait cooperatively. The operation
 * and context are borrowed until dispatch returns; cancellation must unwind
 * pending dispatch rather than return with an outstanding operation. */
typedef int (*cai_lua_tools_operation)(void *context, cai_error *error);
typedef int (*cai_lua_tools_dispatch)(void *context,
                                      cai_lua_tools_operation operation,
                                      void *operation_context,
                                      cai_error *error);
typedef struct cai_lua_tools_config {
  /** Optional trusted host VM for define(). File tools always use isolated,
   * per-invocation private states. No host globals survive between file-tool
   * invocations or YOLO policy changes. */
  struct lua_State *state;
  cai_lua_tools_dispatch dispatch;
  void *dispatch_context;
  /** Bound for explicitly materialized input:decode(); zero selects 4 MiB. */
  size_t decode_limit;
} cai_lua_tools_config;

/** This optional library owns the Lua dependency; libcai never links it. */
void cai_lua_tools_config_init(cai_lua_tools_config *config);
int cai_lua_tools_open(const cai_lua_tools_config *config, cai_lua_tools **out,
                       cai_error *error);
/** Close after all attached catalogs and agents. A borrowed Lua state is never
 * closed. Borrowed-state adapters must be closed on their owner thread. */
void cai_lua_tools_close(cai_lua_tools *tools);
/** Install the file-based "lua" executor. The adapter must outlive catalog
 * users. Restricted files use safe libraries, package-local require and host
 * capabilities; YOLO files get the full standard libraries. */
int cai_lua_tools_attach(cai_lua_tools *tools, cai_plugins *plugins,
                         cai_error *error);
/** Define a trusted function already present on the borrowed Lua stack.
 * Called on that state's owner thread. The function is retained until its
 * definition retires; dispatch marshals invocation and retirement as needed.
 * It receives (input, context), with streaming input:read()/context.write().
 * input:decode() explicitly materializes a bounded JSON value as Lua values.
 */
int cai_lua_tools_define(cai_lua_tools *tools, cai_plugins *plugins,
                         const cai_plugin_definition *definition,
                         int function_index, cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
