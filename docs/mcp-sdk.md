# MCP SDK

This page covers the framework-neutral C/Lua serving and client APIs.
For application connections, ChatGPT apps and lazy tool discovery, see [MCP configuration](mcp.md).

## Serving tools

`<cai/mcp.h>` exposes a transport-neutral MCP Streamable HTTP route handler for serving a `cai_tool_registry` through MCP without making cai an HTTP server.
The host framework, for example Vectis/Kore, owns routing, TLS, auth, timeouts, and connection lifecycle. cai receives a streaming request body, reads logical headers through a callback, writes logical response headers through a callback, and streams the JSON-RPC response into a caller-provided sink.

First implemented scope:

- POST JSON-RPC requests with JSON replies by default.
- POST JSON-RPC requests can also reply as a single SSE event when the client
  asks for `Accept: text/event-stream` without `application/json`.
- GET `/mcp` returns an SSE stream endpoint for server-to-client transport
  compatibility. cai currently emits a lightweight SSE comment heartbeat and
  closes; server-initiated JSON-RPC notifications/requests are not implemented
  yet.
- `initialize`
- `notifications/initialized`
- `ping`
- `tools/list`
- `tools/call`
- optional stateful `Mcp-Session-Id` lifecycle through host-owned callbacks

The request body is a `cai_source` and the response body is a `cai_sink`.
The JSON-RPC envelope is parsed with lonejson while `id`, `params`, and `params.arguments` are validated into spooled JSON values.
Typed tool arguments are parsed from that spool.
Successful `tools/call` results are bounded by `tool_output_max_bytes`, defaulting to `CAI_MCP_DEFAULT_TOOL_OUTPUT_MAX_BYTES`, and cai intentionally uses bounded spooling for that tool-call output so it can fail closed before committing a partial JSON-RPC response.
Set `tool_output_max_bytes` to `CAI_MCP_TOOL_OUTPUT_UNLIMITED` only when the embedding route deliberately accepts direct unbounded tool-output streaming.
`GET`/SSE transport is implemented for compatibility, but cai does not yet provide a host callback surface for queued server-initiated MCP notifications or requests in the handler.
The handler currently serves tools; resources, prompts, completions, generic client requests, and server-to-client notification observation belong to the MCP client surface.
Roots, sampling, elicitation, and resource subscriptions are not implemented.

MCP session persistence is storage-neutral.
The zero/default MCP config is stateless.
Set `config.enable_sessions = 1` and provide `cai_mcp_session_callbacks` to let the host create, load, save, and destroy protocol sessions. cai returns `Mcp-Session-Id` on `initialize`, loads and saves session state for later requests carrying that header, marks the session initialized on `notifications/initialized`, and calls `destroy` for `DELETE` requests with a session id.
Tool state remains tool-owned; MCP session state only tracks protocol lifecycle and client metadata.

Minimal route adapter shape:

```c
cai_mcp_http_request req;
cai_mcp_http_response res;

req.method = "POST";
req.body = request_body_source;
req.header = my_header_get;
req.header_context = route;

res.body = response_body_sink;
res.set_header = my_header_set;
res.header_context = route;

cai_mcp_handler_handle_http(handler, &req, &res, &error);
```

The handler asks for headers by lowercase logical names such as `content-type`, `accept`, `origin`, `mcp-protocol-version`, and `mcp-session-id`; adapters should normalize however their HTTP stack stores header names. cai sets response headers such as `content-type`, `mcp-protocol-version`, `cache-control`, and `x-content-type-options` through the supplied callback.

By default, requests without an `Origin` header are accepted and requests with a non-empty `Origin` header are rejected unless the origin is present in `cai_mcp_handler_config.allowed_origins`.
Set `disable_origin_validation` only when the embedding server handles equivalent browser-origin protection itself.

