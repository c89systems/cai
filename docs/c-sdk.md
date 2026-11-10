# C SDK

Core `libcai` supplies clients, agents, sessions, streaming, tools, MCP and the host-neutral agent runtime.
It has no Softline, libmdf or liblockdc dependency.
See [application embedding](embedding.md) when you want the complete application, or [the agent runtime](runtime.md) when you own the host loop.
Public headers document ownership and callback lifetimes.

## Linking

Installed cai archives include both CMake package metadata and pkg-config metadata.
They are meant to keep consumer build files small and avoid manual include/library path plumbing.

With pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs cai)
```

With CMake:

```cmake
find_package(cai CONFIG REQUIRED)
add_executable(app app.c)
target_link_libraries(app PRIVATE cai::cai_shared)
```

The metadata records the dependency mode used to build cai.
In the default `cpkt` mode it points at the matching official `cpkt` release URL and checksum for the native curl/OpenSSL stack and the matching official lonejson release URL/checksum.
The CMake package models lonejson as an external dependency: `find_package(cai)` requires `liblonejson` and its `curl`, `oidc`, and `openssl` component package surfaces to be discoverable and adds lonejson headers when they are available, but cai release archives do not vendor lonejson or curl runtime libraries.
The discovered host `liblonejson` must also expose cai's expected ABI generation.
Shared-only consumers that only use `<cai/cai.h>` can configure without lonejson development headers; consumers that include lonejson-backed cai headers or use static linking still need the lonejson development package.
The pkg-config file exposes lonejson as a public requirement and libcurl 7.86.0 or newer as a private link requirement.
`pslog.h`/`libpslog` is only needed by source builds, logging integrations that construct pslog loggers directly, and the logging-enabled MCP and Smith terminal examples. cai release archives do not compile in single-header dependency variants.

## Clients, agents and sessions

The preferred high-level API is method-style handles:

```c
cai_client_open(&client_config, &client, &error);
agent_config.prompt_cache_key = "my-app:assistant:v1";
client->new_agent(client, &agent_config, &agent, &error);

agent->add_user_text(agent, "Explain epoll in one paragraph.", &error);
agent->stream_text(agent, sink, &error);

agent->close(agent);
client->close(client);
```

The receiver form is the primary style for instantiated cai handles that expose method tables.
`cai_client` includes the full client-owned Responses, streaming, input-token counting, response lifecycle, provider sessions, agent, and usage operations.
`cai_agent`, `cai_session`, and `cai_tool_schema` follow the same convention.
The matching `cai_*` free functions remain part of the public API for compatibility, callback wiring, and places where a value is still an opaque builder or result handle.

Agent-level calls lazily create a default session and reuse it for follow-up turns, so simple chat and workflow drivers get Responses context continuity without manually carrying a `cai_session`.
Explicit sessions remain available for multi-session workflows and OpenAI provider session handles.

Large user text can use `add_user_text_source` or `add_user_text_spooled` instead of building a temporary C string.
Source inputs are consumed when added and stored as spooled pending input; request retries then clone that spool, so retry behavior is the same as for small string input.

User file input is available at the same facade level.
Use `add_user_file_path` for a local path, `add_user_file_source` for a `cai_source`, or `add_user_file_data_spooled` when the caller already owns a `lonejson_spooled` value.
Source/path file inputs are consumed when added and stored as spooled pending input; request retries then clone that spool, so a failed request does not consume the queued file input.

Process restart/resume has two separate pieces by design.
To continue inference against OpenAI-held Responses context, persist either `session->previous_response_id(session)` or `session->conversation_id(session)` and restore it into a new session with `session->set_previous_response_id` or `session->set_conversation_id`.
That is the actual server-side continuation handle.
Opt-in local history export/import is for offline inspection, host-owned compaction, and replay state; by itself it is not a substitute for the OpenAI continuation id.

For applications that want one file/object to persist, use `cai_session_export_state_source` and `cai_session_import_state_source`.
The state envelope is versioned JSON and contains the active continuation handle, the last completed response usage, plus local history when `enable_local_history` is on.
Import restores the continuation handle and last usage in all sessions and restores local history only for local-history-enabled sessions.
`cai_session_save_state_path` and `cai_session_load_state_path` are the file-backed convenience helpers around the same source/sink state envelope; the primary handle style is `session->save_state_path` and `session->load_state_path`.

Set `prompt_cache_key` on `cai_agent_config` when an agent has a stable prompt prefix, large developer instructions, or stable tool schemas. cai sends it on every Responses request created by that agent so OpenAI can bucket similar requests for prompt caching.
The default is unset; cai does not generate random cache keys because that would fragment the cache.

The high-level agent facade uses `developer_instructions`, matching the current Responses API guidance around application-provided behavior.
OpenAI documents the top-level `instructions` field as high-level instructions that take priority over `input`, and shows it as roughly equivalent to a `developer` message. cai therefore treats these as developer instructions in its DX surface.

The agent/session facade does not expose a normal `system` prompt layer.
Root instructions are OpenAI/model policy and are not API-settable.
The Responses wire format still has low-level message roles, but normal cai users should set `developer_instructions` on the agent and add user input with `session->add_user_text` or `session->add_user_image_url`.

`assistant` is not an instruction surface.
In the Responses API it represents model-generated messages, and it is mainly useful when manually reconstructing session history. CAI default sessions use `previous_response_id`. Sessions bound to an OpenAI provider session use its handle, so assistant turns are preserved through those handles instead of being manually appended by application code.

Structured output text can be decoded into a caller-owned lonejson struct with `cai_output_write_json`.
The function parses `cai_output_text(output)` as JSON; it does not inspect arbitrary response metadata or raw response JSON.
For non-text output items such as hosted-tool, image, code, or future item types, use `cai_response_output_items_json` for a materialized JSON array or `cai_response_write_output_items_json` to stream the preserved output item array into a sink.
The per-item accessors expose common metadata fields, while the preserved JSON is the compatibility path for item variants cai does not type yet.

Tools are agent capabilities.
The default `register_tool` path is a typed lonejson callback; `register_raw_tool` is the full-string JSON escape hatch; `register_raw_spooled_tool` is the raw JSON path for large arguments.
The typed path uses a lonejson map for parameters and a second lonejson map for the result.
JSON Schema is derived from the parameter map automatically, and `_REQ` fields become required schema fields:

```c
typedef struct lookup_customer_params {
  char *customer_id;
  long long limit;
} lookup_customer_params;

