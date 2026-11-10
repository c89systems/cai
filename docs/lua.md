# Lua SDK

The Lua binding targets Lua 5.5 and is exposed as `require("cai")`.
It wraps the public C workflow facade: clients, agents, sessions, responses/outputs, streaming sinks, tool registries, tool presets, ChatGPT auth/login helpers, and the MCP Streamable HTTP handler and client.
Lua Smith runtimes also accept `chrome = { socket_path = path }` or `chrome = { socket_directory = dir }`, with optional timeout/frame limits.
They own a lazy connection until runtime close; see [Chrome integration](chrome.md).
Lua runtime tables also accept all `disable_*` policies and `enable_todo`, `enable_searxng`, `enable_revgeo` with their corresponding tool configuration tables.
`todo.store` may be a native store; the runtime borrows it until close.
`runtime:tools()` returns the installed inventory as rows with `name`, `description`, `input_format`, and `source` (`local`, `hosted` or `mcp`).
MCP rows also identify their client with a one-based `mcp_client_index`.
This wraps the owner-thread C API `cai_agent_runtime_visit_tools`; inspection performs no network discovery or tool execution and is available while a turn runs.

`runtime:instruction_status()` and `agent:instruction_status()` report `global_agents_md_loaded`, `codex_agents_md_enabled`, `codex_agents_md_loaded` and `repository_agents_md_count`.
The C equivalents are `cai_agent_runtime_get_instruction_status` and `cai_agent_get_instruction_status`.
These describe nonempty files actually captured in the developer context, not filesystem existence.
`/status` shows this evidence too.
Opening a resumed session reads the current startup configuration and instruction files again; editing files while CAI runs requires restarting or reopening the runtime.

Build and run the local LuaRock test:

```sh
make lua-test
```

The LuaRock depends on the `lonejson` Lua rock and links against an installed `libcai` discovered through `pkg-config cai`; the local test target installs the current debug build into `build/luarocks/cai-prefix` first.
`lonejson` 0.47.0 is not assumed to exist on LuaRocks.org: `make lua-rock` installs it from the official release source rock at `https://github.com/c89systems/lonejson/releases/download/v0.47.0/lonejson-0.47.0-1.src.rock` when needed.
The binding pins the matching Lua rock because lonejson 0.47.0 changes receiver layouts to ABI 27; rebuild native consumers and Lua modules together.
Lua projects can use the rock as a facade over the C library, while Vectis can still call the C API directly where that fits its performance and integration needs better.

The released rockspec uses the public GitHub release URL for its source archive.
The source rock embeds that same archive, so unpacking it needs no source download.

For manual LuaRock installation, install the matching lonejson source rock first because this version is served from the GitHub release, not LuaRocks.org:

```sh
luarocks install https://github.com/c89systems/lonejson/releases/download/v0.47.0/lonejson-0.47.0-1.src.rock
luarocks install cai-<version>-1.src.rock
```

Lua APIs that accept large text or file payloads have `*_spooled` variants.
Those accept strings, chunk-producing callbacks, or lonejson-style spooled readers with `rewind()` and `read(n)`.
Lua raw spooled tool callbacks receive that same reader shape for arguments, and may return a chunk-producing callback or spooled reader to stream JSON output without building the full value first.

The Lua facade intentionally does not expose C-only embedding surfaces such as custom allocators, `FILE *`, `lc_source` / `lc_sink`, raw `cai_source` / `cai_sink` constructors, or lonejson C maps.
A Lua host written in C can expose any current CAI persistent backend through one constructor:

```lua
local backend = assert(cai.native_store(kind, callbacks, context))
```

`kind` is `"todo"` for a `cai_todo_store_callbacks` pointer plus its opaque context, `"mcp_session"` for a `cai_mcp_session_callbacks` pointer plus its opaque context, `"skills"` for a complete `cai_skill_provider` pointer, or `"agent_session"` for a complete `cai_agent_session_store` pointer (which already contains its context; pass no third argument).
`"blob"` accepts a `cai_blob_store` pointer (which already contains its context; pass no third argument) and is used as `storage` for ChatGPT auth/login or as `global_instruction_store` for Smith.
The embedding host supplies those pointers as lightuserdata; CAI copies the callback table before returning the Lua userdata.
Pass a skills handle as `skill_provider` (or set `skills_directory`) on a Smith/custom agent configuration; it supplies the global catalog and constrained `read_skill` tool without exposing the host's config directory as a workspace file root.
The host must keep the opaque context valid for the handle's documented lifetime; the lightuserdata itself is only an embedding boundary and must not be supplied by untrusted Lua code.

Pass the handle as `store` to `register_todo_tool`, as `session` to `cai.mcp_handler`, or as `session_store` to `new_smith_runtime`.
Handles are typed and cannot be used on the wrong surface.
Todo and MCP handles transfer to their registered owner and call the native `destroy` / `cleanup` hook exactly once.
Agent-session stores remain owned by the native host; CAI pins their Lua handle until the Smith runtime has closed.
No native-store callback enters a `lua_State`.
Pure Lua continues to use `store_path` / `lock_path` for todo and may use the existing Lua callback table for MCP sessions.

The Lua examples include a basic streaming agent, a terminal chatbot with SearXNG and todo/kanban tools, streamed tool output, low-level session handling, and session state save/restore.
See [examples/README.md](https://github.com/c89systems/cai/blob/trunk/examples/README.md).

## Application integration

Use [the Lua owner](lua-workspace-owner.md) for a terminal-free backend and [the Lua workspace client](lua-workspace-client.md) for attachment. [Embedding CAI](embedding.md#lua-55) covers the synchronous `cai.app` composition.
