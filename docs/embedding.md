# Embedding the CAI application

`libcai_app` contains the complete CAI terminal application: softline/libmdf,
configuration, encrypted sessions/auth, tools, MCP, slash commands, review and
non-interactive work. Core `libcai` remains independently usable without the
application dependencies. The application supports Linux x86_64, aarch64 and armhf,
with both glibc and musl. The standalone `cai` binary ships for all three Linux
architectures as a static musl executable, built alongside its matching SDK.

Build with `CAI_BUILD_APP=ON`; `CAI_BUILD_CLI=ON` also selects it. The `cai`
executable owns process scaffolding in `cli/main.c` and `cli/process.c`;
`examples/app.c` is the minimal public-only C embedding example. Install the
`cai-app` and `cai-app-server` components alongside their core/dependency SDKs.
Use `find_package(cai_app CONFIG REQUIRED)` and `cai::app_static` or
`cai::app_shared`, or pkg-config `cai-app`. The application shared library has
ABI 0; the core libcai ABI is 5.

For a frontend that only attaches to an owner, use the standalone
[application socket client](application-client.md). `CAI_BUILD_APP_CLIENT=ON`
builds `libcai_app_client` without selecting the terminal application or its
dependencies. Its CMake package is `cai_app_client`; pkg-config uses
`cai-app-client`. The combined application reuses the same compiled client and
wire codecs, with the existing C API unchanged.

The standalone [workspace owner SDK](application-server.md),
`libcai_app_server`, supplies asynchronous server lifecycle and supervisory
controls without Softline or libmdf. The combined facade depends on that same
backend, preserving one process-wide store registry. Shared settings/status
types live in `cai/app_types.h`; existing `cai/app.h` users retain their API and
structure layouts. Enable `CAI_BUILD_APP_SERVER=ON` independently of the UI.

An agent invocation fails before provider access when its canonical workspace
has no saved approval. Pass `-y` / `--yes` / `--force` to approve that directory
and its descendants, or `-k` / `--insecure` for an invocation-only bypass.
Saved approval applies to other conversations in that tree. Utility modes remain
available without approval. The same argv parser is used by C and Lua hosts.
Hosts may configure the bypass through the `agent.insecure` application setting;
its default is false. Environment aliases follow `program_name`, such as
`SMITH_INSECURE` for Smith. This is independent of `agent.yolo` and TLS settings.

The explicit approval is durably journaled before its small authorization record
is published in the configured scopes namespace. It is not stored in the derived
latest-session index and requires no journal scan to look up. Records survive
conversation deletion/expiry. Imported journal events are audit data and never
grant authorization. Failed approval publication fails startup; bypass runs
never save approval.

Core runtime hosts opt in with `insecure = 1` (the working directory as root), or a
host-authorized `workspace_approval_directory` containing the workspace.
`record_workspace_approval = 1` additionally records an explicit approval when
using a persistent session store. The approved root and bypass policy are inherited
by child runtimes; the request to record a fresh approval is not. The lower-level
preset constructor instead uses `workspace_instructions_root`. NULL roots and
zero-initialized runtime configuration load global instructions only.
Lua exposes the same runtime and preset fields in their constructor tables.

```c
#include <cai/app.h>

int run_smith(int count, const char *const *arguments) {
  cai_app_config config;
  cai_app *app = NULL;
  cai_error error;
  int code = 1;
  int rc;
  cai_app_setting settings[] = {
    {"agent", "identity", "Vectis Agent Smith"},
    {"agent", "codex_agents_md", "true"}
  };
  cai_app_config_init(&config);
  config.program_name = "smith";
  config.invocation_name = "vectis -a smith --";
  config.banner = "Vectis Agent Smith\n\n";
  config.introduction_markdown = "**Enter** sends a task; `/help` lists commands.\n";
  config.introduction_margin_left = 0;
  config.introduction_margin_right = 0;
  config.settings = settings;
  config.setting_count = sizeof(settings) / sizeof(settings[0]);
  /* config.logger = host_logger; -- optional borrowed pslog logger */
  cai_error_init(&error);
  rc = cai_app_open(&config, &app, &error);
  if (rc == CAI_OK)
    rc = app->run(app, count, arguments, &code, &error);
  if (app != NULL)
    app->close(app);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? code : 1;
}
```

Pass only CAI arguments, excluding the program name. A host can forward the
tail after `vectis -a smith --`, or a C main can pass `argc-1`, `argv+1`.
`run` is synchronous, never exits the host, and returns after `/quit`, EOF,
completed batch/review work, or failure. On CAI_OK its output exit code has the
normal CLI meaning (0/1/2). Invocation and restoration failures return a CAI
status and `cai_error` instead.

Agent inference retries `server_is_overloaded` by default: ten retries per turn,
after 30, 60, 120, then at most 240 seconds each. These are cancellable worker
waits, not owner-thread sleeps. The pending request and completed tool results
are reused without a continuation prompt. `CAI_AGENT_EVENT_REQUEST_RETRY`
contains the scheduled notice; `CAI_AGENT_EVENT_REQUEST_RETRY_STARTED` ends the
wait. C and Lua observers receive both. Failed attempts and retry notices also
enter the execution journal when recording is enabled, outside model context.
Set the application setting `agent.capacity_retry` to `false` to opt out, or
use `disable_capacity_retry` in the core C/Lua runtime config. This does not
retry unrelated errors or standalone compaction requests.

The C and Lua receivers accept `-D` / `--delete` through the same argument
parser. With no value it lists eligible conversations in the selected workspace;
an ID or list number deletes the conversation and its owned child history,
prompt recall, logs, exports and delivered inbox messages. An open conversation
is omitted and cannot be deleted. Shared stores and workspace files remain.
Prompt recall is per-conversation, so switching sessions also switches recall.

Core hosts can use `<cai/input_queue.h>` and `<cai/input_controller.h>` without
linking Softline or libcai_app. `cai_agent_runtime_pending_input` borrows the
conversation's queue on its owner thread. Its incremental acceptance, edits,
stop/approval decisions and model claims use the existing session journal;
ordinary commits do not serialize a queue snapshot. Atomic trim and portable
export include a streamed pending-state baseline because earlier queue events
can be removed by a history cut.
An undispatched claim for a closed/interrupted child is retained as a queued
parent turn; a child delivery already acknowledged in the parent is not
repeated. A recovered manual-review pause still requires a replacement review
handoff before queued parent turns run.

`cai_input_controller_select` binds that authority to an asynchronous mailbox.
Frontend `connect`, `request` and `poll` use copied data and wakeup descriptors;
the host schedules bounded `pump` on the queue owner. Submission success means
handoff, not acceptance: consume the explicit accepted/rejected/unknown result.
Selection changes reject unadmitted requests for the old conversation. Accepted
entries remain in the old queue. Slow observers disconnect from deltas and
resynchronize without removing work. Deselect before closing the runtime;
detach frontend connections before freeing their callback contexts. No worker
thread or HTTP listener is created by this core controller.

For transport reconnects, retain the queue producer ID and last issued request
ID. `cai_input_queue_client_resume` reattaches the same producer without a new
journal reservation, preserving its known receipts and high watermark.
`cai_input_queue_client_last_request` reports the backend watermark; use the
larger of this and the frontend's last issued ID for subsequent requests.
Reconcile an uncertain submission with its original identity. The bounded cache
retains 16 results per producer and the 16 most recently used detached producers;
active producers are not evicted. Ordered journal recovery rebuilds this cache.
Missing/expired outcomes remain unknown; never automatically resubmit with a new
identity. A trim/export initialization baseline can omit earlier receipts while
still preserving accepted work and producer-ID allocation.

For a shared frontend server, supply `cai_input_controller_config.authorize`
and a fixed authenticated attachment identity on each connection. Every copied
handoff carries `cai_input_scope` (conversation epoch and original control
generation). Admission checks that generation on the owner thread, immediately
before queue mutation; a takeover cannot be bypassed by an earlier socket read.
Synchronization and receipt reconciliation are read-only. A NULL configuration
selects trusted in-process operation.

`<cai/attachment.h>` supplies transport-independent roles and lifetime for
that gate. First attach acquires control; subsequent clients observe until
explicit takeover. Generations advance when authority changes. Persistence
defaults to false: the last client disconnect starts irreversible shutdown,
counting observers too. A persistent host continues with no attachments. After
READY, a nonpersistent host has a bounded first-attachment grace period; schedule
its reported monotonic deadline without polling while idle. The owner still
performs runtime/transport shutdown; the attachment controller never exits a
process, starts workers or closes application stores.

`<cai/environment.h>` supplies immutable shared views for independent hosts.
Open with an explicit `NAME=value` vector, or NULL to capture the process once.
An empty vector selects an empty environment. Derivation copies changes into
a new view; NULL values remove names and empty values stay empty. Retain/release
references across threads; readers never modify the host environment.