typedef struct lookup_customer_result {
  char *summary;
} lookup_customer_result;

int lookup_customer(void *ctx, const void *params, void *result,
                    cai_error *error) {
  const lookup_customer_params *in = params;
  lookup_customer_result *out = result;

  out->summary = cai_tool_result_strdup("customer found", error);
  return out->summary != NULL ? CAI_OK : CAI_ERR_NOMEM;
}

agent->register_tool(agent, "lookup_customer", "Look up a customer.",
                     &lookup_customer_params_map,
                     &lookup_customer_result_map, lookup_customer, ctx,
                     &error);
```

OpenAI-hosted tools are different from local callback tools: OpenAI executes them, so cai should not register a local callback for them.
Use `cai_response_create_params_add_simple_hosted_tool` or `cai_response_create_params_add_hosted_tool_json` on low-level Responses params, or `agent->add_simple_hosted_tool` / `agent->add_hosted_tool_json` on the facade.
The raw JSON hosted-tool path is intentionally the primary surface for `mcp`, `file_search`, `code_interpreter`, `computer_use_preview`, `image_generation`, `tool_search`, and future hosted tools, because their schemas evolve faster than a C SDK should hard-code. cai validates the supplied tool JSON with lonejson and streams it into the request body as a JSON value.
For remote MCP hosted tools, `cai_hosted_mcp_tool_config` is only a request helper for server identity and exposure policy.
Omit `allowed_tools` to expose all remote tools from that MCP server, or provide `allowed_tool_names` / `allowed_tools_json` to expose only named tools.
`require_approval_json`, `headers_json`, and the raw hosted-tool JSON path remain pass-through so cai does not mirror or freeze remote tool schemas.

For large typed tool results, callbacks can put source-backed or spooled fields into their result struct instead of building a raw JSON string.
Use `cai_tool_result_set_source_path` for lonejson source fields and `cai_tool_result_set_spooled` for lonejson spooled string/base64 fields; cai streams the resulting JSON into the tool output sink and then cleans up those handles.

`cai_run_options.tool_output_max_bytes` is a total output cap for tool auto-runs.
Leave it at zero for unlimited output, or set it when tool output is controlled by remote model decisions and should fail closed above a known size.

`cai_tool_schema_from_map` can be used when callers want to inspect or enrich the generated schema.
Metadata helpers such as `describe` update existing properties; they do not decide requiredness.
Requiredness belongs in the lonejson field map so the C decoder and OpenAI schema cannot drift.

```c
cai_tool_schema_from_map(&lookup_customer_params_map, &schema, &error);
schema->describe(schema, "customer_id", "Customer id", &error);
```

Tool arguments and tool results are hostile boundaries.
The typed tool path validates argument JSON before callbacks run, rejects unknown fields against the lonejson map, relies on lonejson for required/type/duplicate-key failures, and serializes callback results as JSON values instead of raw text concatenation.
This prevents common prompt-injection payloads from smuggling extra fields into typed callbacks or escaping JSON string boundaries on the way back to the model.
`register_raw_tool` deliberately bypasses the typed result API and passes arguments as a full C string, so it is a convenience/materialized escape hatch.
Use `register_raw_spooled_tool` or `cai_tool_registry_register_raw_spooled` when raw JSON arguments may be large; cai validates the spooled value as JSON and passes the spool through without rebuilding a whole argument string.

For streamed auto-run tool calls, `cai_tool_event.arguments_json` remains a compatibility string for logging and simple handlers, while `arguments_json_spooled` exposes the same validated arguments as a `lonejson_spooled` handle. cai uses that spooled path for tool execution when function-call arguments arrive through the streaming capture path.

## Authentication and transport configuration

- `cai_client_open` resolves API keys only from explicit `config.api_key` or
  `getenv(config.api_key_env)`. If `api_key_env` is NULL, cai uses
  `OPENAI_API_KEY`. It does not implicitly load dotenv files.
- ChatGPT subscription auth can be supplied explicitly through
  `cai_chatgpt_auth` from `cai/auth.h`. Open a CAI-owned, Codex-wire-compatible
  `auth.json` path with `cai_chatgpt_auth_open`, set `config.chatgpt_auth`, and
  keep the auth handle alive for the client lifetime. If `auth_json_path` is
  NULL/empty, the auth library uses `$XDG_STATE_HOME/cai/auth.json`, or
  `$HOME/.local/state/cai/auth.json` when `XDG_STATE_HOME` is unset or
  relative. The C SDK's implicit auth default never selects Codex's auth
  storage; the `cai` CLI explicitly selects it by default. Run
  `make chatgpt-login` to create CAI's default auth file through browser OAuth.
  cai reads `id_token`, `access_token`, `refresh_token`, and `account_id` from
  the file, refreshes access tokens through the configured OAuth issuer before
  expiry, persists returned token fields, and retries one 401/403 response
  after a forced refresh. Before using a refresh token, cai re-reads the auth
  file so another process can refresh or replace same-account tokens without
  racing the stale in-memory handle; a different account is rejected. The auth
  file is never loaded implicitly by `cai_client_open`; callers must explicitly
  open auth and pass `config.chatgpt_auth`.
- Interactive ChatGPT login is exposed as a server-agnostic OAuth callback
  handler, not as a built-in webserver. Call `cai_chatgpt_login_start` with an
  auth file path or the library default and a redirect URI, open the returned
  authorization URL yourself or with `cai_chatgpt_login_open_browser`,
  pass the embedding server's callback request target into
  `cai_chatgpt_login_handle_callback`, then write the returned status,
  content-type, and body. The example `cai_example_chatgpt_login` shows this
  with a tiny local test server. The browser helper uses `open` on Darwin and
  `xdg-open` elsewhere by default, accepts a configured opener command, and
  never invokes a shell. Lua exposes the same flow with `cai.chatgpt_login`,
  `login:handle_callback`, `cai.chatgpt_login_open_browser`, and
  `cai.chatgpt_auth_default_path`; the Lua example uses LuaSocket only for its
  example listener.
- `CAI_DEFAULT_DOTENV_PATH` is `.env` for callers that explicitly want cai's
  dotenv parser. Call `cai_load_dotenv_api_key(path, env_name, &key, &error)`,
  pass the returned key to `config.api_key`, then release it with
  `cai_string_destroy` after `cai_client_open` has copied it.
  Dotenv contents are treated as untrusted input: cai does not execute or
  expand dotenv values, and rejects invalid variable names, overlong lines,
  unterminated quoted values, empty keys, and control characters in key values.
- OpenRouter can be selected with `cai_client_config_use_openrouter()`, which
  uses `OPENROUTER_API_KEY` and `https://openrouter.ai/api/v1`.
