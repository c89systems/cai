# Workspace client/server completion audit

Audit of `dc408f3` and its executable-path correction, 2026-11-06. The preceding
turn implemented native JSONL export; this audit checks the whole workspace
contract rather than treating that individual cutover as completion.

The controlling specification is [workspace-server.md](workspace-server.md),
with the [terminal adapter](terminal-attachment.md),
[command admission](workspace-commands.md), [batch adapter](workspace-batch.md)
and typed channel documents it links. HTTP/Web UI, a user-wide daemon and TCP
listeners remain outside this iteration. Verification uses the native Debug
build, the static x86_64 musl executable and local fixture providers. It does
not claim live-provider or complete cross-target release certification.

## Requirement evidence

Each row identifies implementation and observable checks. Passing a codec or
package test alone does not establish the application's lifecycle behavior.

| Requirement | Implementation | Evidence and limits |
| --- | --- | --- |
| Reusable incremental CBOR | `include/cai/cbor.h`, `src/cai_cbor.c` | `cai_cbor`: RFC vectors, every split, byte-at-a-time input, malformed UTF-8/lengths/nesting, floating-point bits, failed sinks and sticky errors. Its default-stack worker sends 32 MiB in 2 KiB chunks and asserts consumption during production. |
| Bounded framed Unix transport | `wire.h`, `protocol.h`, `wire_peer.h`; corresponding core translation units | `cai_wire`, `cai_protocol`, `cai_wire_peer`: partial headers/bodies, incremental payloads, actual socket streaming, short I/O, errors and cancellation. No whole conversation/message accumulator is required. |
| One owner per canonical workspace | `src/cai_workspace.c`, `src/cai_workspace_registry.c` | `cai_workspace`, `cai_workspace_registry`: directory identity/aliases, distinct workspaces, simultaneous starts, private paths, hostile path types, crash recovery, staging/publication replacement and unchanged-inode pruning. |
| Authenticated discovery and instance pinning | `workspace_discovery.h`, `workspace_claims.h`; core discovery/claims/client/server code | `cai_workspace_discovery`, `cai_workspace_claims`, `cai_workspace_client`: stale/ambiguous names, explicit relative paths, probe deadlines, changed instances, malformed hints and rejection before authority/lifetime changes. Discovery does not start an owner. |
| One controller, observers and explicit takeover | `attachment.h`, core attachment/server admission; `cli/workspace_host.c` | `cai_attachment`, `cai_workspace_server`, `cai_input_socket`, `cai_app_server_workspace`, `cai_cli_attach`: observer input/control rejection, own read-only queries, edit ownership, stale generations, takeover and request reconciliation. Provider request logs prove rejected observer text never runs. |
| Slow or broken viewers cannot stall others | Core workspace transport and bounded send queues | `cai_workspace_server` output-progress, transport-failure and producer-backlog cases; `cai_workspace_client` control-backpressure/adversarial cases. Incomplete records do not become accepted actions. |
| Persistent and nonpersistent lifetimes | `src/cai_attachment.c`, `cli/server.c`, `cli/process.c` | `cai_app_server_workspace`, `cai_cli_headless`, `cai_cli_attach`: work after detach, abrupt loss, first-client grace, explicit persistence overrides, last-detach shutdown, orderly signal shutdown and resume. `-x` defaults persistent; ordinary autostart defaults nonpersistent. |
| Quiet discovery/autostart and `-s` | `cli/attach.c`, `cli/spawn.c`, `cli/bootstrap.c` | `cai_cli_headless`, `cai_cli_attach`, `cai_cli_attach_boot`: existing-owner attach, quiet absent-owner startup, simultaneous-launch reconciliation, abandoned bootstrap, errors and no second SDK client/runtime in the terminal. |
| Native executable-path discovery | `cli/executable_path.c` | `cai_cli_executable_linux` and the Darwin API adapter test cover the running image, exact capacity, missing/empty/truncated paths, dyld canonicalization and relative paths. The translation unit also compiles against the real arm64 macOS SDK. Live macOS execution is not claimed. |
| Native server lifecycle without process mutation | `cai/app_server.h`, `cli/server.c`, `cli/app_control.c`, `cli/app_run.c` | `cai_app_server_controller`, `cai_app_controller`, `cai_app_utility_stop`: asynchronous startup, READY/FAILED/STOP, reserved stop admission, join/restart, copied inputs, concurrent contexts and retained host state. Embedding does not fork, exit or install process signal handlers. |
| One shared storage registry and context lifetime | `cli/pouch.c`, standalone server plus facade composition | `cai_app_server_mix_static` and `cai_app_server_mix_shared` use both public products concurrently, assert one Pouch open, close one context and continue using the survivor. Core/client compile-boundary guards reject storage/renderer dependencies where forbidden. |
| Independent client/server SDK products | `CaiAppClient.cmake`, `CaiAppServer.cmake`, public client/server headers and package components | Configuration tests disable renderer/storage packages as appropriate; installed CMake and pkg-config consumers link/run after relocation. Standalone server configuration also disables Lua. Shared exports and object dependency checks guard the actual compiled boundary. |
| One backend queue per conversation | `input_queue.h`, `input_controller.h`, `cli/input_socket.c`, Softline adapter | `cai_input_queue`, `cai_input_controller`, `cai_input_socket`, `cai_cli_attach`, `cai_cli_workspaces` and `cai_app_terminal_workspaces`: independent drafts/receipts, parked queues, edits, cancellation, return/catch-up and no blind reoffer of uncertain input. |
| Runtime continues while frontend is occupied | `cli/workspace_host.c`, reader worker, terminal descriptor adapter | Both `cai_cli_attached_handoff` variants hold shell/help/log pager handoffs while provider work completes; unseen output appears once after return. Resizes, interrupts, failure, takeover and context switches retain ordinary rules. |
| Streaming replay, recall and live rendering | Transcript/history/catalog channels, `cli/transcript.c`, common presenter and formatter | `cai_transcript`, `cai_transcript_store`, wire/selection tests, `cai_app_terminal_replay` and `cai_app_terminal_review`: authenticated boundaries, fragmented/large bodies, bounded readers, initial-window versus suffix selection, review attribution and live/replay presentation. Model restoration remains owner-side. |
| Synchronous/batch facade uses the same owner | `cli/run_frontend.c`, `cli/batch.c`, shared invocation compiler | `cai_app_run_terminal`, `cai_app_run_batch`, `cai_cli_batch`, `cai_cli_batch_shell`, pipe input and startup-intent tests: host callback affinity, accepted-work settlement, goals/instructions, cancellation, local commands and absence of whole-input/response staging. |
| Temporary lifetime and cleanup | `cli/temp.c`, owner/frontend convenience composition | `cai_cli_temp_owner` and `cai_app_run_terminal`: same owner path, selected config/shared stores, private history/discovery lifetime, default-no/yes/force/EOF, and failures before READY. |
| Optional Lua owner and typed client | `lua/app_lua.c`, `lua/app_client_lua.c`; [owner](lua-workspace-owner.md)/[client](lua-workspace-client.md) APIs | A fresh build with current dependencies passes `cai_app_lua_owner` and `cai_app_lua_workspace_client`: copied context/environment, async readiness/failure, stop/join/GC, empty environment, actual streamed tool output, typed actions and no client-side home/store creation. This is not proof from the obsolete Lua build cache. |
| Maintenance remains separate | `cli/utility.c`, captured configuration/logger/browser views | `cai_cli_utility_boundary`, utility-stop and browser-environment tests: no owner/frontend/loop for utilities, no host environment/cwd/locale/signal mutation, no unexpected inference, concurrent independent invocation views. |
| Portable JSONL is a native storage projection | `cli/journal_projection.c`, `cli/pouch_export.inc` | `cai_journal_projection`, CLI export/journal/trim and both compaction tests: bounded generated values, metadata/checkpoint recovery, visible history, usage/goals, source/sink errors, original timestamps and actual imported continuation. JSONL-only export is guarded against client/runtime construction. |
| Runtime-free instruction preparation | `cai_agent_preset_write_instructions`, shared `cai_instruction_writer` | `cai_instruction_writer` guards against client/agent construction and large rendered allocations, streams an 8 MiB caller-owned extension on a default worker stack, and covers callback-backed skills and source/sink/allocation failures. Workspace approval and model-prompt checks compare complete output with live agents, including disabled capabilities. The application's Markdown handover still needs the native-history/execution adapter. |
| Runtime-independent handover formatter | `cai/handover.h`, `src/cai_handover.c`; live runtime cursor adapter | `cai_handover` generates 32 MiB of provider text under allocation guards on a default worker stack, verifies output during production and exactly two source passes, and covers metadata after text, roles/tools/reasoning, omitted attachments, malformed frames, failures and cancellation. Existing runtime Markdown tests exercise its migrated adapter. The application still needs the native-view/execution projection; it still restores an offline runtime for Markdown. |
| Stable native projection context | Private `cai_cli_journal_view` in `cli/journal_projection.c` | `cai_journal_projection` composes native history with the core handover formatter, reuses one captured head for metadata/history/events/repeated JSONL, verifies trim visibility and nested-read rejection, and exercises generated 8 MiB values under allocation guards. Callback cancellation stops both indexing and later reads after 8 KiB. Header/instruction and execution rendering still need wiring into the application's Markdown entry point. |
| Owner controls stay responsive during a large export | `cli/app_run.c` currently invokes `cai_cli_session_export` synchronously | **Incomplete.** The native JSONL projector streams, but its index pass runs inside command dispatch. A slow/gated export must be tested against concurrent status/cancellation; socket separation alone is not proof. Retained export work must not create a worker per chunk/request, reenter collection reads or weaken snapshot validation to pass that test. |
| Static musl executable/default worker stacks | Musl build preset and owned target | Core streaming workers, owner lifecycle, real CLI socket/PTY fixtures and the journal projection run on default pthread stacks. `file` verifies the rebuilt executable is static x86_64; the existing user symlink targets that artifact. No stack enlargement is introduced. |

