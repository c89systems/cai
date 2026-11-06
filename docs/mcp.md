# MCP connections and ChatGPT apps

Add a named Streamable HTTP server to `config.ini`:

```ini
[mcp.example]
enabled = true
eager = false
description = Workspace document search and editing
endpoint = https://example.com/mcp
api_key_env = EXAMPLE_MCP_API_KEY
discovery_timeout_ms = 3000
timeout_ms = 30000
```

Tool loading is lazy by default, including ChatGPT apps.
CAI fetches and caches the catalog on its worker, but does not send those tool schemas to inference until the model calls `tool_search`.
That portable function searches names, descriptions, source hints and parameter names with local BM25 ranking.
It returns up to eight matches by default (maximum 32), installs their schemas for the next request, and makes no discovery network call.
Results contain names, source identities and short descriptions; complete schemas appear once in the next request's tool list.
Source names and short descriptions are included in the discovery tool so the model can find apps without already knowing their tool names.
Provider connector metadata supplies app hints; ordinary servers can use `description` above.

Set `eager = true` on an individual connection to expose all its tools from startup.
Selected lazy tools remain exposed for that runtime; a new runtime starts with deferred schemas again.
Refresh preserves selected aliases that still exist, while disabling a connection removes its tools from discovery and inference.
`/tools` marks unselected tools as deferred; `/mcp` shows the connection's lazy/eager mode and available catalog size.

The endpoint is required for enabled servers without a provider default.
`api_key_env` is optional; when set, its environment variable supplies a Bearer token.
`[env]` can supply that variable under the normal configuration precedence rules.
Server names accept ASCII letters, digits, underscores and hyphens.
Tools are exposed as `mcp_7_example_TOOL`: the number is the server name's byte length, making the server/tool boundary unambiguous even with underscores in either name.
The complete alias fits 64 bytes.
Longer names and names containing other characters receive stable `mcp_h…_t…` aliases; calls retain the exact remote name.
The existing raw-input bridge honors server metadata for Responses and supplies ordinary JSON function arguments for Chat Completions.

Discovery runs on the runtime worker when the first runtime opens, keeping the editor responsive.
Each server's default three-second budget covers initialization and all tools-list pages together; normal tool calls use the separate `timeout_ms`.
Transport, authentication or catalog failures warn and leave that server unavailable while other tools remain usable.
Catalogs are cached for this invocation, including across session switches.
`/mcp` lists connections as a Markdown table; `/mcp NAME` shows details.
`/mcp enable NAME`, `/mcp disable NAME` and `/mcp refresh NAME` update the catalog at the next request boundary, or while idle.
Omit NAME or use `all` for every connection.
These commands use Softline's editable queue and never write `config.ini`.
Their changes last for this application invocation, including `/new` and `/resume`.
Utility commands and isolated `--review` runs do not connect MCP servers.
`[agent] mcp = false`, `CAI_MCP=false` or `--no-mcp` disables configured servers; `--mcp` explicitly enables them.
The generated template contains a commented `[mcp.example]` section to copy.

Authenticated ChatGPT accounts also expose their connected apps through the `chatgpt` connection, enabled by default.
This connection uses the account's saved ChatGPT authentication independently of the inference provider: changing `--provider` does not disable its app tools or send its credentials to inference.
Only already-connected apps are discovered; CAI does not install apps or start connector authorization.
Missing or expired authentication leaves the connection unavailable while other inference and tools continue.
Log in, then refresh it.
An explicit connection can override the endpoint or default enabled state:

```ini
[mcp.chatgpt]
provider = chatgpt
enabled = true
# endpoint defaults to https://chatgpt.com/backend-api/ps/mcp
```

`provider = chatgpt` can also authenticate a differently named connection.
Provider credentials and `api_key_env` are mutually exclusive.
The app's TLS trust settings apply to MCP connections too.
`/mcp disable chatgpt` disables it for this invocation; `enabled = false` persists that choice in configuration.

## SDK integration

The MCP client, provider authentication, tool search and runtime dispatch belong to core `libcai`; they are not restricted to the terminal application.
See [MCP SDK](mcp-sdk.md) for embedders and [dynamic tools](plugins.md) for local tool packages.
