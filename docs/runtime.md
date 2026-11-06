# Agent runtime and Smith

`cai_agent_runtime` is CAI's host-neutral agent-mode loop: it owns the worker, serial tool dispatch, durable commits, and an event queue, while the host owns input, rendering, and its application event loop.
An event callback is delivered only when the owner thread calls `cai_agent_runtime_pump`; a poll-only runtime deliberately suppresses presentation events and exposes state only.
Hosts can use the borrowed wakeup descriptor with `poll`/`select`, copy streamed text or provider-issued reasoning summaries into any renderer, and call the explicit threadsafe input methods from an input thread.

Interactive hosts should use `cai_agent_runtime_pump_bounded(runtime, timeout_ms, max_events, &error)` (Lua `runtime:pump(timeout_ms, max_events)`) to return to keyboard and other watches between batches.
A positive event limit preserves pending wakeup readiness; omitting Lua's limit or setting it to zero uses the original drain operation. The application owner pumps bounded runtime batches; attached frontends consume their own socket observations rather than pumping a second runtime.
Live text/reasoning observations are split into at most 1 KiB chunks; their original journal records and content are unchanged.
Store appends remain serialized and durably acknowledged before notification, but run outside the mutex protecting UI state and published snapshots.

The built-in `smith` preset is CAI's Codex-inspired coding-agent profile.
It uses the native `apply_patch`, sandboxed file/image tools, optional MCP and image generation, durable goals, and one managed pipe/PTY terminal slot with only one active command.
`submit_interactive` follows Codex's StartOrSteer policy: it starts an idle turn or queues interactive direction for an active one at the next safe model/tool boundary.
`submit_queued` queues an ordinary FIFO turn for after the active turn completes.
Core `libcai` does not parse slash commands or render a UI: downstream applications map their own controls to those APIs, review lifecycle calls, and runtime Markdown export.
`libcai_app` supplies the terminal application, including its slash commands; the standalone `cai` executable is its embedding example.

Managed terminal commands preserve the executable `PATH` inherited when their tools are registered, so toolchains installed outside the system directories remain available.
An unset `PATH` uses the system default; an explicitly empty value stays empty.
`HOME` and every `XDG_*` variable are preserved exactly, including empty and unset values.
Other host variables are filtered by default.

