# Workspace servers and attachable clients

Implementation specification, 2026-11-04. This supersedes the transport order
in `frontend-architecture.md`: Unix sockets and CBOR come first; an optional
HTTP/Web UI adapter comes later. It does not claim that the cutover is already
implemented. The existing queue/controller foundation remains the baseline.

Current implementation: core libcai supplies incremental CBOR, bounded records,
canonical endpoint ownership, registry/probes, attachment roles and reusable
server/client event loops. The application supplies a terminal-free owner via
`cai_app_start`/`cai_app_start_server`, with the runtime, stores, accepted queue,
commands and goals independent of terminal lifetime. READY follows restoration;
endpoint ownership remains until runtime/store shutdown. Core transport and
application clients create no model runtime or SDK storage client.

The executable supports persistent `-x` owners, quiet automatic startup,
verified `-s` selectors and a Softline terminal attachment. One controller
admits mutations; observers can inspect and explicitly take over. Terminal
workspaces, drafts and queues remain separate. Native contexts use captured
environment/directory views; configuration does not install `[env]` into the
process. Interactive synchronous run now composes this owner and socket
frontend while retaining callbacks on the calling thread. Temporary interactive
runs use that same composition, with an ephemeral owner and cleanup after join.
Batch composition still owns legacy process scaffolding. Maintenance now has
its own [utility dispatcher](application-utilities.md), retaining the serialized
environment/logging wrapper until the upstream lookup API is available. Native library
controller startup never forks or installs signal handlers.

Independent typed channels carry [queue observations](workspace-wire-queue.md),
[status](workspace-wire-status.md),
[transcript spans](workspace-wire-transcript.md),
[prompt recall](workspace-wire-history.md) and
[completion catalogs](workspace-wire-catalog.md). Transport consumers receive
bounded borrowed spans; they do not restore model context or mirror complete
journals. One retained collection reader worker services native replay/recall
windows while direct append notifications supply live transcript events.
Terminal tools share the common one-pass formatter; help and completion
semantics are shared with the synchronous receiver.

See [terminal attachment](terminal-attachment.md) for current verification and
remaining cutover work, [workspace process handoff](workspace-process.md) for
executable ownership, and [command execution](workspace-commands.md) for backend
admission/delegation. The confirmed decisions and specification below remain
the target contract; an implemented boundary is not a claim of complete feature
parity. In particular, batch composition, extended startup parity
and optional frontend packaging remain unfinished.

The optional [Lua workspace client](lua-workspace-client.md) exposes the typed
C client channels and explicit scopes. Lua drives prepare/pump (or an explicit
blocking wait); callbacks stay on that Lua thread and copy bounded spans.
The [Lua owner lifecycle](lua-workspace-owner.md) starts the same native owner,
with copied startup views, status notifications, join and ordered close/GC.
Lua streaming observers attach through the client; inline observers remain a
synchronous receiver facility and are rejected by asynchronous start.

Submitted prompts belong to the libmdf feed. Complete native inputs and
arbitrarily fragmented socket spans use the same terminal-safety adapter into
libmdf 0.22.0's `write_literal` method inside its blockquote context. The adapter
repairs invalid UTF-8 and filters terminal controls, retaining only a bounded
chunk and an incomplete character. It does not escape Markdown. User prompts
remain literal; thinking uses the normal Markdown feed.

Source newlines and the actual prompt end provide the semantic boundaries;
transport fragments retain the same renderer state and never introduce
delimiters. Interrupted fields discard their unprinted suffix. Libmdf owns
word decisions, quote layout, wrapping, margins and theme colors. Softline
receives rendered bytes and owns the editor and queue.

The accepted upstream contract caps word RAM at 4 KiB by default (configurable
through 64 KiB), with optional temporary-file spooling up to 1 MiB and prefix
truncation at the storage cap. This word decision storage is explicitly
accepted; it is not a reason to collect complete remote prompts in CAI. The
earlier partial-word emission request is withdrawn. No Softline quote API is
required.

## Confirmed product decisions

- One server process owns each canonical workspace directory. A
  user-wide daemon, multi-workspace process and TCP listener are out of scope.
