# Dynamic tools

A tool package supplies an ordinary model function and its implementation. It is
independent of skills: skills supply instructions; tools execute operations.
Core libcai owns the catalog, schemas and reload boundaries. It requires no
scripting VM. `libcai_lua_tools` is a separately selected Lua executor, shipped
as both `.a` and `.so`; the dependency direction is Lua adapter → libcai.

## Application configuration

File packages are enabled by default. `--no-plugins`, `CAI_PLUGINS=false`, or
`[plugins] enabled=false` disables both file and host-supplied catalogs.
The application name changes environment prefixes and default paths as usual.

| INI setting | Flag | Environment | Empty/default meaning |
| --- | --- | --- | --- |
| `[plugins] enabled` | `--[no-]plugins` | `CAI_PLUGINS` | true |
| `[plugins] directory` | `--tools-dir DIR` | `CAI_TOOLS_DIR` | `data_dir/tools` |
| `[plugins] workspace_directory` | `--workspace-tools-dir DIR` | `CAI_WORKSPACE_TOOLS_DIR` | `workspace/.program_name/tools` |

INI paths are relative to the configuration file; CLI paths use the selected
workspace, following the application's normal path rules. Package roots must
be absolute at the core API boundary. Missing roots are allowed and need not be
created until tools are installed. Each immediate, non-hidden subdirectory
with `tool.json` is loaded at startup. No filesystem polling or compilation is
performed by the catalog.

`/tools` lists registered tools. `/tools reload` scans configured roots;
`/tools reload PATH` prepares one package; `/tools unload NAME` retires one tool.
These mutations use Softline's request-boundary policy. The model's
`reload_tools` and `unload_tool` do the same; the former describes the package
format, approved roots and available executors so an agent can build a tool
using its existing workspace tools. Removing files alone does not unload a tool.

Loading prepares a complete candidate before publication. An invalid candidate
preserves the working definition. Reloading several packages is atomic per
package, not a transaction over the whole directory. Native names and the two
management names are reserved; conflicts fail explicitly.

## Process packages

Example `workspace/.cai/tools/echo/tool.json`:

```json
{
  "name": "local_echo",
  "description": "Echo the supplied JSON object",
  "parameters": {"type": "object", "properties": {}},
  "executor": "process",
  "argv": ["/bin/cat"]
}
```

The process receives one JSON value on stdin. Stdout streams UTF-8 tool-result
text; stderr is drained independently and its first 2 KiB provide diagnostics
on a nonzero exit. Only the result is presented as an ordinary formatted tool
result; this is not the command runner's raw transcript path. Input and output
flow concurrently through bounded pipe buffers. The catalog does not first
collect arguments or results into a whole string.

`argv` is literal, without shell expansion. An optional package-relative
`entrypoint` is appended to it and must resolve inside the package. Examples:

| Implementation | argv | entrypoint |
| --- | --- | --- |
| Shell | `["/bin/sh"]` | `main.sh` |
| Python | `["python3"]` | `main.py` |
| JavaScript | `["node"]` | `main.js` |
| Go binary | omit | `tool` (executable, already compiled) |
| Go source | `["go", "run"]` | `main.go` |

The executor does not install interpreters, dependencies or compilers. PATH,
HOME and XDG follow the existing command environment policy; the working
directory is the agent workspace. Process tools use independent pipes and no
PTY. Like CAI's managed commands, they are not an OS security sandbox. Only
load trusted packages. Cancellation/deadline/output-limit failure terminates
and reaps the process group. Defaults are 120 seconds and 4 MiB output;
`cai_plugin_call` lets an embedding host override those invocation limits.

## Embedded Lua packages

Use `executor: "lua"` and `entrypoint: "main.lua"`, with the same metadata and
JSON schema. No external Lua executable is needed. A module returns a function
(or a table with `run`):

```lua
return function(input, ctx)
  local args = assert(input:decode())
  return {answer = args.value + 1}
end
```

`input:read([size])` streams up to 4096 bytes; `ctx.write(text)` streams result
chunks. `input:decode()` is explicitly materialized JSON, bounded to 4 MiB by
default. JSON null is `ctx.null`; decoded empty arrays remain arrays when
returned. Returning a string appends text; other supported values serialize as
JSON; nil adds nothing. `print` writes to the result rather than the terminal.