- TLS verification is configurable on the surfaces that perform outbound HTTP:
  `cai_client_config`, ChatGPT auth/login configs, and
  `cai_mcp_streamable_http_client_config` expose `insecure_skip_verify`,
  `ca_bundle_path`, and `ca_path`. Leave the CA paths unset to let
  libcurl/OpenSSL use their default trust store.
- Public config structs are zero-defaultable. A memset-zero config means "use
  cai defaults" for omitted knobs; fields are named so nonzero values opt into
  behavior or override a limit/path.
- Session continuity defaults to OpenAI-style server-side continuation.
  Client-side history replay is available for stateless compatible providers
  such as OpenRouter.
- The low-level Responses params surface exposes current request controls used
  by modern Responses workflows: background mode, store, service tier,
  truncation, metadata, include, prompt-template JSON, text verbosity,
  reasoning, structured text format, server-side compaction, local function
  tools, raw hosted tool objects, raw JSON `tool_choice`, and `max_tool_calls`.
  `cai_client_count_response_input_tokens` wraps
  `/responses/input_tokens` and sends a counting-safe request clone, because
  the live endpoint accepts input-shaping fields such as tools and
  `tool_choice` but rejects generation/execution limits such as
  `max_output_tokens` and `max_tool_calls`.

## Resource limits and continuation