`cai_client_config.environment` retains a view for API-key lookup and default
command/plugin environments. Agent registrations inherit that client's view
unless explicitly overridden. A terminal retains its source view across YOLO
changes, rebuilding filtering without rereading a changed process environment.
Standalone registries can supply their own view; direct plugin invocations
borrow it through the call.

Configuration constructors take a `cai_config_context` with an absolute base
directory and environment. NULL members capture startup defaults once. Relative
file opens and path resolution continue using that directory even if the host
changes cwd. `cai_config_getenv` reads the effective captured/[env] value;
`cai_config_environment` returns an owned immutable view, usable in client or
tool configuration after the config is changed or closed. Captured values,
including empty ones, win unless `config_env_has_precedence=true`.
The explicit `apply_environment` utility still changes process-global state;
use the immutable view for independently running controllers. The current
application configuration resolver does not install `[env]`: it uses the
effective view for option aliases, path expansion and XDG derivation. Synchronous
reviews, commands and maintenance utilities also use captured views. Utility
logging and Chrome registration do not install environment overrides, and OAuth
browser children receive explicit environment/cwd settings. No process-state
save/restore wrapper remains. Utility streams still use their documented host
channels; independently configured instances must coordinate shared destinations.
Interactive run and ordinary prompt/goal batch run, including temporary
workspaces, use the captured controller/adapter views. MCP
discovery and later refreshes retain a separate effective environment snapshot,
so worker credential lookup does not depend on later global changes. Reopening
the MCP context adopts updated configuration; `/mcp refresh` retains the current
context's environment and still refreshes provider-managed authentication.

The Unix transport foundation is also independent of libcai_app and terminal
libraries. `<cai/cbor.h>` decodes fragments into borrowed tokens and writes
tokens directly to a sink, without a DOM or retained value. A definite string
can span many DATA calls; indefinite strings contain independently valid
chunks. Every begun string/container has an END token. Finish verifies one
complete root item. Callbacks are provisional until successful completion;
never execute a partially decoded request.

`<cai/wire.h>` wraps one CBOR item in a four-byte big-endian length (at most
128 KiB). Known record lengths come from field/chunk sizes; unknown-length
output uses successive chunk records, normally 32 KiB. Encoders do not size a
serialized buffer or backpatch. `cai_wire_peer` adds nonblocking Unix-socket I/O
and a bounded copied write backlog, without a socket worker. Poll for POLLOUT
only while `cai_wire_peer_pending()` is nonzero. Drop a failed/overflowed peer
and reconcile at the application layer; never retry part of a failed record.

`<cai/protocol.h>` adds typed attachment envelopes without retaining their
payloads. Pass `cai_protocol_decoder_event` directly as the wire decoder/peer
callback. HEADER/PAYLOAD notifications expose borrowed metadata/tokens;
COMPLETE means the record is fully validated, not that work was durably
accepted. The host still checks authenticated connection identity, control
generation and the kind-specific payload. `cai_protocol_peer_begin` emits
known metadata and framing immediately; write payload tokens with
`cai_wire_peer_write`, then
finish with `cai_protocol_peer_end`. Derive the payload's encoded length from
its known fields, and split large/unknown-length values into chunk records.

`<cai/workspace.h>` derives a canonical workspace identity and private endpoint,
owns its native listener lock, validates peer credentials and reclaims stale
sockets after a crash. Discovery never opens conversation storage. These are
transport primitives; the attach/autostart CLI remains pending. Foreground
`cai -x` uses the native controller described below. The lifecycle,
roles and persistence cutover is specified in
[workspace-server.md](workspace-server.md).

`<cai/workspace_server.h>` and `<cai/workspace_client.h>` bind those primitives
to owner-loop attachments without a runtime, renderer or worker. The client
validates the complete welcome identity and initial role, exposes poll
interests, and streams provisional payloads without gathering them. Requests
retain their original authority generation. Explicit detach drains pending
output without waiting for server shutdown; close releases immediately. The
executable must coordinate autostart, reconnect reconciliation and shutdown.

`cai_app_start(app, context, argc, argv, error)` starts a terminal-free owner
without blocking the host on session restoration. The optional
`cai_config_context` supplies an absolute startup directory and immutable
environment; omitted members are captured before start returns. Arguments are
copied. This path uses the shared configuration/runtime/store setup, durable
input queue and goal pursuit, creates no Softline/libmdf objects and leaves
host process state alone. Separate controllers may share a Pouch root through
the existing SDK client registry. Their callbacks run on their own owner
threads; hosts must marshal access to a scripting VM or another event loop.

Watch `cai_app_status_fd(app)` for POLLIN and call `cai_app_observe_status` with
an initialized owned snapshot. These notifications coalesce into the latest
state and are drained atomically with the copy; no status object accumulates
for each model fragment. The existing `get_status` remains a nondraining
thread-safe query. Readiness is `running && runtime_ready`; a failed start is
reported through final status/error. Native event/replay callbacks continue to
receive the borrowed semantic stream. The typed control API accepts prompts,
steering, goal operations, cancellation and STOP. Slash/! commands require the
frontend presentation layer and are rejected on this terminal-free surface;
restored frontend actions stay queued. Socket command presentation
and the Softline client cutover are still pending.