- CBOR is the binary encoding. Implement a small reusable codec in libcai;
  do not introduce an ASN.1 package, external CBOR dependency or generated code.
- One attachment controls the workspace. Other attachments observe until an
  explicit takeover. Enforce this in the server, including queue operations.
- The server owns runtimes, stores, active conversation, integrations, accepted
  input, commands, approvals and continuation. Softline/libmdf are a frontend.
- Detach releases only that attachment. Servers default to `persist = false`:
  loss of the last attachment starts orderly server shutdown. `-x`/`--headless`
  selects `persist = true`, so work continues without clients. Automatic
  startup uses the nonpersistent default unless persistence is explicitly set.
- True streaming is a requirement, including codec input/output, transport,
  replay and export. Do not hide full-value buffering or temporary-file staging
  behind a streaming API. Bounded chunks and bounded individual input commands
  are permitted. The explicitly accepted libmdf word-decision storage policy
  above applies to rendering, not to collecting transport messages.
- liblockdc 0.27.0 does not provide true streaming collection push. Do not
  implement a CAI buffering workaround for that limitation. Keep that dependency
  boundary explicit and adopt the upstream streaming push API when available.

## Library boundaries

| Surface | Responsibility |
| --- | --- |
| `cai/cbor.h` in libcai | Incremental CBOR token reader and callback writer; no socket, JSON, DOM, logger or application dependency. |
| Core server/client transport | Bounded framed messages, connection identity, request admission, role enforcement, ordered observations, disconnect/backpressure and reconciliation. No terminal or application commands. |
| libcai_app controller | Owner-thread lifecycle, runtime and storage setup, command interpretation, immutable configuration/environment, status and journal projection. |
| libcai_app workspace host | Discovery, listener lifecycle and binding the reusable transport to the application controller. No renderer. |
| libcai_app client | Typed actions, queue observations, status and replay/live events. Never opens writable session storage. |
| Softline adapter | Native editor/queue interaction, local draft, completions, prompt recall, libmdf, theme/width, pager and terminal handoff. |
| Executable | Explicit process launch, daemonization, signals, foreground logging and readiness diagnostics. A library constructor never implicitly forks its embedding host. |

The synchronous app receiver becomes a convenience composition of controller
and frontend. Remove the old combined lifecycle loop as callers move; do not
keep two implementations of runtime ownership or command policy. Core C APIs
must work without Softline, libmdf, Kore or Lua. Lua is an optional binding,
not a server prerequisite. Expose the same lifecycle and typed client semantics
through the optional Lua application binding.

## Lifecycle and threading

An application controller has one owner thread. It creates and closes runtime
contexts, pumps events and input boundaries, dispatches admitted requests,
continues goals and watches ownership loss whether or not any frontend exists.
Existing runtime workers remain. Socket readiness must not run inference or
storage work, and no worker is created per event, request or output chunk.

Start is asynchronous: publish STARTING, READY or FAILED. STOP has a thread-safe
cancellation token that bounded restoration/configuration/network waits inspect;
it cannot depend solely on the owner reaching its mailbox. Stopping does not
call `exit()` in a host. Join is explicitly blocking and never runs in Softline
callbacks. Detach releases attachment resources, then evaluates last-client
lifetime: a nonpersistent server stops after its last client leaves; a
persistent server continues. An initial readiness/attachment grace period
allows an automatically launched server to receive its first client. Failure
to attach must not leave a nonpersistent server running indefinitely. If
an autostarting client quits before READY, its startup cancellation channel
must also stop the server it launched; disconnecting an incomplete handshake
alone is insufficient. An SDK client must never kill an unrelated owner.
Once last-client shutdown begins, a racing attachment receives a stopping
response and retries discovery after the old owner releases its endpoint; it
does not revive a partially stopped runtime.

All same-root store contexts reuse their liblockdc client and retain it for
their actual lifetime. A frontend neither opens another writer nor renews a
collection lease. Server shutdown cancels/joins workers, saves admitted queue
decisions and closes contexts before releasing the endpoint.

