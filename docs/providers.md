# Providers and authentication

The default `chatgpt` provider imports `~/.codex/auth.json` first when present, otherwise it loads credentials from cai's lockd store.
`--auth-json` selects another import source.
Unchanged source files preserve tokens refreshed in cai's store; refresh never writes back to the source file.
If neither source nor stored credentials exist, `cai --login` opens a browser login, stores auth in the `cai.auth` namespace, and exits.
`-l` means `--list`, not login.
The `openai` provider uses `OPENAI_API_KEY`, `openrouter` uses `OPENROUTER_API_KEY`, `berget` uses `BERGET_API_KEY`, and `custom` requires `--endpoint` and `--model` with `CAI_API_KEY` by default or a variable selected by `--api-key-env`.
`--provider`/`-p` selects a provider.
ChatGPT and OpenAI start with `gpt-6-luna`; OpenRouter starts with `openai/gpt-5.6-luna`.
Berget starts with `zai-org/GLM-5.3-Flash`.
It supports vision, streaming, and function calls for cai's local and MCP tools.
Berget does not offer provider-hosted image generation.
All providers start with medium reasoning; `-m`/`--model` and `-r`/`--reasoning-effort` override those defaults.
`cai` loads optional model metadata from `$XDG_CONFIG_HOME/cai/models.json` (or `~/.config/cai/models.json`); `--model-metadata FILE` overrides it. libcai accepts the same JSON through `cai_client_config.model_metadata_json`.
Berget's live model catalog includes every model it advertises, with model type, capabilities, and EUR prices. libcai includes a maintained table of model-card context limits for Berget models because its API omits them; fallback JSON can override those limits.
See [model metadata](model-metadata.md) for the schema and precedence.

## Chat Completions providers

Berget uses its OpenAI-compatible Chat Completions endpoint.
The CLI streams text, image URLs, and function tools, including `apply_patch` and MCP tools, through libcai's normal agent and session runtime.
It resumes conversations by sending local history as chat messages.
The default Berget model accepts image input.
Hosted image generation is disabled because Berget does not provide that API; file IDs and arbitrary file attachments return an explicit error.
Berget API keys and session content are kept in the same locations as other `cai` providers; the key is read from `BERGET_API_KEY`.
Chat Completions sends tool-returned images in a labelled user message after all tool results from the assistant reply.
The tool messages carry text acknowledgements; image URLs and detail settings remain attached to the images.
GLM may emit reasoning separately or inside a tagged `<think>` section; cai keeps tagged thinking out of the final answer and streams plain answer text.
Berget does not provide a concise reasoning summary for that model.
A live Berget compaction test runs a real function tool, forces client-side compaction, then checks that the next turn recalls the tool's result without executing it again.
A separate vision test asks the live model to identify a deterministic red PNG:

```sh
CAI_RUN_BERGET_LIVE_TEST=1 BERGET_API_KEY=... \
  ctest --test-dir build/debug -R 'cai_tests.berget_live_' --output-on-failure
```

Chat Completions refusal deltas use the visible output sink and delta callback, and remain typed refusal content in completed message items and saved history.
Buffered responses expose that text through `response->refusal(response)`.

## Retry behavior

Smith runtimes automatically retry the exact provider error `server_is_overloaded` ("Selected model is at capacity").
The first retry waits 30 seconds, then 60, 120, and 240 seconds; subsequent waits stay at 240 seconds.
There are at most ten retries per turn, in addition to the original attempt.
The active turn remains responsive to Escape, host cancellation and shutdown.
The status message and logs say when the next retry will run; exhaustion reports the provider error and stops the turn.
Completed tool calls are not repeated.
The pending request is reused without adding a user prompt such as "Proceed." Incomplete attempt captures are discarded; any partial text already displayed can appear again when the model resamples.
Quota, rate-limit, policy and other errors do not trigger this capacity policy.

Disable it with `--no-capacity-retry`, `CAI_CAPACITY_RETRY=false`, or `[agent] capacity_retry = false`.
C/Lua runtime config accepts `disable_capacity_retry = true`; libcai_app accepts the normal agent setting.
The same policy applies to child runtimes and both wire protocols.
Low-level standalone client requests and explicit compaction endpoints do not use it.
With transcript recording, failed attempts and retry notices are execution records, never model input.
A new turn resets the retry allowance.

Separately, Chat Completions requests retry transient HTTP 429, 502, and 503 failures and connection failures before response headers, with at most three attempts.
This includes failures while a large request is still uploading; stopping its producer does not suppress a transport retry.
Input serialization failures remain errors and are not retried.
Retries stop once model activity or usage is received.
Authentication, invalid requests, permanent quota errors, capacity errors (handled by the runtime above), malformed streams, callback failures, and partial responses are not retried.
A retry after a completed tool round resends the tool results without executing the tools again.
Backoff starts at 500 ms, adds jitter, and doubles up to 30 seconds.
Valid `Retry-After` seconds or HTTP dates set a minimum wait; values above the cap return the original error.
Cancellation interrupts the wait promptly.
`timeout_ms` applies to each attempt.
The C client config and Lua `cai.open` accept `chat_max_attempts` (1 disables retries, maximum 16), `chat_retry_initial_delay_ms`, and `chat_retry_max_delay_ms` (maximum 60000).
Zero selects each field's default; the effective initial delay must not exceed the maximum delay.

## Provider policy diagnostics

For `cyber_policy` or `cyber_notice` error codes, the runtime sends one automatic explanation-only prompt in the same conversation, using the selected model.
Only the enabled workspace-scoped `read_file` and `list_files` tools are available, with at most eight sequential tool rounds; execution, edits, MCP, hosted tools and goal actions remain unavailable.
It asks for a bug-report summary of the preceding work, possible triggers, and a tentative assessment of the visible security context, separating facts from hypotheses.
The model cannot know the provider's internal trigger or certify a false positive.
The prompt and answer are recorded normally and can be exported.
A failed diagnostic is logged without another diagnostic attempt.
The original error still fails the run, including non-interactive invocations, and no original task or goal is automatically resumed.
Goal state remains unchanged, and an explicit next user prompt runs normally with the user's configured tools.
The diagnostic's restrictions do not carry into that turn.
The public runtime event `CAI_AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED` (`cai.AGENT_EVENT_POLICY_DIAGNOSTIC_STARTED` in Lua) identifies the generated prompt before its response streams.

See [model metadata](model-metadata.md) for capability/context/pricing precedence and [C SDK authentication](c-sdk.md#authentication-and-transport-configuration) for explicit library handles.
