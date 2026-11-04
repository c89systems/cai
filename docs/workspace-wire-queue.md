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
| SYNCHRONIZE | `[producer, session]` |
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
2 for an input observation reset. Reset uses request zero. Input operations
follow `cai_input_operation` through RESOLVE; the trusted internal
FORGET_ORIGIN operation is unavailable on the wire. Intent and outcome values
use their native enums. Frontends cannot provide provenance origins.

SYNCHRONIZE binds an attachment to a logical queue producer. Producer zero
explicitly opens a new producer in the active conversation. A nonzero producer
must provide the exact persistent session identity. A nonzero conversation
epoch must also match; after verifying a changed server instance a client may
use zero to resolve that session's new epoch. Failure must never silently
reserve a replacement producer. Binding is read-only with respect to accepted
inputs. A validated greeting alone neither binds nor submits anything.

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