The controller uses explicit workspace paths and a captured environment;
remove host `chdir`, `setenv`, `clearenv`, locale and signal mutation from that
path. Only exec children change their cwd/environment. Standalone process
scaffolding owns signals. Terminal configuration belongs to the frontend.
The per-workspace process choice does not excuse process-global mutation in
the embeddable controller.

## Workspace identity and discovery

The workspace is the canonical selected directory, not its Git root. Starting
in different subdirectories selects different workspaces. Symlink aliases to
one directory select one server. Configuration is resolved before discovery,
preserving explicit/global config and existing `--home`/XDG precedence.

Use the application's configured runtime directory, normally
`$XDG_RUNTIME_DIR/<app>`. Existing unified-home behavior uses `<home>/run`.
The fallback must be a private user-owned directory; diagnose an unsafe path.
Use a short digest of the canonical workspace identity for socket/lock names,
check Unix socket path length, and verify the full canonical identity during
the handshake rather than trusting a digest. Directories are mode 0700 and
sockets mode 0600; validate the peer user where supported.

An OS advisory lock coordinates endpoint ownership and simultaneous starts.
Only its owner may replace a stale endpoint. Never unlink a live socket based
on a failed request, an old PID or an absent registry entry. Check file type,
owner and directory safety; do not follow a hostile socket-path symlink.
Hold the ownership descriptor until shutdown, CLOEXEC everywhere except an
explicit readiness descriptor passed to the new executable.

Registry entries contain workspace path, optional name and endpoint identity.
They are discovery hints, not writable conversation state. Validate by
connecting. Prune stale entries under the corresponding ownership lock.
Names are application-local, unique and registered explicitly with
`--workspace-name NAME`; ambiguous names fail with the available identities.
Paths and stable workspace IDs also work as selectors. No dependence on
whether agent-to-agent messaging is enabled.

Implemented registry storage uses `ws-<id>.info` in that same private runtime
directory. The owner encodes a definite four-item CBOR array directly to an
exclusive temporary file: schema text `cai.workspace.registry`, canonical
workspace bytes, name text and 16-byte instance bytes. An atomic rename
publishes it; readers never see an unfinished record. Its staging path is
`.ws-<id>.tmp`: while holding workspace ownership, a new publisher may remove
an interrupted private regular staging file, but rejects symlinks and other
unsafe types. No directory scan or random temporary-file buildup is needed.
This bounded metadata
publication is separate from streamed transcripts and conversation storage.
Paths are limited to 4095 bytes and names to 255 printable UTF-8 bytes without
slash, C0/C1 controls, or the special names `.` and `..`. Empty means unnamed.
Names may contain spaces and non-ASCII characters; escape them when displaying.

`--workspace-name`, `[server] workspace_name`, the application-prefixed
`WORKSPACE_NAME` environment alias and native `cai_workspace_server_config.name`
all feed that publication. A separate `nm-<name-hash>.lock` reserves each name
for its owner's lifetime. Both workspace and name locks are native OS locks:
no timer, renewal thread, independent Pouch client or stale PID file is involved.
Lock files remain to avoid split-inode ownership. A crashed process releases
its locks automatically; stale metadata never prevents a new owner publishing.

`workspace_registry.h` exposes one-record-at-a-time hint iteration, with fixed
heap metadata and a 512-byte read buffer. It validates file ownership, private
regular-file type, bounded size, CBOR/schema, strings and the path hash against
the filename. Invalid records return an error and advance rather than blocking
the rest of a listing. It does not gather all entries or open session stores.
Explicit pruning takes the corresponding workspace lock nonblockingly and
removes only the unchanged current metadata inode. Owner close similarly
removes only its own publication before releasing endpoint ownership. Neither
operation unlinks ownership locks, and pruning never removes a socket. Native
tests cover live-name conflicts, process-death recovery, changed-inode safety,
malformed/unsafe records and default-stack worker use. The headless CLI tests
verify flag/config/environment precedence and compare registry instance IDs
with a real socket probe. Terminal selector commands and workspace switching
use the same verified discovery through the
[terminal adapter](terminal-attachment.md).