`cai_app_start_server` starts this same owner with a canonical workspace
listener in the effective `paths.runtime_dir`. An explicit
`cai_attachment_config` is copied and overrides startup settings. With NULL,
the lifetime uses `[server] persist` (normally false), the application-prefixed
`SERVER_PERSIST` environment setting, and `--persist` / `--no-persist` in normal
configuration precedence. The default first-attachment grace period still
applies. For a nonpersistent owner, the last graceful or abrupt
detach cancels active work and closes the runtime/stores before endpoint
ownership is released. Observers count as attachments, and vacant control
requires their explicit takeover. Set `persist` to continue after all clients
disconnect. Explicit socket shutdown and native STOP end either lifetime.
Read `has_server` and `server` in application status for phase, attachment count,
generation, controlling identity and final shutdown reason. `server_instance`
is the same identity published in the socket handshake. Without a supplied
logger, a native owner writes to the configured log directory and publishes
the full startup/session `log_path`; it does not collect frontend log notices.
A borrowed host logger keeps its output policy and leaves `log_path` NULL.
Status copies own their path, and the final snapshot retains it after shutdown.
The same status wakeup covers these changes; the owner does not poll while
idle. Socket handshake, role, lifetime, model input and queue operations are implemented.
The [queue payload contract](workspace-wire-queue.md) binds original producer
receipts and authority generations to real Unix connections. Snapshots use
begin/item/end observations; the producer never builds a complete queue value.
The Softline adapter constructs its explicit bounded editor mirror and publishes
it only at the validated end. Disconnect/takeover releases edits durably,
without advancing request IDs or replacing accepted text. Recovery releases
orphaned edits before selecting the queue. Application command replies use the
[semantic execution channel](workspace-commands.md); the terminal receiver
renders those replies through the shared presenter. Owner startup does not
fork or install signals. Explicit frontend shell/pager actions follow the
[terminal handoff contract](terminal-attachment.md#local-terminal-handoff).

Synchronous `run` composes the same owner and a terminal or batch socket client;
plain redirected input uses bounded lines and live transcript rendering. An
explicit `--server SELECTOR` instead discovers and attaches to that owner,
without a local runtime, SDK storage client or process launch. Runtime/tool
callbacks remain with the remote owner; use typed client observations for its
status and transcript. Native host STOP detaches this frontend, preserving the
remote owner's persistence policy. Configured environment/directory views do
not replace the embedding process's environment, cwd or locale.

`cai -x` / `cai --headless` is the standalone process composition of this API,
with a default of `persist = true`, JSON logging to stderr and automatic color.
An explicit INI, environment or flag setting can override persistence;
`cai --headless --no-persist` stops after the last client disconnects.
SIGINT, SIGTERM, SIGHUP and SIGQUIT request native STOP and join the owner;
stdin and terminal rendering are unused. Readiness and termination have
structured `workspace_ready` / `workspace_stopped` records. A disconnected
frontend leaves a persistent server running. Native embedding uses
`cai_app_start_server` and the host's own lifecycle/logger, rather than passing
the executable-only `--headless` mode to the synchronous terminal `run`.
The current application SDK still links Softline/libmdf; separating optional
frontend packages is part of the remaining cutover. Core libcai remains
independent of those dependencies.

`<cai/app_client.h>` is the typed socket-side queue, transcript and status
interface in libcai_app.
Open it with explicit runtime/workspace paths and service its single descriptor
and deadline through `prepare`/`pump`. It creates no worker, runtime, store,
Softline or libmdf object. Connection callbacks report verified instance,
attachment and authority generation. After CONNECTED, explicitly `bind` a new
producer or resume an original producer with its exact session identity. A
rejected binding never silently creates another producer. Request IDs for
binding and queue operations are separate.

Input callbacks borrow one validated row or receipt. BEGIN/ITEM are provisional;
publish a frontend mirror only at END. The client retains baseline metadata,
not a queue array, and enriches each row with the captured producer/state.
It rejects mixed epochs, rows, revisions and missing changes. A reset aborts
the partial baseline. Terminal consumer failures permit an explicit rebind;
transport loss requires a new connection and instance verification. Last valid
identity and processed watermark remain available for reconciliation, while
`ready` becomes false. No pending work is automatically resubmitted. Preserve
the frontend's higher last-issued request ID as well.

Submit requests with their original scope and ID. The server checks durable
receipts before authority; this lets an observer or stale generation reconcile
previously accepted work without authorizing a new mutation. Explicit
`takeover`, `shutdown` and draining `detach` retain the core lifetime rules.
This client also provides `subscribe`/`unsubscribe` and a separate borrowed
transcript callback. It accepts arbitrarily long logical events as bounded
BEGIN/DATA/END spans, retaining only fixed metadata and a 4 KiB wire chunk.
Consumers acknowledge their cursor by returning success from END. Completed
replay advances across skipped commit records to its captured watermark; live
events follow it. ERROR aborts provisional rendering and requires explicit
resubscription without resubmitting input. See the
[payload and lifetime contract](workspace-wire-transcript.md).

Status observation needs neither a queue producer nor a transcript subscription.
`CAI_APP_CLIENT_STATUS` delivers a borrowed completed `cai_app_view`; query the
same view with `cai_app_client_get_view` (NULL until the first update). Strings
are literal data valid until the next pump/close. Escape them when rendering.
The view includes effective/pending settings, identifiers/log path, context,
usage, quota, execution policy and goal state/duration, excluding goal objectives
and detailed errors. The owner coalesces unsent semantic changes; elapsed-only
updates do not generate records. `cai_app_view_elapsed` advances active timers
from the received monotonic anchor, without a provider call or server poll.
Unavailable metadata invalidates only the view. After disconnect the last view
remains explicitly stale and its durations no longer advance. ROLE still owns
control authority. See the [status contract](workspace-wire-status.md).
The Softline adapter consumes this status channel. Lua applications use
[`cai.app.client`](lua-workspace-client.md) for the same typed socket client,
including streamed transcript/execution, queue, recall and catalog callbacks.

An idle controller stays open until STOP. After observing shutdown, call
`cai_app_join` outside UI/owner callbacks, then close or start the receiver
again. Join itself does not request STOP. STOP also sets a cancellation signal
checked at journal replay boundaries. Individual dependency calls without
cancellation callbacks remain nonpreemptible through this API, including
configuration-file loading and initial store opening. Utility/review/batch CLI
modes use the synchronous receiver.

Lua `runtime:pending_input()` returns an owner-thread attachment with `state()`,
one-based `entry(index)`, `request(table)`, `set_ready(index, boolean)`,
`claim(index)` and `close()`. Requests use operation names such as `offer`,
`begin_edit`, `end_edit`, `approve`, `withdraw`, `set_intent`, `stop`, `resume`,
`clear`, `auto_approval`, `synchronize` and `resolve`. IDs/revisions/tokens are
decimal strings, `intent` is `queued` or `steer`, and `enabled=true` on an offer
requires approval. Omitted request IDs increase per attachment. Edit acquisition
returns its revision and token; supply both on replacement. Set `queue_revision`
from `state()` for resume/clear/auto-approval. Classifying host slash actions and
publishing readiness remain host responsibilities. Closing a Lua attachment
keeps accepted work; closing its runtime disables all further queue operations.

`/compact` uses the agent worker, leaving the application event loop responsive.
It stays in the conversation's queue, editable through Softline, until the parent has
no active turn, reviewer, pending control, or shell execution. Application event
observers receive the normal run and compaction lifecycle events; manual
compaction's `RUN_STARTED` has a NULL prompt payload. Escape or host cancellation
interrupts the request. The next turn uses the committed replacement context.

The initialized config selects CAI defaults, including automatic XDG paths and
ChatGPT/provider/model defaults. `config_file`, `config_directory`, `no_config`
select startup configuration. The `settings` array accepts the full INI schema,
including `[mcp.NAME]` and `[env]`, without duplicating its validation rules.

MCP connections belong to the application, independently of the active
inference provider. Saved ChatGPT authentication enables the built-in
`chatgpt` app connection by default. `[mcp.NAME] provider = chatgpt` selects
that authentication for another configured endpoint; ordinary connections can
use `api_key_env`. Discovery and `/mcp` updates run on the runtime worker.
`/mcp enable`, `/mcp disable` and `/mcp refresh` update only this invocation,
including subsequent session switches, and never edit the configuration file.
Core embedders can implement other credential sources through
`cai_mcp_client_auth_provider` and schedule attached-catalog replacement with
`cai_agent_runtime_refresh_mcp()`.
All connections default to lazy tool schemas. The local `tool_search` function
advertises source hints, searches the cached catalog with BM25, and exposes
selected schemas on the next model request; it works with either inference
protocol. `[mcp.NAME] eager = true` opts a connection into eager exposure;
`description` supplies its discovery hint. Catalog discovery still runs once
on the worker, with explicit refresh afterward. Core hosts can set the parallel
`cai_agent_runtime_config.mcp_options` array for source names/descriptions and
per-client `eager` mode; strings and registration-policy mappings are copied.
Values use INI's parsed value form: e.g. `true`, without surrounding quotes.

Sessions use an incremental ordered collection journal, with Pouch zlib
compression enabled by default. `[storage] disable_compression`, the matching
environment variable and flag, or an application `settings` entry can disable
it; the mode must match the existing root. See [session journals](journal.md)
for recovery, portable exports and the dependency-free C/Lua core APIs.

`program_name` is the application identity, default `"cai"`. Set it once before
`open`; the receiver copies it. It must be a lowercase identifier, at most 32
bytes, starting with a letter (remaining characters: letters, digits, `_`, `-`).
Use `invocation_name` for a multiword usage label such as `vectis -a smith --`.
With `program_name = "smith"`, automatic defaults are:

| Setting | Default |
| --- | --- |
| Environment prefix | `SMITH_` (`-` becomes `_`) |
| Configuration | `$XDG_CONFIG_HOME/smith/config.ini` |
| Instructions / skills / model overrides | `config_directory/AGENTS.md`, `skills`, `models.json` |
| State / cache / data | `$XDG_STATE_HOME/smith`, `$XDG_CACHE_HOME/smith`, `$XDG_DATA_HOME/smith` |
| Pouch / encryption key | `state_directory/pouch`, `pouch.key` |
| Prompt recall | Encrypted `prompts.<session-id>` collection in the sessions namespace |
| Clipboard / interactive logs | `cache_directory/clipboard`, `cache_directory` |
| Exports | `data_directory/exports` |
| Chrome sockets | `$XDG_RUNTIME_DIR/smith/chrome` when set; otherwise `cache_directory/chrome` |
| Storage namespaces | `smith.sessions`, `smith.scopes`, `smith.auth`, `smith.todo`, `smith.messages` |
| Automatic mailbox / receipt prefix | `smith` |
| MCP client name / default agent identity | `smith` |

Unset XDG variables use the standard HOME fallbacks. The C and Lua open configs
both accept `environment_prefix`, `namespace_prefix`, `queue_prefix`,
`home_directory`, `runtime_directory`,
`config_file`, `config_directory`, `state_directory`, `cache_directory`,
`data_directory`, `clipboard_directory`, `log_directory`,
and `pouch_key_file`. Roots are independent: moving the config file does not
implicitly move instructions or state. Child defaults follow their selected
root. For example, a host can supply its own resolved XDG paths:

```c
config.program_name = "smith";
config.invocation_name = "vectis -a smith --";
config.config_file = host_config_file;
config.config_directory = host_config_directory;
config.cache_directory = host_cache_directory;
config.state_directory = host_state_directory;
```

Set `home_directory` to derive all unset roots below one directory: `config`,
`state`, `cache`, `data` and `run`. Explicit individual fields, INI settings,
environment variables and flags take precedence over home-derived paths.
No application-name suffix is added. Both receivers copy the home string at
open, just like the other path fields:

```c
config.home_directory = "/srv/vectis/smith";
config.config_file = "/etc/vectis/smith.ini";
```

```lua
local app = assert(require("cai.app").open {
  program_name = "smith",
  home_directory = "/srv/vectis/smith",
  config_file = "/etc/vectis/smith.ini",
})
```

The equivalent invocation settings are `--home DIR`, `SMITH_HOME`, or
`[paths] home_dir`. The field, flag or environment variable can select
`home/config/config.ini` before startup configuration is read. An INI home
value changes derived defaults without reopening configuration elsewhere.
`-c FILE`/`--config FILE` remains authoritative. `--no-config` suppresses only
configuration creation/loading; it does not disable home-derived paths.
`runtime_directory`, `--runtime-dir`, `SMITH_RUNTIME_DIR`, or `[paths] runtime_dir`
can override the runtime root independently. These settings never change the
host process's `HOME` or relocate explicit external inputs or endpoints.

These fields seed defaults. Explicit INI values, environment, `settings`, and
argv take precedence in that order. Paths are also configurable through
`[paths]` (`home_dir`, `runtime_dir`, `config_dir`, `state_dir`, `cache_dir`, `data_dir`,
`clipboard_dir`, `log_dir`, and the existing individual file
settings), and matching `--...-dir` / `--pouch-key-file` flags. Each namespace
has its own `[storage] ..._namespace` key and matching flag, alongside
`namespace_prefix`. `[messaging] queue_prefix` / `--queue-prefix` controls
automatic names; `queue_name` / `--message-queue` replaces the full public name.
`[chrome] socket_directory` and `[mcp.NAME] client_name` override those defaults.

Prompt recall shares the configured session store and encryption key through
separate `prompts.<session-id>` collections. `ui.prompt_history_limit` defaults
to 200 and configures recall and the collection's native `max_entries` cap;
`/history` exposes the same setting in the TUI. SDK maintenance prunes oldest
entries asynchronously, without a foreground scan or per-append deletion.
Zero disables recall/recording and clears the cap without deleting saved prompts.
`storage.conversation_retention_days` defaults to 30 days since the latest
journal append; zero disables expiry. `/retention` shows that policy and
persists an override for the active conversation (0/off means forever).
Liblockdc maintenance removes the whole journal collection and its session
metadata. Individual journal records never expire separately. Both defaults
are available through C/Lua `settings`, prefixed environment variables and
forwarded flags; there is no separate prompt-history filesystem path.

Environment keys use the selected prefix plus uppercased section and key,
for example `SMITH_PATHS_STATE_DIR` and `SMITH_STORAGE_AUTH_NAMESPACE`.
Convenience aliases include `SMITH_MODEL`, `SMITH_PROVIDER`,
`SMITH_REASONING_EFFORT`, `SMITH_HOME`, `SMITH_RUNTIME_DIR`, `SMITH_STATE_DIR`, `SMITH_CACHE_DIR`, `SMITH_DATA_DIR`,
`SMITH_LOCKD`, `SMITH_NAMESPACE_PREFIX`, `SMITH_QUEUE_PREFIX`, and the existing
tool/messaging names with the new prefix. `SMITH_CONFIG` and `SMITH_CONFIG_DIR`
select the startup config file/root before loading INI. An explicitly supplied
`environment_prefix`, e.g. `"VECTIS_SMITH_"`, replaces `SMITH_` entirely; there
is no fallback to `CAI_`. Custom-provider API keys default to `SMITH_API_KEY`.
Provider conventions (`OPENAI_API_KEY`, `BERGET_API_KEY`, `CODEX_HOME`), pslog's
`LOG_`, extension host protocol names, and actual library/module names stay
independent. Explicit banner and introduction strings are used verbatim.

Names isolate storage even at the same endpoint. To share selected data, set
the corresponding namespaces explicitly; encrypted shared roots also require
the same `pouch_key_file`. The independent `cai_app_kanban_open` (Lua `application.kanban`) receiver accepts
`program_name`, `namespace_prefix`, `todo_namespace`, and `pouch_key_file` in
addition to its existing state/endpoint fields, so route handlers can open the
same board as a named application.

### Native custom tools

For hot loading, use the [dynamic tool catalog](plugins.md), available in
libcai and through `cai_app_config.plugins`. File packages and optional embedded
Lua share that catalog. The registration hook below remains for static host
capabilities, which are installed before a catalog is attached.

Set `cai_app_config.register_agent_tools` to register host tools through the same
`cai_agent_register_*` APIs used by the core runtime:

```c
static int host_fact(void *context, const char *arguments, cai_sink *output,
                     cai_error *error) {
  static const char result[] = "{\"host_fact\":\"ready\"}";
  (void)context;
  (void)arguments;
  return cai_sink_write(output, result, sizeof(result) - 1U, error);
}

static int register_host_tools(void *context, cai_agent *agent,
                               cai_error *error) {
  return cai_agent_register_raw_tool(agent, "host_fact", "Read a host fact",
                                     "{\"type\":\"object\"}", 0,
                                     host_fact, context, error);
}

/* Before cai_app_open: */
config.register_agent_tools = register_host_tools;
config.register_agent_tools_context = host_context;
```

Typed, spooled and custom/freeform tools can be registered in the same hook.
Registration runs on the thread calling `app->run`, after built-in and messaging
tools are installed and before workers start. It runs again whenever a new
runtime opens, including session switches. The agent is borrowed for the call.
Host contexts remain borrowed and must outlive each run; tool handlers execute
on worker threads and must be thread-safe. Lua and other VM callbacks require
their own owner-thread dispatch rather than calling the VM from a tool worker.
Registration errors abort startup. Help and utility commands do not call the
hook. It is not inherited by child agents and is rejected in isolated review,
matching the core runtime's extension policy.
Precedence is defaults, file/environment, explicit settings, then arguments.
Environment entries retain the normal `config_env_has_precedence` policy.

Interactive DTG timestamps use `ui.utc` (default `false`), `--utc`/`--no-utc`,
and the application's environment prefix plus `UTC` (`CAI_UTC` by default).
Runtime events, journal events and execution records expose `timestamp_ns`
(UTC Unix nanoseconds; zero means unknown) and `utc_offset_seconds` (seconds
east of UTC on the originating host). These are presentation/diagnostic
metadata, never model input. Replay preserves the original instant, including
when child events are forwarded into the parent. Rendering uses the viewing
host's local timezone at that instant unless UTC is selected. Missing original
times are omitted, not replaced with the replay clock. Noninteractive output
is unaffected. Feed timestamps use gray without additional dimming;
slash-command replies share the command input's timestamp. Submitted prompts
use libmdf's `write_literal` method inside blockquote tokens. Independently
styled timestamps beside `>` remain pending presentation support in that feed path; they do not
require a Softline quote API. Softline receives rendered bytes and retains
editor and queue ownership.
The bounded adapter repairs UTF-8 and filters terminal controls without
escaping Markdown. Thinking blockquotes use normal Markdown. Word-based
emission and the agreed bounded word RAM/spooling policy belong to libmdf;
no fragment boundary closes a quote or
inserts synthetic whitespace.
The completed status message includes its DTG but shares Softline's status
style until an independently styled status metadata slot is available.

`/tools` uses the normal full-table layout. libmdf automatically switches
the current table to row streaming at 64 KiB or 1024 retained rows, emitting
buffered rows once before continuing. Descriptions are retained in full, and
small tables preserve widths calculated from the complete table. The ordinary
Markdown feed and sink remain unchanged. Command rendering errors are
reported inline so subsequent commands and conversation turns remain usable.

`<cai/dtg.h>` provides an allocation-free parser/formatter independent of
runtime objects. `cai_dtg_parse` returns calendar fields with a two-digit year
and does not infer centuries or convert incomplete dates into Unix time.
`cai_dtg_format_time(when, reference, utc, out, capacity)` selects the short
form only when both full calendar year and month match. Local formatting
honors historical DST without changing the process timezone. `src/cai_dtg.c`
contains only syntax/zone logic; `src/cai_dtg_time.c` is the POSIX time adapter.
Lua exports `dtg_parse`, `dtg_format`, `dtg_format_time`, and `dtg_zone`;
invalid values return `nil, error`. For example,
`cai.dtg_format(assert(cai.dtg_parse("091630tjul11")))` returns
`091630TJUL11`. `J` has an unknown offset when parsed; stored event metadata
retains the exact offset. Whole-hour letters and ASCII star codes follow the
[USNO/HMNAO chart](https://aa.usno.navy.mil/downloads/TimeZoneMap_2021.pdf);
Unicode chart designators and unmapped offsets use `J`.

Prompt and Markdown themes use the application setting `ui.theme` (default
`default`). For example, pass `{"ui", "theme", "synthwave"}` in the C
`config.settings` array, or `settings = {ui = {theme = "synthwave"}}` in Lua.
Aliases select paired softline/libmdf themes; `riced:iosvkem` selects them
independently. The same setting accepts `--theme` / `-t`, `CAI_THEME` (rebased
to the host's environment prefix), and `[ui] theme` in INI. `--list-themes`
lists both libraries' themes before configuration, authentication or storage
startup. See the [pairing table](../README.md) for aliases. `plain:ascii`
disables Markdown ANSI styling while preserving UTF-8. `NO_COLOR` overrides
the selection with the plain prompt and escape-free Markdown. Themes apply
to the Markdown introduction, conversation feed and tool headings; raw banners stay verbatim
and logger configuration remains independent.
The same TUI provides `/theme` to show the active pair and both libraries'
themes, and `/theme NAME` or `/theme SOFTLINE:LIBMDF` to change the selection.
Enter and Alt-Enter apply a theme change at the next safe response/tool boundary
during a turn. Active command
display state survives the change. Commands that require a whole turn boundary
are reclassified as ordinary `Q` entries when submitted with Alt-Enter, leaving
later valid steers eligible at their own boundaries.
This selection applies to future output in the current app invocation,
including after `/new` or `/resume`, without changing startup settings.

Markdown context rotations use the application settings `ui.theme_offset`
(parent, default 0), `subagent.theme_offset` (ordinary child, default 0), and
`review.theme_offset` (reviewer, default 1). Each accepts a signed 64-bit
integer. Child offsets apply to the parent's effective libmdf theme. Rotation
uses libmdf's theme order with `boring` excluded; offsets are reduced modulo
that count before addition. ASCII/NO_COLOR and Softline's theme are unchanged.
Live and replayed review content uses the selected child theme, restoring the
parent on completion, failure or cancellation.
Tool activity verbs and durations use the effective palette's H3 foreground
with normal weight; commands, paths, tool labels and slash-command text use
H2 foreground in bold. Neutral/success/failure symbols stay gray/green/red.
The application copies foregrounds from libmdf's public palette into
libcai's dependency-free `cai_tool_display_set_palette` API (Lua
`display:set_palette(activity_foreground, label_foreground)`). Palette changes
preserve active call identity, output limits and partial UTF-8/SGR decoding.
ASCII/NO_COLOR suppresses styles. Raw subprocess SGR and patch diff backgrounds
retain their existing independent treatment.

C hosts pass these keys in `cai_app_setting`; Lua uses, for example,
`settings = {ui = {theme_offset = -300}, review = {theme_offset = 1}}`.
The matching flags are `--theme-offset`, `--subagent-theme-offset`, and
`--review-theme-offset`. Environment aliases are `CAI_THEME_OFFSET`,
`CAI_SUBAGENT_THEME_OFFSET`, and `CAI_REVIEW_THEME_OFFSET`, using the host's
configured prefix. Matching slash commands query without an argument and set
with one. Changes apply at a safe response/tool boundary through Softline's
editable queue. These presentation settings belong to libcai_app; the core
runtime continues to emit renderer-independent lifecycle events.

The optional `run_review_and_fix` model tool is disabled by default. Native
runtime hosts set `cai_agent_runtime_config.enable_review_and_fix = 1`; Lua
runtime hosts use `enable_review_and_fix = true`. Application hosts set
`[agent] review_and_fix_tool = true` through INI or their C/Lua settings, or use
`--review-and-fix-tool` / `CAI_REVIEW_AND_FIX_TOOL`. It requires reviewer and
goal tools. A user must explicitly authorize repeated review/fix work; normal
review requests and follow-up fixes do not grant that permission. The tool
replaces the current conversation's goal and returns after queueing it.
`cai_agent_review_and_fix_objective` and Lua's
`cai.review_and_fix_objective(workspace, request)` expose the shared objective
builder without starting an agent. The request accepts default, base and
uncommitted scopes (`base_branch` names the base in C/Lua; model tool uses `base`).

The C and Lua application receivers use Softline's native editor controls.
Alt-K stashes the whole draft and clears it; Alt-J appends the reusable saved
draft after the current text. The stash lasts for this app run, across prompts
and shell handoff, independently of the kill ring. Alt-N clears an ordinary
draft or discards an edited queued/approval entry. Undo restores only text,
without its queue identity or approval; submitting an empty queued edit also
discards it. Approval focus requires an empty ordinary prompt. `/help` lists
the complete bindings. Explicit incoming-message removals are persisted before
session changes and shutdown; untouched pending messages remain recoverable.

`[paths]`, `[storage]`, and `[env]` XDG variables provide host path overrides.
Configuration strings/settings are copied by open. Logger ownership stays with
the host until close; CAI neither closes nor reconfigures a supplied logger.
With a supplied logger CAI does not create its default log files or intercept
that logger's records for interactive notices. Use a terminal-compatible sink.

`banner` and `introduction_markdown` default to NULL in C and are unset by
default in Lua. The host supplies both; CAI's own name, attribution and usage
text are configured by the executable or the public C example. The optional raw banner is printed
verbatim before softline/libmdf starts: all newlines belong in the host's banner
string. CAI's scaffold begins its banner on the first output line and ends it
with `\n\n` to separate the introduction. The optional Markdown introduction
has independent `introduction_margin_left` and `introduction_margin_right`
fields in both C and Lua, passed directly to its own libmdf renderer. Both
default to zero and must be nonnegative, leaving at least three content columns
at the current terminal width. Interactive conversation text retains its normal
margin and a blank line before content starts. Non-interactive final responses
have no added left margin. The introduction goes through
libmdf's output feed before the first session opens or replays; it is
not added to model context or session history. Help, version, utilities, native
messaging, reviews and non-interactive runs print neither. The CAI scaffold
colors C/I bright white and A bright blue on a terminal, resets before Smith
and the attribution, and honors NO_COLOR and TERM=dumb for its banner.
`-h` uses the configured program name. `app->option_count/option_at` expose an
immutable array of long flags, short flags, argument types, descriptions,
repeatability and negative aliases. `app->write_help` writes CAI's formatter to
a caller-owned sink. Neither method loads configuration, logs in or starts UI.

Interactive `run` keeps the common controller and host callbacks on the calling
thread, with one default-stack terminal adapter worker. The caller may itself
be a host worker. Give the adapter exclusive terminal ownership; its terminal
modes and signal dispositions are restored before return. Ordinary batch prompts,
goals and explicit review/fix loops use a default-stack socket adapter instead
of a terminal worker. They preserve trusted host follow-up controls, callback
affinity and the original failure/cancellation/STOP outcomes. Final output is a
streamed journal-range projection through libmdf without an intermediate spool.
Direct review, combined goal/instruction startup, slash/shell batch instructions
and maintenance utilities still
require serialized process-resource use: configured environment
and locale state are restored after their synchronous invocation, including
failures. Maintenance has a separate dispatcher and cannot run an agent loop or
open a workspace owner/editor; its offline export helper still restores saved
context read-only. See [application utilities](application-utilities.md) for the
remaining utility/browser environment boundary. Owner and batch loggers read
captured `LOG_` settings without installing an environment; relative file sinks
use the selected workspace, and retained loggers retain that immutable view.
One run per receiver may be
active; recursive invocation is rejected.
Workspace selection, configuration and temporary workspaces leave the host cwd
unchanged. Relative CLI paths use the selected workspace, while INI paths use
the configuration file's directory; only subprocesses enter their workdir.
The receiver may run again after returning. Close it only while idle. Worker
threads finish before return; they never call the Lua facade.

### Observing and controlling a run

`event_callback` and `replay_callback` in `cai_app_config` observe the same
runtime and journal events as the application, alongside its normal rendering
and logging. They run on the thread calling `run`. Events and their pointers
are borrowed for the callback only; copy anything retained. Replay does not run
tools or submit prompts. Return `CAI_OK`, or a CAI error with `cai_error` detail
to stop the run. Observers may use the receiver's `control` and `get_status`
methods, but must not reenter `run`, close the app, or operate on its runtime.

Console replay defaults to 10 whole turns or standalone shell entries. Set
`ui.replay_turns` through `cai_app_setting` or Lua
`settings = {ui = {replay_turns = 10}}`; `--replay-turns` and the application
environment prefix plus `REPLAY_TURNS` also apply. Zero hides replay and -1
shows all. This affects console output only: C `replay_callback` and Lua
`on_replay` still receive each retained journal event once, including omitted
console turns. Logs, exports, and the full restored agent context are unchanged.

Provider-supplied reasoning summaries are retained as ordered presentation
events and replayed through the live renderer: headings use the status line,
and multiline bodies use Markdown blockquotes. They follow the same turn limit
as replies and tools. These journal events add no synthetic model input.

Local execution policy is configured with `[agent] yolo`, `CAI_YOLO`, or
`--yolo`/`--no-yolo`, defaulting to false. An embedded application supplies
`{ "agent", "yolo", "true" }` as an application setting; its program-specific
environment prefix applies normally. C runtime/preset configurations and Lua
runtime/Smith tables expose `yolo`. Standalone C tool configurations and Lua's
exec/read/list registration tables expose the same opt-out. It is inherited by
reviewers and other child runtimes. `HOME` and every `XDG_*` value are preserved
in both modes; YOLO also inherits other host variables, with managed-command
pager overrides described below. Ordinary
user permissions, disabled tools, output retention, timeouts and cancellation
still apply.

`/yolo` inspects the live policy; `/yolo on` and `/yolo off` change it without
reopening the conversation or writing config.ini. `/status` shows the effective
mode and any queued change. Changes remain editable in Softline until the
runtime is idle: the active turn, child runtimes, shell capture and managed
terminal processes must finish first. Esc preserves the conversation's stopped queue.
The selected mode applies to later tools and new children; it is not stored in
the conversation commit. Restarting the application uses startup settings.

C hosts use the owner-thread `cai_agent_runtime_get_yolo(runtime, &enabled,
&can_change, error)` and `cai_agent_runtime_try_set_yolo(runtime, enabled,
&applied, error)` APIs. Busy changes return `CAI_OK` with `applied == 0`; the host
retains the request and retries at an idle boundary. A no-op succeeds immediately.
An error preserves the complete previous policy. Tool registrations, terminal
handles/output, callbacks, goals and session identity are retained. Lua exposes
`runtime:yolo()` returning `{enabled=boolean, can_change=boolean}` and
`runtime:try_set_yolo(boolean)` returning whether the request applied. A false
result means busy; an error returns `nil, error`.

Managed agent commands use pipes with closed stdin by default; model requests
with `tty=true` get a PTY and can receive `write_stdin` input. Exactly one command
can run at once. A second `exec_command` returns `status="busy"`, `accepted=false`
and the current handle without starting or queueing a process, or consuming its
output. Every accepted command receives a new handle. Managed commands force
`PAGER`, `GIT_PAGER`, and `GH_PAGER` to `cat` in restricted and YOLO modes.

The default initial yield interval is 10000 ms. A wait ends when the process
finishes or the deadline expires; receiving output alone does not return control
to the model. Output continues through lifecycle callbacks during the wait.
C hosts inspect/change this policy on the owner thread with
`cai_agent_runtime_get_terminal_yield_time(runtime, &milliseconds, error)` and
`cai_agent_runtime_set_terminal_yield_time(runtime, milliseconds, error)`.
The range is 0..30000 ms; zero forces foreground waiting until completion or
cancellation, overriding model arguments. Changes affect subsequent waits in
the runtime and its active child, and future children inherit the policy.
Lua exposes `runtime:yield_time_ms([milliseconds])`; Smith/runtime constructor
tables accept `yield_time_ms` and `foreground`. Low-level C terminal registration
uses `default_yield_time_ms` (zero selects the 10000 ms default) and `foreground`.

For libcai_app, `[terminal] yield_time_ms`, `CAI_YIELD_TIME_MS`, and
`--yield-time-ms N` select the interval; **zero means foreground** at this
application boundary. `[terminal] foreground`, `CAI_FOREGROUND`, or
`--foreground` also force foreground execution. Program-specific environment
prefixes and `cai_app_setting` overrides apply normally. `/foreground` (`/fg`)
enables it; `/background` (`/bg`) restores the libcai 10000 ms default if yielding
was disabled, otherwise preserving the current positive interval.
`--background` has the same startup behavior. `/yield_time_ms` queries the
effective policy and `/yield_time_ms N` changes it at a request boundary.
These are host settings, not model input or persistent conversation settings.
The model-facing argument `yield_time_ms=0` still means immediate yielding when
the host has not forced foreground execution.

Execution history is enabled by the application and is independent of pslog
levels or host logger configuration. `event_callback` / Lua `on_event` receives
`CAI_AGENT_EVENT_EXECUTION_RECORD` / `cai.AGENT_EVENT_EXECUTION_RECORD` with a
bounded JSON record in `data`; `replay_callback` / `on_replay` receives those
records with the journal type `execution`. The normal console replay limit
still applies only to presentation. No execution record is model input.

C hosts include `<cai/execution.h>`, initialize a `cai_execution_record`, call
`cai_execution_record_parse`, inspect the fields and call
`cai_execution_record_cleanup`, including on error. The optional
`cai_execution_record_write_markdown` consumes records in order and joins their
bounded payload chunks without inserting breaks inside a field. Rendering
escapes terminal controls while leaving the stored payload unchanged.

For fragmented input, use `cai_execution_decoder_open` / `feed` / `finish`.
Feed original bytes directly; call `finish` only at the enclosing authenticated
record boundary, then inspect/clean up the initialized owned output record.
`reset` abandons a candidate and reuses the handle. Failure leaves the previous
output unchanged and requires reset. The decoder retains bounded metadata and
one decoded 4 KiB execution chunk, without gathering encoded JSON or a complete
logical tool result. Its SAX binding uses the same field schema as the writer,
validates UTF-8/NUL and integer limits, and skips unknown values without storing
them. The one-shot parse helper uses this same decoder.

Socket hosts can set `cai_app_client_config.execution` to receive a borrowed
`cai_app_execution_event` at authenticated transcript END. It includes the
decoded chunk, source attribution and replay/subagent flags. It runs before
the raw END callback; both consumers must accept before the cursor advances.
Malformed execution content produces a typed historical warning (NULL record,
nonzero code and a bounded reason). Accepting it advances past that record;
later valid chunks remain usable. Framing errors, local allocation failures or
a rejecting consumer terminate only that attachment.
Disconnect/replacement discards a partial candidate. A callback may request
another subscription: its borrowed record stays valid until return, and the
old END cannot advance the replacement subscription's cursor. No storage
client, worker or tool execution is created in this projection.

`cai_agent_runtime_export_execution_markdown(runtime, sink, error)` streams the
stored timeline at an idle boundary; Lua exposes
`runtime:export_execution_markdown(sink)`. The application combines this with
the existing conversation exporter in its human-facing `/export` and
`--export` Markdown file. Core `export_markdown` and `export_markdown_file`
remain agent-handover projections and exclude execution records.

```lua
local cai = require("cai")
-- Inside on_event, or on_replay with event.type == "execution":
if event.type == cai.AGENT_EVENT_EXECUTION_RECORD or event.type == "execution" then
  local record = assert(cai.execution_record(event.data))
  -- record.kind, tool_name, call_id, data, offset, final, code, http_status...
end
```

Kinds include `tool.started`, `tool.arguments`, `tool.completed`, `tool.output`,
`tool.failed`, `tool.partial_output`, `hosted.tool`, `subagent.input`,
`request.failed`, `response.completed`, `turn.started`, `turn.completed`,
`turn.cancelled`, `turn.failed`, `goal.changed`, and compaction
started/completed/failed. Error fields are separate ordered chunks; arguments
are exact JSON text or native custom-tool input. `offset` counts original
UTF-8 bytes and `final` ends that field, including an empty field. The source
session/sequence and parent session/call identify delegated activity. When
correlating with a parent export, use its outer journal sequence for display
order. A started call without a terminal record is incomplete, not a replay
request to execute it.

The journal durably acknowledges each record before its live notification.
Journal acceptance is serialized separately from the runtime state mutex:
worker-side store callbacks do not hold the lock needed by owner-thread state
and metrics reads or cancellation. An append error is still a turn failure;
it never acknowledges or presents the failed text as successful output.
Worker-side notifications use the bounded event queue; synchronous owner
operations never wait for their own pump and retain the journal if the live
queue is full. Recording failures prevent starting a tool whose arguments
could not be recorded. Deeper transport/storage implementation logs remain
with the host's pslog sink; no log-severity filter feeds execution history.

Use `cai_agent_runtime_pump_bounded(runtime, timeout_ms, max_events, error)` in
an interactive event loop. The positive limit bounds callbacks, not callback
execution time; the host must keep its own callbacks short. Remaining events
keep the borrowed wakeup descriptor readable. `cai_agent_runtime_pump` retains
its drain behavior. Lua accepts the optional event limit as
`runtime:pump(timeout_ms, max_events)`; zero/omitted selects ordinary drain.
libcai_app uses four events per interactive dispatch, including manual reviews.
Text/reasoning observations have at most 1024 payload bytes, splitting complete
UTF-8 scalars at chunk boundaries. Their durable provider deltas are unchanged.

`app->control(app, type, text, &id, &error)` is safe from other host threads
while the app remains alive. It copies ordinary input into a FIFO of at most
64 waiting commands and wakes the app loop. Calls with no ready runtime,
invalid arguments, or a full ordinary queue fail immediately, with `id` set
to zero. STOP has one reserved allocation-free command: it is accepted during
startup, utility runs and at ordinary capacity. Repeating pending STOP returns
its original ID. Application acknowledgements remain ordered, including
cancellation of commands that cannot apply before shutdown.

| Type | Text | Behavior |
| --- | --- | --- |
| `CAI_APP_QUEUE_PROMPT` | Required | Interactive: inject into softline's editable approval queue, using the existing autoqueue setting. Batch: enqueue the next turn. |
| `CAI_APP_STEER` | Required | Submit steering to the active turn at the next runtime safe boundary. Reject if no turn can accept it. |
| `CAI_APP_CANCEL_TURN` | None | Cooperatively cancel the current turn and durably pause an active goal. This leaves an interactive app open; ordinary user turns work while paused. |
| `CAI_APP_STOP` | None | Request orderly shutdown and discard unstarted work. Run/join completion waits for workers; the host process stays alive. |
| `CAI_APP_REPLACE_GOAL` | Required | Replace the goal with an unbudgeted objective and pursue it, as `/goal new TEXT` does. |
| `CAI_APP_PAUSE_GOAL` | None | Pause an active goal at a safe boundary. |
| `CAI_APP_RESUME_GOAL` | None | Resume a resumable goal and pursue it. |
| `CAI_APP_CLEAR_GOAL` | None | Clear goal tracking without undoing completed work. |

The application pursues the runtime's active durable goal regardless of whether
it was created by a model tool, a slash command, startup options or host control.
Successful turns generate further goal continuations until the goal completes
or stops; approved queued prompts run first. Resuming an active goal from storage
also starts pursuit. Cancellation durably pauses the goal, including across
restart; provider failure stops automatic pursuit for the current invocation.
The model may pause for a user answer/approval and decide to resume a paused
goal in a later turn, respecting outstanding approvals and instructions to
remain paused. Completed goals cannot resume. Three consecutive empty automatic
turns without text, reasoning, tool or compaction activity block the goal.
This behavior is shared by the C receiver, Lua facade and CAI executable.

Goal status and the last turn's outcome are independent. Host resume/replacement
controls can start pursuit immediately after cancellation or failure; no ordinary
prompt or process restart is required. Resume also accepts an already-active
unfinished goal without resetting its timer or accounting. Completed goals
remain non-resumable. An in-flight turn continues through its safe boundary;
resume does not create a duplicate turn. Existing usage/token caps still apply.

Hosts using libcai directly can call
`cai_agent_runtime_continue_goal(runtime, &started, &error)` or Lua
`runtime:continue_goal()`. These admit an idle active goal and render the compiled
Codex-derived objective/budget/audit prompt. They report whether work started;
pending user input or goal controls take priority. Ordinary user submissions
are separate from automatic continuations and reset the empty-turn streak.

Acceptance into the app FIFO is distinct from acceptance by the runtime or
softline. `control_callback(context, id, status, error)` acknowledges each
accepted command on the run thread, outside the queue lock. This includes
commands rejected during application or abandoned at shutdown. Successful
prompt injection acknowledges queue insertion, not human approval or completion;
successful goal/steering controls acknowledge the runtime's safe-boundary request,
not its eventual effect. IDs increase within each run and restart at one on the
next run. A callback may enqueue another command. Further commands behind STOP
are rejected. A stopped run need not deliver events still pending at shutdown.
STOP also ends isolated review without emitting unfinished findings. It does
not deliver approved prompts after shutdown has been requested. An observer's
failure detail is retained even if the terminal loop reports a later I/O error.
During `/shell` or `!sh`, the child uses the terminal directly. Only the shared
variants (`/shellshare`, `!sh+`) use a PTY relay to capture output. STOP interrupts
either handoff, terminates the shell and restores the terminal before command
acknowledgement. Other host controls remain queued until handoff ends.
STOP also interrupts login's callback accept and partial-request waits. Login
and standalone sends acknowledge STOP without opening an agent runtime. Standalone
`--queue NAME --message TEXT` returns after liblockdc accepts the enqueue; a
STOP arriving during that operation is acknowledged afterward and does not
recall the committed message.
Acknowledgement callbacks must not close or recursively run the app, including
final callbacks delivered after process restoration. Close after `run` returns.

The optional model-controlled `set_reasoning` tool accepts just
`{"mode":"normal"}` and `{"mode":"high"}`. Enable it with
`[agent] reasoning_tool = true`, `CAI_REASONING_TOOL=true`, or
`--reasoning-tool`. Independent `normal_reasoning_effort` and
`high_reasoning_effort` settings default to `medium` and `high`; the
corresponding `CAI_NORMAL_REASONING_EFFORT` / `CAI_HIGH_REASONING_EFFORT`
variables and `--normal-reasoning-effort` / `--high-reasoning-effort` flags
override them. The generated INI template includes these settings.
The model is instructed to raise effort for complex tasks and only return to
normal when explicitly told. A high request never lowers a higher current
effort. Changes take effect after the durable tool round, before another model
request, and use the existing persisted request settings on resume.

Every valid tool request returns the same `ok` acknowledgement, whether it
changes effort or leaves settings unchanged. The tool does not expose the
effective effort or confirm that a change took effect. Unsupported or unavailable
advertised effort lists produce a host/user warning only; no recovery prompt or
provider request is generated for an unsupported change. Supply metadata through
the existing provider catalog when choices are unavailable.
Hosts receive `CAI_AGENT_EVENT_REASONING_TOOL_RESULT` with a user-facing receipt
identifying the requested mode, mapped effort, previous effort and actual result.
Unchanged settings include a reason. These receipts also appear in replay and
exports as `reasoning.receipt` execution records; they never enter model context.
Mappings and tool enablement come from configuration, not conversation storage;
manual `-r` and `/reasoning` remain independent of the normal mapping.
`/reasoning-tool [on|off|normal LEVEL|high LEVEL]` configures the current app
between turns, including subsequent `/new` or `/resume`, without editing INI.
Review children retain their own reasoning settings and do not inherit the tool.

C hosts initialize `cai_agent_runtime_config.reasoning_tool` with
`cai_reasoning_tool_config_init`, then set `enabled`, `normal_effort`, and
`high_effort`. The owner-thread `cai_agent_runtime_get_reasoning_tool` and
`cai_agent_runtime_set_reasoning_tool` APIs inspect or reconfigure it between
turns; setters copy strings. Lua hosts pass
`reasoning_tool={enabled=true, normal_effort="medium", high_effort="high"}`
to `client:new_smith_runtime` and use `runtime:reasoning_tool()` or
`runtime:reasoning_tool{enabled=false}` to inspect or update it.

```c
cai_app_status status;
unsigned long long id;
cai_app_status_init(&status);
/* From a host thread while the run has a ready runtime: */
rc = app->control(app, CAI_APP_QUEUE_PROMPT, "Check the result", &id, &error);
rc = app->get_status(app, &status, &error);
/* status.session_id, status.effective, status.pending, status.goal,
 * status.metrics, status.control_id/control_error, status.outcome/error */
cai_app_status_cleanup(&status);
```

Status getters copy an owned snapshot under a mutex; they never call into the
runtime from a host thread. Initialize the output before first use. A successful
query replaces it; a failed query leaves it unchanged. All its strings and error
fields survive later queries, runs and application close, until snapshot cleanup.
Do not shallow-copy a snapshot or modify its owned pointers.

Snapshots describe the most recent owner-thread safe boundary; an observer may
see the previous boundary while its event is being processed. `running` and
`runtime_ready` distinguish startup/utilities, an open runtime, and a finished
invocation. The final snapshot retains the session ID, effective/pending model,
reasoning and tier settings, goal projection, cumulative token/cost accounting,
context usage, turn timing, outcome and CLI exit status. Paused, blocked and
other goal outcomes are in `goal.status`. Failures retain `cai_error` fields
where available; runtime failure events provide their diagnostic text. The last
control's status/error is separate from run failure. Assistant output stays
incremental through the event observer rather than being accumulated in status.
`error` can also retain a recovered interactive diagnostic; use outcome and exit
status to decide whether the invocation failed.
After STOP or an observer error, runtime fields retain the last available
boundary before shutdown; `outcome` and `running` describe the completed app
invocation even if that interrupted runtime snapshot still shows an active turn.

A later accepted run resets status and command IDs; an invocation rejected
before acquiring application ownership leaves the previous result intact.
Keep the receiver alive until host threads have stopped using controls/status;
`close` itself is idle-only and must not race other receiver calls. The new
application surface remains ABI 0, independently of the core library's ABI 5.

## Lua 5.5

With the application and Lua surfaces selected, `require("cai.app")` exposes
the same receiver and ownership boundary:

```lua
local app = assert(require("cai.app").open {
  program_name = "smith",
  invocation_name = "vectis -a smith --",
  banner = "Vectis Agent Smith\n\n",
  introduction_markdown = "**Enter** sends a task; `/help` lists commands.\n",
  introduction_margin_left = 0,
  introduction_margin_right = 0,
  settings = {agent = {identity = "Vectis Agent Smith"}},
  -- logger = initialized_pslog_logger,
})
local code, err = app:run {"-Nni", "Explain this workspace"}
app:close()
assert(code, err)
```

`app:options()` returns metadata entries, `app:help()` returns formatted text,
and `app:close()` is idempotent. Invalid operations return `nil, error`.
The logger is retained through its public pslog Lua interop until close.
Only loggers advertising pslog's native-sink capability are accepted, because
the application and its dependencies log from native worker threads. Lua
function sinks are rejected before starting work.
The complete executable example is `examples/app.lua`.

Set `on_event = function(event) ... end` and `on_replay = function(event) ... end`
in the open configuration to observe runtime and saved journal events. Lua
receives copied tables on its owning run thread. Callback errors stop that run
and appear in `app:get_status().error`, after process state is restored.
Callbacks remain collectable even when they capture the application itself.

`app:control(name, text)` returns a command ID or `nil, error`. Names are
`queue_prompt`, `steer`, `cancel_turn`, `stop`, `replace_goal`, `pause_goal`,
`resume_goal`, and `clear_goal`; text requirements match the C table above.
`app:get_status()` returns a copied table with the C snapshot's fields. Outcome
strings are `not_run`, `running`, `completed`, `stopped`, `cancelled`, and
`failed`. The last command acknowledgement is in `control_id`, `control_status`
and `control_error`. Lua callbacks may queue controls or query status; they
cannot close or recursively run the active application. Native custom-tool
functions remain a C-only hook because their handlers execute on workers.
`state` is the numeric C `cai_agent_run_state` value (see `<cai/agent_runtime.h>`).
For a terminal-free host, use the [Lua owner lifecycle](lua-workspace-owner.md):
`start`/`start_server`, copied status notifications, controls and join/close.
Server snapshots include `has_server`, binary `server_instance`, and a
`server` table retaining phase, stop reason, persistence and attachment state.
Streaming Lua observers run on the [typed client](lua-workspace-client.md)
pump thread; inline `on_event`/`on_replay` remain synchronous `run` facilities.
In Lua, `metrics.session_usage` contains token counters directly alongside
`estimated_spend_usd`, `estimated_spend_eur`, and `limit_exceeded`; it does not
have the C accounting struct's nested `usage` member. `app:options()` returns
a 1-based array of copied option records; absent short flags/argument labels
are `nil`. Neither query starts a run or reads configuration files.

The optional native module installs with component `cai-app-lua` under
`lib/lua/5.5/cai/app.so`. Add that prefix's `lib/lua/5.5/?.so` to `LUA_CPATH`,
or compile `lua/app_lua.c` against the installed public `cai-app` SDK and Lua
5.5/lua-pslog headers. `lua/CMakeLists.txt` builds this module when pkg-config
finds `cai-app`; its install target places the module in the same Lua layout.
Both Lua modules use installed public SDK headers and shared libraries;
neither requires CAI's private application or runtime headers.
The ordinary core `cai` LuaRock stays usable without libcai_app. Its source
archive includes the optional application facade for native embedders.
`make lua-app-test` builds this optional surface against the native application
and runs its ownership, error, export and mock-provider embedding checks.

The six Linux x86_64/aarch64/armhf GNU/musl SDK presets each include `libcai_app.a`,
`libcai_app.so` (ABI 0), the public header and CMake/pkg-config metadata. They
ship inside the corresponding `cai-<version>-<target>.tar.gz` SDK archive:

| Architecture | glibc preset | musl preset |
| --- | --- | --- |
| x86_64 | `x86_64-linux-gnu-release` | `x86_64-linux-musl-release` |
| aarch64 | `aarch64-linux-gnu-release` | `aarch64-linux-musl-release` |
| armhf (ARMv7 hard float) | `armhf-linux-gnu-release` | `armhf-linux-musl-release` |

Other SDK targets retain core libcai only. Application dependency pins are recorded
in `share/cai_app/dependencies.json`; core dependency metadata remains in
`share/cai/dependencies.json`. The standalone binary archive includes the C/Lua
embedding examples and this guide alongside `bin/cai` and `cai(1)`.

Chrome native-host registration records the current executable path. An
embedding executable that offers `--chrome-setup` must also dispatch Chrome's
extension-origin invocation to CAI's run entrypoint, or directly to the public
`cai_chrome_native_host_run` API, rather than its normal host option parser.

## Conversation messaging

The application includes optional shared-Pouch messaging (default off) with user approval through
softline's normal prompt hook. Configure it through C app settings or Lua
`settings = {messaging = {enabled = true, enable_auto_enqueue = false,
ack_timeout_ms = 300000, queue_name = "smith"}}`. Omit `queue_name` to derive
a stable name allocated from the basename and the fewest distinguishing
ancestor components, with the actual hostname as a leading component.
The newest app start owns the
named mailbox; old delivered messages stay in their original conversation.
`/queue on` and `/queue off` enable or disable messaging between turns in the
same conversation. `/queue NAME` overrides the full name; `/queue` shows it.
`/status` includes the queue and approval mode. These live settings survive
`/new` and `/resume`, without changing config.ini.
`storage.messaging_store` selects a named `store.NAME` backend independently
of `storage.sessions_store` and `storage.auth_store` (all default to `default`).
Each store defines `endpoint`, pouch `key_file`, and optional HTTP `client_pem`.
In C these are ordinary `cai_app_setting` section/key/value entries. In Lua use,
for example, `settings = {["store.team"] = {endpoint = "pouch:///srv/agents/pouch",
key_file = "/srv/agents/pouch.key"}, storage = {messaging_store = "team"}}`.
Default-store key configuration is `[store.default] key_file`; the C
`pouch_key_file` field still seeds that default.

Clients live for the app run and its nested contexts, not individual turns or
reopened sessions. The process shares one client per physical pouch root, and
the final owning context closes it. App-owned logging state is retained with
the client. Externally supplied loggers and outputs remain borrowed and must
outlive the store contexts using them. SDK diagnostics retain the opening
context's sink until that client closes; supply the same host logger when
independent contexts need a common destination. Liblockdc owns reconnect behavior.

`--temp` (or `settings = {ui = {temporary = true}}` in Lua) creates a private
cache workspace and conversation store without rewriting the normal config.
Relative forwarded input paths resolve against the original selected workspace
before entering the temporary workspace; INI paths remain relative to the
configuration file.
Auth and messaging retain their selected stores. Teardown closes workers,
clients and softline, and deletes
private history, prompt history and managed cache data. The workspace deletion
question defaults to No. Non-interactive runs, EOF, errors, and host STOP
preserve the workspace unless `--yes` / `--force` is passed. STOP also interrupts
the final question. See the [temporary workspace and store configuration](../README.md).
Interactive temporary runs use the common owner and socket frontend, resolving
configuration once without changing host cwd/environment. Their separate
private runtime directory is removed after owner shutdown, before asking the
cleanup question; persistence is disabled for the ephemeral owner. The host
run remains active during that question and accepts STOP until return.

See [messaging](agent-messaging.md) for delivery,
editing, ACK deadlines, and core C/Lua next-turn integration APIs.

## Darwin port assessment

Core libcai already has a Darwin arm64 SDK. All four application dependencies
also publish Darwin arm64 SDKs at the pinned versions. The application remains disabled
for Darwin until these boundaries are implemented and verified:

| Boundary | Current implementation | Port work |
| --- | --- | --- |
| Worker/log notifications | Linux `eventfd` | Use a nonblocking pipe or Darwin notification backend; keep softline's descriptor-watch interface |
| Turn/status and shell deadlines | Linux `timerfd` | Provide monotonic timers with idle disarming and cancellation; preserve existing deadlines |
| Shell handoff | `pipe2`, `ptsname_r`, Linux PTY setup | Use Darwin PTY and descriptor setup while preserving terminal restoration, child cleanup and bounded capture |
| Host environment restoration | GNU `clearenv`, process `environ` | Use Darwin `_NSGetEnviron()` and portable environment clearing/restoration without changing the host ownership contract |
| Agent command sandbox | Core exec already selects experimental `sandbox-exec` on Darwin | Verify Smith's tool defaults, permissions and failure behavior on a native Mac; Linux cgroups remain unavailable |
| Chrome native host | Linux host/setup code; Darwin currently returns unsupported | Implement executable/manifest discovery, socket flags and peer credentials, or explicitly keep this capability unavailable |
| Shared/module packaging | ELF `.so`, GNU linker flags | Add Mach-O `.dylib` identities, export rules and the appropriate Lua module linking rules |

Recommended implementation: keep one application runtime and put notification,
timer, PTY and environment differences behind small private platform helpers.
Local osxcross SDK probes confirm the missing `sys/eventfd.h`, `sys/timerfd.h`,
`clearenv` and `pipe2` interfaces. The port requires source changes.
Use the existing osxcross lane for build/link/export/relocation checks, then a
native macOS host for real softline input, resize, shell handoff, cancellation
and repeated embedded-run checks. Cross compilation cannot establish that
terminal behavior. See the [roadmap](../ROADMAP.md) for the deferred port.

## Shared kanban and HTTP routes

Use `cai_app_kanban_open` to open CAI's encrypted shared board engine independently
of the terminal. It returns the core `cai_todo` receiver; `handle_http` serves an
embedded read-only page, assets and live snapshots. The equivalent Lua helper is
`require('cai.app').kanban`. See [shared kanban](kanban.md) for C/Lua ownership,
routing, streaming, configuration and per-conversation selection.
