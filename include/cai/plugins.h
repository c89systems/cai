#ifndef CAI_PLUGINS_H
#define CAI_PLUGINS_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_plugins cai_plugins;

/** Per-invocation borrowed capabilities. A host executor must obey this policy.
 * Package discovery is limited to configured roots. Process tools follow the
 * command environment policy; they are not an OS sandbox. No VM is required. */
typedef struct cai_plugin_call {
  const char *workspace;
  int yolo;
  long timeout_ms;
  size_t max_output_bytes;
  int (*cancelled)(void *context);
  void *cancel_context;
  /** Optional host capabilities, called with JSON arguments and streamed
   * output. The host owns authorization and execution on the correct thread. */
  int (*call_tool)(void *context, const char *name, const char *arguments_json,
                   cai_sink *output, cai_error *error);
  void *tool_context;
} cai_plugin_call;

/** One ordinary function tool, independent of its implementation language.
 * Strings and argv are copied by define/load. Executor names are host-defined.
 * Package, entrypoint and argv are execution metadata, never model arguments.
 */
typedef struct cai_plugin_definition {
  const char *name;
  const char *description;
  const char *parameters_json;
  const char *executor;
  const char *directory;
  const char *entrypoint;
  const char *const *argv;
  size_t argc;
  int strict;
} cai_plugin_definition;

/** Prepared handler, owned by its catalog generation after successful define.
 * invoke borrows streams until it returns; it must not retain them afterward.
 * Arguments are one JSON value. Results are UTF-8 text, optionally JSON.
 * A retired handler is destroyed only after all bound calls release it. */
typedef struct cai_plugin_handler {
  void *context;
  int (*invoke)(void *context, cai_source *arguments, cai_sink *output,
                const cai_plugin_call *call, cai_error *error);
  void (*destroy)(void *context);
} cai_plugin_handler;

/** Named executor factory. prepare runs outside catalog locks. Hosts marshal
 * VM access themselves; libcai never calls into a foreign VM implicitly.
 * Context is borrowed until plugins close. */
typedef struct cai_plugin_executor {
  void *context;
  int (*prepare)(void *context, const cai_plugin_definition *definition,
                 cai_plugin_handler *handler, cai_error *error);
} cai_plugin_executor;

typedef struct cai_plugins_config {
  /** Approved package roots. load accepts only canonical descendants. */
  const char *const *directories;
  size_t directory_count;
} cai_plugins_config;

typedef int (*cai_plugin_visitor)(void *context,
                                  const cai_plugin_definition *definition,
                                  cai_error *error);

/** Initialize zero/default values. */
void cai_plugins_config_init(cai_plugins_config *config);
void cai_plugin_call_init(cai_plugin_call *call);
/** Open a catalog. The built-in process executor uses pipes, never a PTY.
 * Callers retain executor contexts until all catalog/agent users close. */
int cai_plugins_open(const cai_plugins_config *config, cai_plugins **out,
                     cai_error *error);
void cai_plugins_close(cai_plugins *plugins);
/** Register an executor once; duplicate names fail. */
int cai_plugins_register_executor(cai_plugins *plugins, const char *name,
                                  const cai_plugin_executor *executor,
                                  cai_error *error);
/** Define or replace a tool atomically. On failure the handler stays caller-
 * owned and the current definition remains active. */
int cai_plugins_define(cai_plugins *plugins,
                       const cai_plugin_definition *definition,
                       const cai_plugin_handler *handler, cai_error *error);
/** Prepare tool.json in an approved package directory and atomically publish
 * it. Failed preparation preserves the previous working definition. */
int cai_plugins_load(cai_plugins *plugins, const char *directory,
                     cai_error *error);
/** Load immediate package subdirectories of approved roots. Each package is
 * atomic; an invalid package returns an error without removing valid tools. */
int cai_plugins_reload(cai_plugins *plugins, cai_error *error);
int cai_plugins_unload(cai_plugins *plugins, const char *name,
                       cai_error *error);
int cai_plugins_visit(cai_plugins *plugins, cai_plugin_visitor visitor,
                      void *context, cai_error *error);
/** Direct execution pins the current handler for the complete invocation. */
int cai_plugins_run(cai_plugins *plugins, const char *name,
                    cai_source *arguments, cai_sink *output,
                    const cai_plugin_call *call, cai_error *error);
/** Attach before using the agent. The agent retains the catalog; later loads
 * apply before its next model request, after the current response's calls.
 * Native tools must be registered before attachment. NULL is a no-op.
 * call and workspace are copied; callback contexts remain borrowed. */
int cai_agent_use_plugins(cai_agent *agent, cai_plugins *plugins,
                          const cai_plugin_call *call, cai_error *error);

#ifdef __cplusplus
}
#endif
#endif