Selector attachment must bind its selected server instance before the host
admits an attachment. Merely comparing the later WELCOME is insufficient: an
owner may restart between probe and attach, and rejecting the new WELCOME
afterward would already have acquired/released control and affected a
nonpersistent server's lifetime. The core and typed app clients now accept
`expected_instance`, copied at open. The host checks it in HELLO before
admission; clients also check the complete reply. A mismatch never acquires
or releases an attachment. NULL selects the current canonical workspace owner
without pinning; guarded reconnects still require producer/request reconciliation.

`workspace_discovery.h` now provides owner-thread prepare/pump lookup and
listing. No runtime, renderer, storage client, process or worker is opened.
It reads one candidate at a time and performs an identity-only probe, pinning
the instance from a registry hint. STARTING and READY are verified outcomes;
STOPPING, timeouts and changed instances cannot become successful selections.
Passing no selector lists verified entries through borrowed callbacks. Lookup
matches exact names and complete stable IDs without precedence; if a name
matches another workspace's ID, AMBIGUOUS returns both live identities. Paths
must be explicit (`/`, `./`, `../`, `.`, `..`, or a slash-containing path);
relative paths require an absolute directory context. Bare names never fall
back to the caller's cwd. Path lookup bypasses the registry and verifies the
canonical endpoint directly. It creates no directories or server processes.

The resolver retains only one result and one borrowed candidate, and reuses
one probe slot. Local scanning requests a due timer; socket work contributes
only its readiness/deadline. Completion has no descriptor or timer. Close
cancels discovery without affecting server lifetime. Per-probe timeout zero
selects ten seconds. Malformed/unsafe metadata is an inline observation and
does not hide healthy entries; underlying directory I/O or allocation failure
terminates the operation. A failed probe may prune its unchanged hint under
the workspace lock; a live or changed owner is preserved. MISSING, AMBIGUOUS
and UNAVAILABLE are typed outcomes rather than failures of the host. FOUND
returns borrowed canonical identity, instance, phase and persistence; copy or
use them before closing the resolver. Pin that instance in the subsequent
client greeting so a restart between discovery and attach stays harmless.

Attaching never overwrites the running server's configuration. Frontend flags
apply locally. Explicit inference settings are ordinary acknowledged backend
controls. Incompatible explicit server/store/config identity reports which
server is already running and which setting conflicts. Do not silently start
a second owner or mutate config.ini.
The implemented [startup requirement inventory](workspace-wire-claims.md)
validates these identities before admission, without publishing configuration
values in discovery. Full runtime startup-control parity remains unfinished.

## CLI contract

| Invocation/action | Behavior |
| --- | --- |
| `cai -x`, `cai --headless` | Start this workspace server in the foreground, without a terminal frontend, with persistence enabled by default. Fail clearly if already served. Log JSON through libpslog to stdout/stderr with normal automatic color detection. |
| `cai` | Discover and attach. If absent, launch a quiet server by exec of this executable, then attach after an explicit READY handshake. Server logs use the normal file policy, with no inherited terminal stdout/stderr. |
| `cai -s SELECTOR` | Attach to an already-running workspace by name, path or stable ID. A name never invents a new directory or starts in the client's cwd. |
| `cai -l`, `cai --list` | List saved conversations and session IDs for the selected current directory. Do not launch an agent server. |
| `cai -L`, `cai --list-all` | List saved conversations across directories in the entire configured session store. Do not launch an agent server. |
| `--workspace-name NAME` | Assign the workspace discovery name when starting its server. |
| `/workspace` | List available running workspaces and identify the current one. |
| `/workspace SELECTOR` | Select an existing owner after verified discovery and handshake. Retain visited attachments until client quit; do not stop either owner or carry accepted inputs across workspaces. |
| `/takeover` | Explicitly acquire control, revoking the previous controlling attachment. |
| `/quit`, Alt-Q, terminal EOF | Detach; Alt-Q persists the local draft to this conversation's prompt history before detach. The last detach shuts down a nonpersistent server; a persistent server continues. |
| `/shutdown` | Controller-only orderly shutdown, distinct from detach and goal pause. |
| `-R`/`--resume`, `/new`, `/resume` | Select a conversation inside the selected workspace server; queues remain per conversation. |

