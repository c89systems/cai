# Workspace queue payloads

Application binding for the existing `cai.workspace` envelope. No independent
protocol version is introduced. Integers are unsigned; booleans use CBOR
booleans. Strings below are bounded byte strings without embedded NUL, preserving
native input bytes without requiring a complete JSON value. Input text is at
most 66,048 bytes; session IDs 128, origins 127, result reasons 191. An absent
input/replacement is CBOR null. Arrays are definite with the exact listed shape.
The application frontend queue remains bounded to 128 entries.

The decoder may retain one bounded input command or queue entry. It never
collects an entire snapshot. The encoder emits fields directly, deriving frame
lengths from those fields, with no preliminary serialization.

| Envelope kind | Payload |
| --- | --- |
| SYNCHRONIZE | `[producer, session, managed_delivery, select_session]` |
| INPUT_SETTLED | `[after_request]` |
| REQUEST | `[operation, item, item_revision, queue_revision, edit_token, intent, enabled, target_request, text]` |
| QUEUE_BEGIN | `[producer, last_request, revision, count, stopped, auto_approval, uncertain, session]` |
| QUEUE_ITEM | `[index, id, revision, client, request, edit_owner, edit_token, intent, held, edit_held, ready, stopped, action, independent, origin, text]` |
| QUEUE_END | `[count, revision]` |
| QUEUE_CHANGE | `[type, index, previous_view, client, request, producer, last_request, revision, count, stopped, auto_approval, uncertain, ENTRY...]` |
| RESULT | `[category, producer, last_request, outcome, code, item, item_revision, edit_token, reason]` |

`ENTRY...` is the fifteen QUEUE_ITEM fields after its index. Queue kinds carry
the captured conversation epoch and view revision in envelope conversation and
sequence. RESULT carries the original request ID/conversation; its sequence is
zero. Result categories are 0 for an input receipt, 1 for binding rejection, and
2 for an input observation reset, 3 for a connection-control outcome, and 4
for a [prompt-save result](workspace-wire-history.md).
Reset uses request zero. A control result does not advance producer request
watermarks or update the accepted queue. Input operations
follow `cai_input_operation` through RESOLVE; the trusted internal
FORGET_ORIGIN operation is unavailable on the wire. Intent and outcome values
use their native enums. Frontends cannot provide provenance origins.

SYNCHRONIZE with `select_session = false` binds an attachment to a logical queue producer. Producer zero
explicitly opens a new producer in the active conversation. A nonzero producer
must provide the exact persistent session identity. A nonzero conversation
epoch must also match; after verifying a changed server instance a client may
use zero to resolve that session's new epoch. Failure must never silently
reserve a replacement producer. Binding is read-only with respect to accepted
inputs. A validated greeting alone neither binds nor submits anything.

With `select_session = true`, producer and conversation must both be zero, and the attachment must be unbound and controlling the workspace at the supplied authority generation.
The session field is a session ID or a workspace listing number; an empty session requests a new conversation.
The owner resolves and selects it outside the socket decoder callback, then binds a new producer and returns the ordinary queue baseline.
Failure returns BIND_REJECTED without subscribing to or replaying any conversation, and an invalid selector does not close the current runtime.
Selecting the already active conversation is a silent no-op before binding, even if work is active; selecting another conversation rejects active work.
This is startup control, with no generated slash command, queue offer, prompt-history entry or command transcript record.
The binding payload has changed; restart existing workspace owners when installing this build so clients and owners use the same schema.

Managed producers start with delivery gated. Each handed-off queue request
closes the gate again. INPUT_SETTLED has zero request/sequence/stream metadata,
and the original conversation and authority generation. It is ephemeral
availability, not a mutation or receipt: all pending requests and accepted
receipts awaiting dependent requests must settle before it is sent.
The controller permits delivery only after draining requests at exactly the
announced processed watermark. Stale epochs/generations cannot release a new
binding; unavailable or revoked producers do not gate other producers. Drafts,
cursor state, history search and keystrokes stay entirely in the frontend.
See [producer-settled delivery](workspace-producer-delivery.md).

REQUEST uses the bound producer, the original authority generation and the
original request ID. It may be admitted only after full record validation.
Durable input receipt lookup precedes current authority, while all new
mutations require authority at owner dispatch. The wire adds no second input
deduplication log. SYNCHRONIZE binding request IDs and input request IDs belong
to their different operations; RESULT.category distinguishes their outcomes.

A snapshot is QUEUE_BEGIN, individually emitted QUEUE_ITEM records in order,
then QUEUE_END, all at one view revision. There is no combined snapshot frame.
The initial baseline carries the binding request ID throughout. Subsequent
baselines carry request zero; their individual SYNCHRONIZE receipts retain the
input request IDs. A newer resync cannot relabel an already-open baseline.
Consumers publish their editor mirror only after END. A mutation while the
producer is reading that baseline invalidates it: reset precedes a new BEGIN.
Partial snapshots on disconnect are never committed. Changes after END use
previous_view to detect missing observations. A slow socket gates production
using bounded transport backlog and descriptor readiness, not polling or
whole-snapshot buffering; accepted model work remains independent of it.
This reset/restart also applies before the initial binding's first END; it is
not a binding rejection. A reset with a nonzero error code terminates only
that observation binding and permits an explicit rebind. A zero conversation
reset means the host has unbound the old conversation. Neither case submits a
draft or selects a replacement producer automatically.

Session switching unbinds old socket input connections before runtime
destruction. Requests never move to a different conversation. Reattachment
reconciles original producers and receipts; it does not automatically resubmit
uncertain text. Per-conversation queue ownership and the original generation
rules remain unchanged.

CANCEL carries a null payload, nonzero connection-scoped request identity, the
original authority generation and active conversation epoch; sequence/stream
are zero. The owner checks authority and epoch immediately before cancelling
foreground work, outside the trusted host mailbox. The last identical request
returns its cached outcome; changed or older identities are rejected. A replayed
cancel cannot affect a later turn. Detach and shutdown remain separate operations.

Every control-generation change releases the previous generation's edit
capabilities before admitting new work, including native input connections
authenticated as those attachments. Ordinary observer departure does not
invalidate the controller's edits. Disconnect cleanup and crash recovery use
the same incremental RELEASE journal record, with no frontend request ID.
Approval holds, stopped state and accepted text survive release; a local draft
is never submitted by cleanup. Failed/ambiguous release writes fence queue
mutation until authoritative recovery, like other journal transitions.

Focused verification lives in `cai_input_wire` (fragmentation, caps, malformed
values and completion), `cai_input_snapshot` (bounded production, revision
restart, isolation/allocation failure), `cai_input_queue` (release/receipt and
journal recovery), and `cai_app_workspace_server` (real sockets and inference).
`cai_app_client` additionally checks typed baseline/change validation, callback
and thread ownership, reset/rebind, partial EOF and consumer cancellation. The
workspace-server checks use this public client for real inference and producer
reconnect, and interrupt a large initial snapshot through another attachment.

The shared Softline adapter can now consume those public client observations
through `cli/input_socket.c`, using the same snapshot/change/result path as its
native connection binding. A retained editor maps its own request IDs onto its
durable producer namespace, preserves unknown outcomes across reconnect, and
settles original proposals after RESOLVE. It does not own a socket pump or
storage context. Its opaque item tokens remain stable across a verified runtime
epoch change while new requests retain the live protocol scope. The
`cai_input_socket` test exercises real socket/editor handoffs and the failure
cases, while [terminal attachment](terminal-attachment.md) describes the implemented terminal/process composition.