## Explicit dependency and buffering boundaries

- CAI uses liblockdc's direct pull-source append and bounded read callbacks.
  The 0.27.0 collection push API's staging is not used as a streaming substitute.
  A future push adapter still requires a genuinely streaming upstream API.
- Softline 0.16 lacks a pre-activation completion veto. With prompt providers
  enabled, unmatched `$`/`@` words in shell text can lose their trigger before
  CAI receives them. Returning no candidates does not veto activation. The
  local CR is `stash/softline-completion-context-and-literal-triggers.md`.
  No editor-source patch, text reconstruction or hidden alternate FIFO is used
  to claim that upstream limitation is solved.
- Libmdf's explicitly accepted word decision buffer/spool and finite native
  pager buffering remain rendering policies. They do not authorize collecting
  entire transport payloads or model context behind a streaming API.

## Remaining application requirements

**The full goal is not complete.** Offline Markdown export still constructs a
read-only runtime, restoring the retained provider context before writing the
handover. It is not a storage-only streaming export.

Application `/export` also needs explicit long-operation control proof and an
owner-responsive dispatch path. Its current synchronous call can occupy the
owner during journal reads. Retain command identity and storage lifetime until
completion, keep UI requests/control progress available and make cancellation
reach bounded source work. Utility `run` callbacks retain their documented
calling-thread affinity; this is not permission to move every utility to a
background thread. Concurrent durable queue admissions also append to the
journal: resolve that interaction with the stable export view explicitly.

The existing handover includes the active developer instructions available to
that export, which need not match the saved conversation. Preserve those
semantics while factoring a runtime-free instruction-preparation path and
shared formatter, as specified in the
[Markdown baseline](workspace-export.md#markdown-implementation-baseline).
That refactor is authorized work, not blocked on a new approval. The separate
recorded-only proposal changes the content contract and remains optional.

The verification above proves the implemented boundaries, with the stated
platform/dependency limits. It does not turn this outstanding export work
into completion or claim a full live/release test matrix was run.