Help, version, config dump, Chrome native messaging/registration and standalone
maintenance utilities do not spawn an agent server. Existing noninteractive
output remains without interactive margins or receipts on stdout. A batch
client waits for its own acknowledged work, reports its outcome and detaches;
it must not confuse unrelated ongoing work with completion of its submission.

`[server] persist = false` is the normal configuration default, also available
as the application's `SERVER_PERSIST` environment setting (`CAI_SERVER_PERSIST`
for cai) and `--persist` / `--no-persist`. Headless startup defaults to
persistence; explicit INI, environment and argv settings take precedence in
that order. The generated template keeps the normal false default.
An attaching client never changes an existing server's
persistence implicitly. Explicit server shutdown remains available in either
mode. Count all attached clients, including observers, for last-client lifetime.

Temporary workspaces have an explicit ephemeral server lifetime. Detach from
their last client first stops and joins that server, then performs the
existing default-no cleanup prompt after Softline returns. Observers cannot
delete a live workspace; `--yes`/`--force` retain their explicit cleanup meaning.
Do not leave an invisible temp server in a directory just deleted by a client.

Interactive `cai_app.run` resolves configuration once for both owner and
frontend. Temporary state/cache/data remain under the unique cache tree;
socket discovery uses a separate private `t-XXXXXX` directory under the
configured runtime root, avoiding the additional deep workspace path in Unix
socket names. Both directories belong to the run. Persistence is disabled
even if inherited or explicitly requested for that temporary invocation.
Owner/store/reader shutdown and frontend join precede the plain cleanup
question. Temporary socket metadata, locks and the private conversation tree
are removed before asking; only workspace files may be kept. The host run stays
stoppable throughout confirmation. A startup failure
never waits for an answer; `--force` still deletes its owned workspace.

## Control ownership and request outcomes

The first attachment acquires control. Subsequent attachments observe. A lost
controller releases its role, but does not automatically give authority to a
viewer; takeover is explicit. A new attachment may acquire vacant control.
Takeover advances a server-owned control generation and broadcasts a role
change. Every mutation carries that generation; pending unadmitted requests
from the previous generation fail explicitly. Already durably admitted work
belongs to the conversation and continues. A disconnected editor's holds are
released through normal queue transitions without submitting its local draft.

Viewers can request status, replay, catalogs and attachment-local settings.
They cannot submit/steer, change model/goals/conversation, edit/approve queues,
run shell commands or shut down the server. Do not trust a client-side disabled
widget to enforce this. Graceful detach and abrupt socket loss use one cleanup
path. The Softline adapter retains local editing while displaying read-only
observations for a viewer; it must not pretend a rejected offer was queued.

Requests have stable attachment identity, monotonic request ID, selected
conversation epoch and control generation. Distinguish received, durably
accepted, applied, rejected and unknown outcomes. Correlate all errors with
the request. Reconnect reconciles uncertain submissions using their original
identity and receipt; it never blindly reoffers text with a new ID. Existing
input queue IDs/revisions/edit tokens remain authoritative. Extend their
resumable producer/receipt contract where required rather than maintaining a
second deduplication layer that can execute input twice.

The core queue now retains 16 results for each active producer and a cache of
the 16 most recently used detached producers. Reattaching a retained identity
does not append a CLIENT record. Only one handle may own that producer at once.
Recovery creates detached receipt state from ordered CLIENT/REQUEST records;
live handles cannot survive a crashed server. Expired receipts and IDs omitted
from a trim/export initialization baseline stay unavailable, rather than
becoming new submissions. Frontends retain their own last-issued request ID
and reconcile unknown work before making any replacement offer.

The input controller resumes a producer only against its verified conversation
epoch. Producer lookup runs on the owner; an unknown or already-attached
identity disconnects that connection with an actionable reason, without
reserving a replacement or failing other clients. Across a process restart the
host must verify the persistent session identity before using its new epoch.
Historical receipts are looked up before current authority: an observer can
learn whether its earlier prompt was accepted, but cannot admit new work.
Authority rejections are durable receipts without the rejected text, so gaining
control later cannot cause the same rejected request ID to execute.

## CBOR and framing