The test build also provides `cai_mcp_http_server`, a tiny plain HTTP utility for serving the MCP handler on `/mcp`.
It is test/example infrastructure only and is not linked into `libcai`.
It binds `127.0.0.1`, defaults to port `18765` for manual runs, and supports `--port 0 --print-port` for conflict-free tests.
Headers are parsed in memory, while request bodies remain socket-backed `cai_source` streams pulled by the MCP handler.

An opt-in MCP Inspector e2e test validates the same server through the official Inspector container image:

```sh
CAI_MCP_INSPECTOR_E2E=1 make mcp-inspector-e2e
```

The test uses `ghcr.io/modelcontextprotocol/inspector:1.0.1` through rootless Podman Kube and covers `tools/list` and `tools/call` over Streamable HTTP.
It is skipped by default so normal local test runs do not depend on container image availability.
Override the image with `CAI_MCP_INSPECTOR_IMAGE`.
Its container is declared in `devenv.yaml.in` and rendered only for the explicit opt-in.
Pasta forwards only the local test fixture port into the pod; host networking is not used.

The example MCP server can also be attached to an agent client for manual end-to-end testing of the production tool presets.
Start it with an isolated todo store:

```sh
cmake --build --preset debug --target cai_example_mcp_server
tmpdir=$(mktemp -d)
CAI_MCP_EXAMPLE_TODO_STORE="$tmpdir/todo.json" \
CAI_MCP_EXAMPLE_TODO_LOCK="$tmpdir/todo.lock" \
./build/debug/cai_example_mcp_server --port 18766
```

Then add the Streamable HTTP endpoint to Codex or Claude Code:

```sh
codex mcp add caiTodo --url http://127.0.0.1:18766/mcp
claude mcp add --transport http caiTodo http://127.0.0.1:18766/mcp
```