- Non-streamed JSON responses are capped by `json_response_limit_bytes`
  (default 1 MiB). Buffered Chat Completions calls apply this limit during
  capture and to the complete constructed JSON, including escaping and usage
  fields; exceeding it returns `CAI_ERR_LIMIT`. Streaming calls are not capped
  by this setting. Tool output can spill to disk with
  `tool_output_memory_limit` and can be hard-capped per auto-run with
  `tool_output_max_bytes`.
- Clients and sessions can enforce cumulative token and estimated USD spend
  budgets with `cai_usage_limits`. Zero leaves a limit unset. Client limits are
  shared across all agents/sessions on that client; session limits are per
  session and can be set directly or inherited from
  `cai_agent_config.session_usage_limits`. If `max_spend_usd` is positive, the
  selected model must have CAI pricing metadata; verified free OpenRouter models
  are accepted as zero-cost, but unknown or incomplete model metadata fails
  closed instead of silently estimating `$0.00`. CAI estimates spend with
  bundled Standard API token prices and applies known long-context token prices
  for models whose pricing changes above a documented input-token threshold. A
  request that crosses a budget is charged, returns `CAI_ERR_LIMIT`, and later
  requests are rejected before transport until the limit is raised or disabled.
  Use `client->usage`, `session->usage`, or `session->close_with_usage` to
  report cumulative token usage and estimated USD spend. The matching
  `cai_client_usage`, `cai_session_usage`, and
  `cai_session_close_with_usage` free functions remain available:

  ```c
  cai_usage_limits limits;
  cai_usage_accounting spent;

  cai_usage_limits_init(&limits);
  limits.max_total_tokens = 100000;
  limits.max_spend_usd = 5.00;
  config.usage_limits = limits;

  /* ... run one or more sessions ... */
  session->usage(session, &spent, &error);
  ```

  Lua exposes the same surface with `usage_limits` on `cai.open`,
  `session_usage_limits` on `client:new_agent`, and `usage()` /
  `set_usage_limits()` methods on clients and sessions.
- The C and Lua terminal-chat examples set a default session
  `max_output_tokens` budget of `1,000,000` cumulative output tokens. Override
  any budget lane with `--max-input-tokens`, `--max-cached-input-tokens`,
  `--max-output-tokens`, `--max-reasoning-output-tokens`,
  `--max-total-tokens`, or `--max-spend-usd`; pass `0` to disable that lane.
- Production SDK calls should choose a model explicitly.
- Ordinary sessions use Responses context management. The agent runtime keeps
  local client history and compacts it before a known context limit. Responses
  uses the V2 `compaction_trigger` protocol. Chat Completions providers such as
  Berget use bounded client-side summaries, retaining recent turns; provider
  context-overflow errors also trigger one compaction and retry. A streamed
  `finish_reason=length` near the known context limit triggers compaction and
  continuation; an output-only cutoff continues without compaction.
  Reported cutoff usage is counted even when only reasoning or incomplete tool
  arguments were emitted. Session/client limits and goal token budgets apply
  before continuation; incomplete calls are never executed or added to history.
  Chat compaction summaries keep the selected service tier. Failed summaries
  still count reported usage while retaining the original history and error.