The workspace host accepts `HELLO` with zero metadata and a definite three-item
payload array: canonical workspace as bytes, the text schema identity
`cai.workspace`, then NULL or the expected 16-byte instance as bytes. Identity
fields are compared as chunks arrive, without gathering the
greeting. Only successful record completion validates it. A completed greeting
may wait while the application is STARTING; it acquires no authority until
READY. Incomplete greetings expire after ten seconds. All connection slots,
including unfinished greetings, count toward the bounded transport capacity.
When full, the listener is excluded from readiness interests to avoid spinning
on connections the host cannot accept yet.

Discovery uses `PROBE`, with zero metadata and a definite two-item array of
canonical workspace bytes and schema text. It never becomes an attachment. A validated probe
receives one `IDENTITY` record with zero metadata and a definite five-item
array: canonical workspace bytes, schema text, 16-byte server-instance bytes,
persistence boolean and startup phase (`0` STARTING, `1` READY, `2` STOPPING).
The server closes that connection after draining its bounded reply. Probes
consume transport slots and retain the greeting deadline while draining, but
never acquire authority, advance generations or change attachment counts,
first-attachment grace or last-client shutdown. They cannot carry controls or
application requests. Already accepted probes may report STOPPING; a stopping
server accepts no new connections.

Native callers set `cai_workspace_client_config.probe` and use the ordinary
prepare/pump loop with an explicit handshake timeout. Only validated COMPLETE
publishes `has_identity`, `server_phase`, instance and persistence. Successful
completion becomes terminal `CAI_WORKSPACE_IDENTIFIED`, with no descriptor or
deadline; no runtime, storage or worker is opened. Readiness is a snapshot,
not a subscription. A later attach revalidates the owner through its ordinary
greeting, and must not treat registry hints or a past probe as authority.

`WELCOME` assigns the server-owned attachment/generation metadata. Its definite
three-item array contains the canonical workspace bytes, a random 16-byte
server-instance identity and the selected persistence boolean. A following
`ROLE` record carries the controlling attachment ID as an unsigned payload;
zero means vacant control. Clients verify the complete welcome identity before
submitting requests. A new instance requires explicit reconciliation, rather
than treating an old attachment/generation as valid after restart.

Client `DETACH`, `TAKEOVER` and `SHUTDOWN` require null payloads and zero
conversation/sequence/stream metadata. The actual connection identity must
match the attachment header; the header can never impersonate another client.
`TAKEOVER` checks the original generation at admission, then explicitly changes
control and broadcasts the new role/generation;
`SHUTDOWN` additionally requires current controlling authority. These controls
take effect only after validated record completion. `REQUEST` and
`SYNCHRONIZE` payloads remain incremental application callbacks; applications
must classify mutations, validate payloads and recheck authority at actual
admission. A COMPLETE callback is still not a durable acceptance receipt.

The socket client has no default connection/greeting deadline: restoration can
take time while its owner loop remains responsive. Hosts can configure an
explicit timeout. It preserves the caller's original request generation rather
than refreshing pending mutations after control returns. Reconnection, instance
reconciliation and process startup remain explicit host responsibilities.

The owner uses the host's poll interests and absolute monotonic deadline,
calling a bounded nonblocking pump after readiness or timer expiry. There are
no per-connection workers or idle timers when no deadline exists. A subscriber
that exceeds its bounded output backlog is disconnected, without blocking
other attachments or cancelling accepted runtime work. The application must
finish orderly worker/storage shutdown before closing endpoint ownership.
An open outgoing record may be flushed before its producer finishes. Role
notifications wait behind that record in a fixed queue of 16 small snapshots;
only consecutive broadcasts may coalesce, never request acknowledgements.
Overflow disconnects that subscriber. No payload accumulator is introduced.