Every file invocation gets a fresh VM. Restricted mode exposes base, table,
string, math, utf8 and coroutine libraries, plus a package-local `require` for
`.lua` modules. It omits io, os, debug, package, load, loadfile and dofile. Module
paths are canonicalized inside the package; cyclic requires fail explicitly.
Restricted memory is bounded to 32 MiB. Instruction hooks observe deadlines and
cancellation. YOLO enables standard libraries and removes that memory sandbox;
unrestricted native library operations must themselves cooperate with shutdown.
Use `ctx.write` or `print` for results, not terminal/file stdout APIs.

`ctx.workspace`, `ctx.yolo` and `ctx.cancelled()` describe the invocation.
The runtime supplies `ctx.call_tool(name, json_arguments [, on_chunk])` for
enabled read_file, list_files, apply_patch, exec_command and write_stdin tools.
Their ordinary policies still apply. Without a callback, their output streams
to the plugin result. A callback receives streamed chunks for inspection or
transformation. For example:

```lua
assert(ctx.call_tool('read_file', '{"path":"README.md"}', function(chunk)
  ctx.write(string.upper(chunk))
end))
```

Each entrypoint is limited to 1 MiB and its captured source changes only after
reload. Required helper modules are read at invocation time, so keep package
files stable during an invocation.

## C embedding and ownership

Include `<cai/plugins.h>`. Initialize `cai_plugins_config`, set roots, open a
catalog and load/reload packages. `cai_plugins_define` installs native handlers;
`cai_plugins_register_executor` adds a host factory for another language.
Handlers borrow streaming sources/sinks until invocation returns. Handler
ownership transfers on successful define only; destruction waits for every
bound invocation or agent generation to release it. Callbacks execute outside
the catalog mutex and may reload tools themselves.

Set `cai_agent_runtime_config.plugins` or `cai_app_config.plugins` before open.
For a basic agent, call `cai_agent_use_plugins` after native registration and
before creating/using sessions. The agent retains the catalog; caller-supplied
executor contexts and callbacks must outlive all attached agents/runtimes.
Application host catalogs replace file discovery, and remain host-owned.
Plugins are not inherited by subagents and are rejected in isolated review;
restricted diagnostic requests do not advertise them.

A response uses the tool generation advertised in its request. Even a reload
from its response-completion callback cannot replace handlers for calls it
already returned. The next request, including continuation within the same
turn, sees the new definitions. An in-flight call finishes with its old handler.

For embedded Lua, include `<cai/lua_tools.h>`, initialize/open an adapter and
attach it to the catalog **before loading Lua packages**. Close agents first,
then the catalog, then the adapter. The optional CMake package is
`find_package(cai_lua_tools CONFIG REQUIRED)`, with `cai::lua_tools_static` and
`cai::lua_tools`; pkg-config is `cai-lua-tools`. `CAI_BUILD_LUA_TOOLS=OFF`
removes it entirely. Core `find_package(cai)` does not discover or link Lua.

Vectis can supply an existing `lua_State` and a synchronous owner-thread
`dispatch` callback to the adapter. `cai_lua_tools_define` retains a trusted
function already on that VM's stack. Dispatch must execute and finish the
operation before returning, including cancellation. The adapter never closes
the borrowed VM or replaces its libraries. Retired functions are released on
its owner thread; close a borrowed-state adapter on that thread.

## Lua facade

`cai.plugins({absolute_roots...})` returns a receiver with `load`, `reload`,
`unload`, `list`, `run` and `close`. `run(name, source, sink [, workspace, yolo])`
uses the facade's normal string/stream source and sink callbacks.
`agent:use_plugins(catalog [, workspace, yolo])` attaches before sessions;
runtime constructors accept `plugins = catalog`. Attached users keep the
receiver alive and explicit close refuses while they remain attached.

When the optional adapter is built, `catalog:define(metadata, function)`
defines trusted functions for synchronous agents/direct calls. Metadata fields
are `name`, `description` and `parameters_json`. Native worker runtimes accept
file packages but reject these host Lua functions without a C dispatcher;
this prevents accidental calls into the Lua VM from the wrong thread. Vectis
uses the public C adapter dispatch contract for that integration.

Process definitions retain metadata and executable paths, not copies of process
files or libraries. Keep those files stable during calls. Lua instruction hooks
cannot preempt a blocking C library function or host dispatcher; trusted host
implementations must cooperate with cancellation.