- Local spooled session history is opt-in with `enable_local_history`; default
  sessions rely on OpenAI server-side context through `previous_response_id` or
  provider session IDs. When enabled, `cai_session_export_history_source` streams the
  captured history as a JSON array for `cai_session_compact`, export, or
  offline inspection, and `cai_session_import_history_source` can load that
  JSON array back into a local-history-enabled session.
- Examples and integration development tests default to `gpt-5-nano`.
- OpenRouter development can use
  `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES`, currently
  `openai/gpt-5.6-luna`, because it passes cai's generic Responses,
  tool-calling, read-tool, streaming, and client-side continuity integration
  regressions through OpenRouter. cai's bundled metadata gives this OpenRouter
  route the same GPT-5.6 Luna context and pricing as the direct OpenAI model.
- OpenRouter tool-calling integration uses
  `CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES` by default. Free OpenRouter models
  and `openrouter/free` may still be useful for ad hoc development, but they are
  not the release-pinned compatibility model.

## Logging

`cai_client_config.logger` and `cai_agent_runtime_config.logger` accept a borrowed `pslog_logger *`.
CAI never owns or destroys either logger.
A runtime inherits its client logger when no runtime logger is supplied; set the relevant `logger_disabled` field to nonzero to suppress logging.

Current cai-owned log events cover client setup, runtime open/input/tool/ subagent lifecycle, OpenRouter server-continuity warnings, HTTP request start/completion, API error status responses, transport failures, and configured HTTP response-size limit failures.
API keys and normal request/response payloads are not logged; runtime logs likewise exclude user prompts, tool arguments, delegated instructions, and streamed model output.
HTTP request starts are trace-level, successful completions are debug-level, 4xx API responses are warn-level, and 5xx/transport failures are error-level.
Chat Completions also logs the final stream result (including HTTP status, model, error code, provider error detail, and request ID), so HTTP 200 inference errors, invalid streams, and failed callbacks remain visible.
Cancellation is debug-level; output cutoffs and compaction/continuation attempts are info-level.
Exhausted recovery attempts and failed compactions are warn-level.
Runtime compaction completion is logged after commit/rollback handling.
Model catalog source/cache selection is debug-level; failed refreshes and fallback to configured metadata are warn-level.
These diagnostics exclude normal session content; cai's CLI separately logs session events as described above.

## Related guides

- [Lua facade](lua.md)
- [MCP handler and client API](mcp-sdk.md)
- [Provider and model metadata](model-metadata.md)
- [Dynamic tools](plugins.md)

## Streaming callbacks and transport

Streaming callers that need function-call arguments can attach callbacks to `cai_stream_sinks.function_call_arguments_delta` and `cai_stream_sinks.function_call_arguments_done`.
Callers that need raw streamed output-item events for hosted tools, image/code outputs, and future item types can attach `cai_stream_sinks.output_item_done`; cai passes common metadata plus the raw item JSON as a `lonejson_spooled` value.
Callers that need raw streamed assistant text deltas before any terminal/UI affixes can attach `cai_stream_sinks.output_text_delta`.
`stream_text` remains text-only; the callback surface is for callers that want to observe streamed tool-call argument deltas and output items directly and decide their own orchestration policy.

For OpenAI API-key clients and ChatGPT subscription-auth clients, the streaming path uses the Responses WebSocket transport when the base URL is OpenAI's API or the ChatGPT Codex backend.
OpenRouter and local/mock HTTP providers continue to use HTTP/SSE streaming.
The WebSocket implementation uses libcurl's connect-only WebSocket support, sends `response.create` messages, requests the OpenAI Responses WebSocket beta, reuses the connection while healthy, retries one transient upgrade or stale-connection failure before events are observed, falls back to HTTPS streaming by default when a WebSocket stream fails before any output reaches caller sinks, and transparently refreshes ChatGPT OAuth bearer tokens once on 401/403.
After text, reasoning, tool-call, or output-item events have been delivered to the caller, WebSocket transport failures are returned as errors instead of retrying and risking duplicated output.
Set `cai_client_config.responses_websocket_fallback_disabled` to nonzero when callers prefer the WebSocket error instead of the HTTPS retry.