Use the [RFC 8949](https://www.rfc-editor.org/rfc/rfc8949.html) data model with
a deliberately small profile: unsigned and
negative integers, finite binary64 floats, booleans, null, UTF-8 strings, byte
strings, arrays and maps. Binary64 carries native usage/credit estimates
without decimal rounding or locale conversion; signed zero and subnormals
retain their exact bits. Reject half/single precision, NaN, infinity, tags and
unsupported simple values explicitly. Envelope metadata remains integer-only.
Text validation works across input boundaries, including truncated/overlong UTF-8, surrogates
and code points above U+10FFFF. Nesting is bounded. No C-string assumptions for
byte/text chunks. CBOR negative integers carry the unsigned argument directly,
so the codec can represent the complete 64-bit CBOR integer domain.

POSIX paths and runtime data fragments use byte strings: filesystem names need
not be UTF-8, and producer fragments can split a Unicode scalar. Use text
strings for validated labels/identifiers. Preserve exact bytes until the
frontend applies its existing rendering/sanitization policy.

The codec accepts successive byte fragments and emits borrowed tokens/chunks
while consuming them, without a DOM or retained value. Definite strings can
emit many data chunks. Indefinite strings accept definite chunks of the same
type; each text chunk is independently valid UTF-8. Definite and indefinite
arrays/maps are supported; reject breaks outside indefinite containers and
maps ending between key and value. Finalize detects every incomplete header,
body or container. Callback/transport failure stops further consumption and
propagates the original failure. Writer calls synchronously emit tokens to a
borrowed sink, without concatenating an object or backpatching its length.

Each wire record has a four-byte big-endian payload length followed by one
CBOR item. Maximum payload is 128 KiB. The record length is derived from known
field/chunk lengths; never serialize into a temporary buffer merely to count.
Large or unknown-length output is successive records carrying bounded chunks
(normally 32 KiB), with begin/end and stream identity. Model deltas need not
be merged to reach that size. The framing decoder accepts arbitrarily split
headers and bodies and rejects zero, oversized, truncated or multiple-root
payloads. Outbound readiness retains only bounded unsent chunks, not an entire
replay or export. A short send, EINTR, EAGAIN, EOF and SIGPIPE are handled.

Each message is a definite eight-pair map with ascending integer keys:
0 kind, 1 request ID, 2 conversation epoch, 3 control generation, 4 event
sequence, 5 stream identity, 6 attachment identity and 7 payload. Metadata
values are unsigned integers, zero when inapplicable. Payload is one CBOR item;
its schema is specific to the kind. Require the exact envelope shape/key order;
reject duplicates, missing keys and unknown keys. Workspace and server-instance
identity belong in the greeting payload. An attachment field supplied by a peer
does not authenticate that peer: admission uses its bound connection identity.

Semantic kinds include HELLO/WELCOME (attachment handshake), DETACH,
TAKEOVER/ROLE, REQUEST/
RESULT, STATUS, QUEUE snapshot/items/end/change, EVENT, REPLAY begin/item/end,
stream begin/chunk/end, synchronization required and shutdown. Unknown
operation/required schema fails explicitly. Do not introduce pre-1.0 version
negotiation, compatibility translation or multiple protocol generations.

The typed decoder emits HEADER, borrowed PAYLOAD tokens and then COMPLETE, only
after the wire decoder validates the complete record. This is a parse boundary,
not a durable-admission receipt. Provisional tokens may
prepare a bounded command; they must not execute it. Producers derive framing
lengths directly from metadata and known payload fields/chunks. They emit the
envelope and payload without a sizing serialization. Large logical values use
successive records, and do not require a payload accumulator.

The greeting identifies the implementation schema, server instance, canonical
workspace and capabilities so a rebuilt client cannot misinterpret a still
running incompatible server. Equality is a validation contract, not a promise
to support old schemas. Authentication is Unix peer identity plus private
endpoint ownership in this phase; no bearer credentials appear in discovery.

## Replay, live output and backpressure

Transmit semantic events and original timestamps: assistant Markdown,
reasoning summaries, tool invocation/result/failure, actual subprocess output,
review/subagent identity, compaction, cancellation and diagnostics. No rendered
ANSI, terminal cursor commands, theme escapes or pslog debug stream on the
wire. Clients use the existing formatting policy, including tool output limits,
theme offsets, command duration and one timestamp per entry.

Attach captures a durable replay watermark and queues subsequent live events.
Replay uses bounded collection range reads and streams records to the client;
then release live events after that watermark exactly once. Respect the
existing replay-turn selection and trim semantics. Do not read full history
twice, construct a full response, or add recurring full snapshots. Context
restoration belongs to server startup, never attachment.

Bound subscriptions by both count and bytes. A slow reader cannot block agent
execution. On a burst, drain queued complete events and use the retained reader
for bounded native catch-up. If it fails or stops draining, close that live
subscription with a synchronization requirement and cursor; preserve admission
outcomes and accepted work. A
reconnecting client requests a consistent snapshot/replay, and distinguishes
server restart from a transient transport loss. Do not silently skip output
and continue as if its transcript were complete.

## Commands, completion and terminal ownership

Retain one command classifier and policy surface. Backend commands retain
their established request/feed/turn boundaries and go through the durable
per-conversation queue. Queue receipts are not a second human-visible
submission echo. Local frontend actions such as paging, clipboard and theme
do not create model turns. Backend-facing status is structured data; libmdf
presentation is produced at the frontend.

Softline is configured with the remote queue callbacks. Callbacks only copy
requests and signal readiness; never synchronously wait for a server result.
Use Softline's own FIFO, editable state and approval semantics through the
existing controller contract. Keep a local draft distinct from accepted work.
Escape cancels/pauses through backend controls; it does not stop the server.

Completion catalogs arrive asynchronously and callbacks read a local cache.
Slash names/aliases and skills are advertised; file completion uses the
server workspace and policy, not the client's cwd. Softline 0.16's synchronous
completion callback must not perform socket round trips. If refreshing an
already-open completion needs an unavailable public Softline hook, report
that upstream; do not replace its editor or mutate its draft behind it.

Client-owned `less`, pager and interactive shell handoff suspend only that
frontend. Agent managed exec remains server-owned with its pipe/PTY semantics.
Any explicit terminal bridge uses bounded data and acknowledged size/control
messages. Do not pass the client's terminal descriptors to runtime ownership
or run a pager on server stdout.

## Implementation and proof

1. Land and test the reusable incremental CBOR codec and framed transport.
2. Extract application owner lifecycle and semantic frontend observations.
   Preserve one policy path; remove UI ownership from server execution.
3. Bind controller, roles, reconnect-safe queue identities, discovery and
   executable launch. Add typed C/Lua client/server access.
4. Move Softline/libmdf behind the client attachment and implement CLI
   discovery, switching, detach/takeover/shutdown and local utility handling.
5. Run normal Debug tests and affected focused CLI/application tests. Exercise
   the rebuilt static x86_64 musl executable. Do not substitute the full release
   matrix for focused proofs or enlarge worker stacks.

Codec tests use published RFC vectors, every split point and byte-at-a-time
delivery, integer/length boundaries, UTF-8 boundaries, invalid headers,
truncation, nesting, malformed indefinite strings/maps and sink failures.
Prove a large generated value reaches the consumer before its producer ends,
using bounded memory and a default-size musl worker stack.

Transport/controller tests cover short I/O, disconnect mid-header/body/result,
oversized input, slow viewers, bounded backlog, takeover races and stale
generations. Reconnect tests cover acknowledgement loss and demonstrate no
duplicate input execution. Discovery tests cover simultaneous first launch,
canonical aliases, unsafe paths, stale/live endpoints, permissions, path-length
errors, names and configuration conflicts. End-to-end tests demonstrate:

- A persistent server continues active work after detach, and reattach restores
  transcript/status without reopening the runtime or duplicating inference.
- A nonpersistent server stops after the last client disconnects, including an
  abrupt connection loss; earlier detaches leave it running for other clients.
  Auto-start/first-attach races and abandoned startup have bounded lifetimes.
- One controller and an observing client receive the same ordered events;
  unauthorized mutation fails, and explicit takeover changes authority.
- Workspace/conversation switching preserves each queue and local draft.
- Steers reach the proper safe boundary, including reviewer/subagent work.
- Headless mode needs no terminal; plain startup auto-launches quietly;
  startup failure is reported, not mistaken for a ready server.
- Cancellation, goal pause/resume, compaction, tool errors, import/export,
  prompt history, temporary workspace cleanup and orderly shutdown preserve
  their observable contracts.

A liblockdc push streaming blocker is reported as such. Codec/transport tests
must not claim to establish end-to-end storage streaming until that public
dependency API has been upgraded and verified.