See [examples/mcp-server/README.md](https://github.com/c89systems/cai/blob/trunk/examples/mcp-server/README.md) for the equivalent config-file snippets and a todo-kanban workflow prompt that checks tool discovery, help text, WIP-limit denial, completion, and board state.

## Connecting to servers

`<cai/mcp.h>` also exposes a transport-independent `cai_mcp_client` interface that cai consumes through receiver-style function pointers.
The built-in Streamable HTTP implementation can initialize sessions, ping, cache and call tools, cache and read resources, cache resource templates, cache and get prompts, complete prompt/resource arguments, send generic JSON-RPC requests/notifications, and observe server logging, resource-updated, progress, cancellation, and list-changed notifications on response streams.

The HTTP client config owns transport details: endpoint URL, client identity, protocol version, timeout, TLS verification behavior, optional CA bundle/path, receiver callbacks, and allocator.
When CA bundle/path are unset, libcurl/OpenSSL resolve their default trust store.
`discovery_timeout_ms` optionally bounds a complete tools refresh, including initialization and pagination.
Zero preserves per-request timeouts.
A positive budget does not change subsequent tool-call timeouts.
Lua `cai.mcp_client` accepts the same field.

`cai_mcp_streamable_http_client_config.auth_provider` accepts a copied native `cai_mcp_client_auth_provider` descriptor whose borrowed context outlives the connection.
Its `headers` callback supplies credentials for each request, and optional `refresh` renews them after HTTP 401.
Rewindable JSON-RPC requests retry once; raw uploads are not replayed.
Headers are validated and cannot override transport-owned headers.
Use HTTPS, or loopback HTTP for local development.
This interface is independent of inference and reusable by other providers.
`cai_mcp_chatgpt_auth_provider_init` adapts a borrowed `cai_chatgpt_auth` with Bearer authentication, optional account routing, product SKU and originator.
For this gateway, also set `allow_notification_204 = 1`: it acknowledges notifications with an empty HTTP 204 instead of MCP's 202.
Other connections retain strict validation by default. libcai_app and Lua's `chatgpt_auth` constructor select this compatibility setting automatically.

Lua `cai.mcp_client({chatgpt_auth = auth})` uses the same native adapter and defaults to the ChatGPT app gateway when `url` is omitted.
It pins the auth userdata until the client closes.
Lua callbacks are not accepted as native credential providers.
`auth:account_id()` returns the optional account route.

`cai_agent_runtime_refresh_mcp()` (Lua `runtime:refresh_mcp()`) schedules an atomic replacement of attached MCP tools on the runtime worker.
Existing non-MCP tools remain installed; preparation failure preserves the old registry.
`CAI_AGENT_EVENT_MCP_UPDATED` reports completion without adding model context.
Runtime clients default to lazy schema exposure through the built-in `tool_search` function on both Responses and Chat Completions.
An optional `cai_agent_runtime_config.mcp_options` array, parallel to `mcp_clients`, provides source names/descriptions and per-connection `eager = 1` opt-in.
The runtime copies these settings and the complete registration policy at open; client objects remain borrowed.
Tool visitors report `deferred` for schemas not yet selected.
Search results and subsequent MCP calls use the ordinary journal, replay and tool-result presentation paths.

Lua takes the same parallel array:

```lua
mcp_clients = { apps, documents },
mcp_options = {
  { name = "ChatGPT apps" }, -- lazy, connector hints from the gateway
  { name = "Documents", description = "Search workspace files", eager = true },
},
```

Clients remain borrowed for the runtime's lifetime; hosts serialize access to shared clients, including credential callbacks, across attached runtimes.

Remote MCP tools can be registered into a local `cai_tool_registry` with `cai_mcp_client_register_tools`.
The registry keeps a non-owning client pointer, so callers must keep the MCP client alive for as long as those registered tools can run.
Names that exceed the provider's 64-byte limit or contain unsupported characters receive stable aliases without changing the remote call name.
`cai_mcp_tool_alias(prefix, remote_name, out, error)` (Lua `cai.mcp_tool_alias(prefix, remote_name)`) computes the same alias used by the registry and application; ordinary valid prefix+name combinations are retained.

### Raw input for MCP tools

libcai bridges model-facing Responses `custom` tools to ordinary MCP JSON arguments.
A server advertises the string field under the vendor metadata key `systems.c89.cai/raw-input`; this is a libcai extension, not a standard MCP custom-tool type.
Other clients continue to use the original object schema.

For an ordinary JSON tool, attach a copied descriptor after registration:

```c
cai_tool_raw_input raw;
memset(&raw, 0, sizeof(raw));
raw.field = "patch";
cai_tool_registry_set_raw_input(registry, "apply_patch", &raw, &error);
```

The MCP handler adds this hint to `tools/list`:

```json
{
  "_meta": {
    "systems.c89.cai/raw-input": { "field": "patch", "framing": "text" }
  }
}
```

Native custom tools automatically export an ordinary object schema with a required string `input` property and the corresponding hint.
The server unwraps that argument before invoking the raw callback.
Custom registration and `cai_response_create_params_add_custom_tool` accept a NULL format for unconstrained text; a supplied grammar must specify `grammar`, `lark` or `regex`, and a nonempty definition.
The same format is included in the hint.
Use spooled callbacks when input can contain embedded NULs.

`cai_mcp_tool_registration_config.raw_input_policy` defaults to `CAI_MCP_RAW_INPUT_AUTO`: use server hints when the MCP client implements `call_tool_raw`.
`CAI_MCP_RAW_INPUT_JSON` keeps the original function schema.
`CAI_MCP_RAW_INPUT_CUSTOM` requires raw-upload support for mapped tools.
`cai_agent_register_mcp_client_tools` automatically selects JSON for Chat Completions agents; explicitly requesting CUSTOM there returns an error.
When registering directly into a standalone registry for a Chat Completions provider, select JSON yourself.

Per-tool `raw_input_overrides` take precedence over server hints.
Each entry names the remote tool before `name_prefix` and supplies a descriptor; a NULL mapping disables the hint.
Unknown or duplicate override names are errors.
Mappings must select a declared top-level string property.
Registration copies configuration strings and the selected mapping before refreshing discovery, so borrowed cached metadata can be used as overrides.
It preserves pre-existing tools on failure and rolls back all additions from the failed registration.
Custom MCP client implementations can supply `cai_mcp_client_tool.input_schema_json` for full schema validation.

Plain `text` framing sends the entire input as the selected field.
If other arguments are required, use `CAI_TOOL_RAW_INPUT_JSON_HEADER` (`json-header` in metadata): one JSON object on the first line, LF, then exact raw content.
The header must omit the raw field, contain the other required arguments, have unique keys, and fit within 16 KiB including the LF.
For example, mapping `content` with this framing accepts:

```text
{"path":"src/example.c"}
#include <stdio.h>
int main(void) { puts("hello"); }
```

Header/body instructions and the ordinary argument schema are added to the model-facing tool description.
A supplied grammar constrains the entire framed input.
The remote tool retains responsibility for its normal argument schema constraints; libcai validates the mapping and header structure.

The HTTP adapter uses a pull generator and chunked requests to escape input incrementally, preserving quotes, backslashes, Unicode, embedded NULs, and trailing newlines without staging a complete serialized request.
The embedding HTTP server must support chunked request bodies.
Completed model input and server callback dispatch retain the existing **spooled** lifecycle: this does not execute a tool while the model is still producing its input.
HTTP session recovery rewinds and re-encodes the same completed input.

Malformed input is rejected with `CAI_ERR_INVALID`; source read/rewind failures and dropped connections use `CAI_ERR_TRANSPORT`.
Raw calls log the tool name, status, and error details through pslog, without logging raw payloads.
A failed upload or response does not imply the remote tool had no side effects.
Only HTTP 404 for an expired session triggers one automatic replay; other failures are returned to the caller.
Turn cancellation reaches the HTTP worker, returns `CAI_ERR_CANCELLED`, and never replays the cancelled request.
Cancellation is logged at debug level and does not undo remote side effects.

Deterministic tests cover buffered and streamed agent-to-MCP round trips, JSON-header framing, model continuation after tool errors, Chat Completions' JSON fallback, metadata and policy validation, atomic registration rollback, byte-split Unicode and embedded NULs, expired-session recovery, JSON/SSE remote errors, response ID mismatches, malformed responses, disconnects, HTTP errors, output-sink failures followed by a successful call, borrowed metadata across discovery refresh, oversized reader results, and cancellation before and during HTTP work with successful client reuse.
Run the C cases with `ctest --test-dir build/debug --output-on-failure -j1 -R mcp_raw_input`.
`make lua-test` also exercises the real libcai HTTP server with large raw uploads, malformed headers and UTF-8, failing Lua readers/writers, retained client lifetime, configuration mutation and collection inside input callbacks, and recovery after failures.

The equivalent Lua interfaces are:

```lua
registry:set_raw_input("apply_patch", { field = "patch" })
client:register_tools(registry, {
  raw_input_policy = "auto", -- "json" or "custom"
  raw_input_overrides = {
    { name = "write_file", mapping = { field = "content", framing = "json-header" } },
    { name = "another_tool", mapping = false },
  },
})
local result = client:call_tool_raw("apply_patch", { field = "patch" }, raw_patch)
```

Lua `call_tool_raw` accepts text or the existing spooled-input interfaces and an optional fifth output sink argument.
Discovered tool tables expose `raw_input` and `input_schema_json`.
Lua input is collected into the explicit input spool; HTTP serialization does not collect a second complete request.

Result-producing MCP client methods stream the JSON-RPC `result` value to the caller sink as it is parsed from JSON or SSE response chunks once the response id has been validated.
Server notifications and requests that arrive before the final response are handled before result streaming starts.
Invalid pre-result envelopes and server errors do not write partial result JSON; invalid data after streamed result bytes can still make the call fail after the sink has observed those bytes.
