# Workspace servers and attachable clients

Implementation specification, 2026-11-04. This supersedes the transport order
in `frontend-architecture.md`: Unix sockets and CBOR come first; an optional
HTTP/Web UI adapter comes later. It does not claim that the cutover is already
implemented. The existing queue/controller foundation remains the baseline.

Current implementation progress: libcai now contains the incremental codec
(`cbor.h`), record/socket transport (`wire.h`) and canonical endpoint ownership
(`workspace.h`). Their tests cover streaming, bounded backpressure, native
ownership races and killed-owner recovery. The core attachment controller
(`attachment.h`) implements roles, generation-checked admission and persistent
versus last-client lifetime. Input-controller handoffs carry the original
authority generation and check it on owner-thread dispatch. Typed protocol
envelopes (`protocol.h`) stream metadata/payload tokens and distinguish
provisional data from validated record completion. Queue producers can resume
their retained receipts after disconnect or ordered journal recovery, without
another producer reservation. Immutable environment views now scope client
credentials and command/plugin tools; configuration accepts explicit directory/
environment contexts and produces effective views without process mutation.
The application controller/frontend cutover,
connection handshake and headless/attach CLI are still pending; these core
components alone do not enable those CLI modes.

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
  are permitted.
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
to attach must not leave a nonpersistent server running indefinitely. Once
last-client shutdown begins, a racing attachment receives a stopping response
and retries discovery after the old owner releases its endpoint; it does not
revive a partially stopped runtime.

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

Attaching never overwrites the running server's configuration. Frontend flags
apply locally. Explicit inference settings are ordinary acknowledged backend
controls. Incompatible explicit server/store/config identity reports which
server is already running and which setting conflicts. Do not silently start
a second owner or mutate config.ini.

## CLI contract

| Invocation/action | Behavior |
| --- | --- |
| `cai -x`, `cai --headless` | Start this workspace server in the foreground, without a terminal frontend, with persistence enabled automatically. Fail clearly if already served. Log JSON through libpslog to stdout/stderr with normal automatic color detection. |
| `cai` | Discover and attach. If absent, launch a quiet server by exec of this executable, then attach after an explicit READY handshake. Server logs use the normal file policy, with no inherited terminal stdout/stderr. |
| `cai -s SELECTOR` | Attach to an already-running workspace by name, path or stable ID. A name never invents a new directory or starts in the client's cwd. |
| `--workspace-name NAME` | Assign the workspace discovery name when starting its server. |
| `/workspace` | List available running workspaces and identify the current one. |
| `/workspace SELECTOR` | Reconnect this client; do not stop either server or carry accepted inputs across workspaces. |
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
as the application's `SERVER_PERSIST` environment setting and `--persist`.
Headless startup enables persistence rather than exiting before its first
client arrives. An attaching client never changes an existing server's
persistence implicitly. Explicit server shutdown remains available in either
mode. Count all attached clients, including observers, for last-client lifetime.

Temporary workspaces have an explicit ephemeral server lifetime. Detach from
their last client first stops and joins that server, then performs the
existing default-no cleanup prompt after Softline returns. Observers cannot
delete a live workspace; `--yes`/`--force` retain their explicit cleanup meaning.
Do not leave an invisible temp server in a directory just deleted by a client.

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

## CBOR and framing

Use the [RFC 8949](https://www.rfc-editor.org/rfc/rfc8949.html) data model with
a deliberately small profile: unsigned and
negative integers, booleans, null, UTF-8 strings, byte strings, arrays and maps.
Reject tags, floats and unsupported simple values explicitly. Text validation
works across input boundaries, including truncated/overlong UTF-8, surrogates
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
execution. On overflow, close its live subscription with a synchronization
requirement and cursor; preserve admission outcomes and accepted work. A
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