Execution policy is separate from tool availability. See [execution policy](configuration.md#execution-policy) for the application controls; embedders select the corresponding runtime/preset policy explicitly.

At an idle runtime boundary, hosts can select the next-turn model with `cai_agent_runtime_set_model` (or Lua `runtime:set_model`).
For ChatGPT subscription clients, `cai_client_list_models` retrieves Codex's authoritative account catalog, including the opaque compaction compatibility hash.
CAI uses that metadata for switches: equal hashes preserve context, differing declared hashes compact retained local history before the selection changes, and missing metadata does not cause CAI to invent a compatibility decision.
The selected model, reasoning effort, and reasoning-summary mode are committed together.
Resume restores them unless explicit runtime configuration overrides the corresponding fields.

Context reporting uses the effective provider window, rather than a compiled API model maximum for a signed-in ChatGPT account.
Before discovery it is unknown; a commit retains the last known window for the selected model so resume can report it without blocking startup on the catalog.
Current catalog metadata supersedes that saved value.
Runtime metrics and compaction use the same window.
Lua exposes these metrics through `runtime:metrics()`, including context/usage and active/completed turn timings, and accepts `resume_session_id` for exact resume.
`runtime:ids()` exposes `session_id`, the last retained provider `response_id`, and a provider `conversation_id` when configured.
The C snapshot API is `cai_agent_runtime_get_ids`; it remains safe during a turn and makes no provider requests.
Lua clients select `api_protocol = "responses"` or `"chat_completions"` explicitly when endpoint presets are insufficient.

## Model-owned instructions

Smith bundles pinned model-specific Codex developer instructions.
GPT-5.6 Sol/Terra/Luna share a template; GPT-6 Luna, GPT-6 Sol, GPT-6.1 Sol and GPT-6 Astra each select their own.
The `openai/` provider prefix is accepted for these identifiers; other model identifiers use the GPT-5.6 fallback.
Custom preset instructions and the isolated review rubric remain authoritative.

Model changes replace the model-owned instruction prefix at the existing safe settings boundary, preserving the captured tool policy, repository instructions, skills and developer extension.
Resume selects the saved model's instructions; a failed commit restores both the model and its prior prompt.
CAI adapts Codex's channels, serial tool dispatch, questions, skills and connector assumptions through its capability overlay.
Builds use committed assets only.
The import helper `bash scripts/import_smith_prompts.sh CODEX_CHECKOUT` updates sources and their provenance manifest for a deliberate prompt refresh.

## Workspace instructions

Smith loads cai's global `AGENTS.md` from `${XDG_CONFIG_HOME:-$HOME/.config}/cai/AGENTS.md` when it exists; `--agents-md PATH` overrides that file.
`--codex-agents-md` additionally loads `${CODEX_HOME:-$HOME/.codex}/AGENTS.md` after the cai global file and before authorized repository ancestor `AGENTS.md` files.
Workspace files are disabled by default in core libcai: a host must supply `cai_agent_preset_config.workspace_instructions_root`, or use runtime `workspace_approval_directory` / `insecure`.
`libcai_app` supplies the approved root after its startup gate.
`-I TEXT` or `--developer-instructions TEXT` appends developer instructions.
Loaded AGENTS.md sources are explicitly framed as the current user's instructions and working context, including any stated role or preferences.
Their complete contents and loading order are preserved on resume and model changes.

## Goals and settings

Goal state and accounting are durable runtime data. `cai_agent_runtime_get_goal` reads a snapshot; the create/replace/start, pause/resume and clear APIs control it on the owner thread. An achieved goal cannot resume. Cancelling an active goal durably pauses it; a later host resume or permitted model decision can restart pursuit.
Hosts call `cai_agent_runtime_continue_goal` after a turn settles. It reports whether a continuation started, gives pending user work priority and blocks a goal after three consecutive empty automatic turns. It does not resume a paused or achieved goal implicitly.
Use the runtime's settings boundary for model, reasoning and service-tier changes. The application maps its editable backend queue onto these operations; it does not send slash commands as model text. See [terminal goal controls](terminal.md#goals) and [command policies](interactive-command-audit.md) for the application surface.

## History, compaction and export APIs

The C API exposes `cai_agent_runtime_visit_history(runtime, limit, visitor, context, &error)` and `cai_agent_runtime_trim_history(runtime, count, &error)`.
Both are owner-thread operations.
Visitor views are borrowed only during the callback; callbacks must not mutate the runtime.
A zero listing limit means 20; an excessive trim count fails without removing entries.
Hosts can use the runtime configuration's `replay_callback` for retained journal replay, and `cai_agent_runtime_history_event_visible` when exporting a stable journal.

Lua exposes the same behavior:

```lua
local entries = assert(runtime:history()) -- optional listing limit
for _, entry in ipairs(entries) do
  print(entry.remove_count, entry.kind, entry.summary)
end
assert(runtime:trim_history(1)) -- remove the newest whole entry
```

`kind` is `turn`, `shell`, or `compaction`.
History and trimming do not issue provider requests or execute tools.

Native hosts call `cai_agent_runtime_can_compact(runtime, &ready, &error)` and `cai_agent_runtime_compact(runtime, &error)` on the owner thread.
The latter starts asynchronous worker work; pump the usual run and compaction lifecycle events until completion, failure or cancellation.
`RUN_STARTED` has no prompt payload for this task.
Lua exposes `runtime:can_compact()` and `runtime:compact()` with the same behavior.
These additions use the opaque runtime; no receiver layout or ABI changes are needed.

Core SDK hosts enable this together with `record_transcript` and can inspect live `CAI_AGENT_EVENT_EXECUTION_RECORD` events or replay events of type `execution` or `subagent`.
`<cai/execution.h>` provides incremental chunk decoding, cleanup and Markdown rendering.
Socket clients can receive typed execution chunks at authenticated transcript boundaries without collecting encoded JSON.
`libcai_app` enables recording by default.
Lua exposes the same event constant and `cai.execution_record(json)` decoder.
Lua `on_replay` receives `subagent` envelopes through the same callback; decode `event.data` with `cai.execution_record` to inspect child identity and content.
SDK hosts can stream a human-inspection timeline using `cai_agent_runtime_export_execution_markdown` or Lua's `runtime:export_execution_markdown(sink)`.
Core agent-handover exporters exclude these records; the application's Markdown export combines both projections.

Runtime hosts export Markdown through the handover API in `cai/handover.h`; application hosts use the [native export projection](workspace-export.md).
See [tool execution and display](tools.md), [session journals](journal.md) and [application owner lifecycle](application-server.md) for their separate ownership boundaries.
