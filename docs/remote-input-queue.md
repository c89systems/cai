# Local and backend-backed input queues

Status: implemented queue authority and socket input binding, 2026-11-04. Public core headers
`input_queue.h` and `input_controller.h`, the Lua runtime attachment and
`cli/input_adapter.c` implement the authority and Softline bridge described
here. Each conversation journals its own queue. The terminal-free application
owner exposes in-process input connect/resume, owned observations and status
wakeups. Workspace sockets now carry input requests and queue observations
using the [wire payload contract](workspace-wire-queue.md). Transcript/command
presentation and the terminal client cutover remain future work in the
[attachable frontend architecture](frontend-architecture.md). Illustrative
C shapes below explain responsibilities; installed headers define exact APIs.
The typed `cai/app_client.h` socket client exposes borrowed incremental queue
observations without opening storage or materializing a baseline. Its explicit
bind/resume and original scopes use the same receipts and admission gate as
in-process connections.

Softline must retain its existing local queue as the constructor default. A
constructor may instead select an application-supplied remote queue backend.
"Remote" describes authority: the same adapter works with an in-process CAI
controller or one reached over HTTP. Softline never depends on CAI, HTTP,
liblockdc or a server thread.

## 1. Exact ownership boundary

| State | Local Softline mode | Remote/backend-backed mode |
| --- | --- | --- |
| Active unsubmitted draft, cursor, undo, kill ring | Softline | Softline |
| Accepted queued text, order, Q/S intent, holds and stopped state | Softline native queue | Backend queue |
| Native key bindings and queue/editor presentation | Softline | Softline, presenting a bounded backend mirror |
| Admission and execution readiness | Existing Softline profile plus host policy | Backend policy; Softline displays acknowledged state |
| Actual model input consumption | Host after native delivery | Backend directly; never redelivered by the mirror |
| Accepted queue persistence | Existing local application's choice | Backend journal, independent of frontend lifetime |

Local and remote are alternative authorities for one Softline instance, not
two independently executable queues in that instance. A remote instance still
has its local native editor and bounded display/edit cache. That cache is not
an offline execution queue. It cannot auto-approve, execute, dequeue or reorder
backend items on its own.

Backend accepted queue entries are distinct from prompt recall. Recall records
previous input for arrow-up/search; the queue records work not yet applied.
Do not use recall or its collection to recover queued execution.

## 2. Core libcai support

Expose a dependency-free pending-input receiver in libcai, reusing/refactoring
the runtime's existing normal/steering admission and consumption machinery.
This must be usable by a host that does not link libcai_app or Softline.
Do not put the generic queue feature solely in the terminal application, and
do not bolt another independently executable queue onto the existing ones.

The receiver provides identity/revision-aware operations for offer, inspect,
begin edit, replace/end edit, abort edit, approve, reject/withdraw, change
delivery intent, stop/resume backlog and claim/consume. It exposes bounded
snapshots and change observations. Actual public declarations must follow the
existing C89 receiver/init/cleanup and owned/borrowed result conventions.

One pending item contains:

- Stable backend item ID, item revision, ordering identity and originating
  request ID.
- Original/current accepted UTF-8 text, intended Q/S lane, approval hold,
  edit hold and backlog stopped state.
- Kind: model input or an opaque host action. Core does not parse slash
  commands; libcai_app classifies them and supplies their execution boundary.
- Current lifecycle and admission/consumption identity. Readiness is a published
  backend fact, not a frontend assertion that the model is between tool calls.

The application controller owns this receiver for its conversation and attaches
the same queue to the runtime. A standalone core runtime may create/own its
default receiver. The model loop and application dispatcher claim from that
one authority according to their respective responsibilities. A claimed model
input can be retained as active execution state; it is not another editable
pending entry. Host actions are claimed/dispatched on the owner thread after
runtime pumping returns, not from a callback that forbids runtime reentry.

Admission/changes/consumption persist through dependency-free journal callbacks
and the existing conversation journal. The app binds those to its Pouch adapter.
Publish success only after the corresponding durable transition when storage
is enabled. Direct core consumers can use their own store or explicitly volatile
operation. Softline sees neither the storage implementation nor its credentials.

Pure input acceptance must have a durable idempotency identity. Crashing after
admission but before its acknowledgement cannot cause an identical frontend
retry to create a second input. Consumption records use the same backend item
identity. Refactor the existing journal admission/consumption rules instead of
creating a separate full-queue snapshot or a second execution log.

Connections publish their logical producer and processed request watermark.
Reconnect uses that producer within the verified conversation epoch, keeping
the greater of the frontend's last-issued ID and the reported watermark.
Missing/evicted/already-attached producers fail explicitly; they never silently
reserve another identity. Result lookup precedes current authority checks, so
a former controller can resolve accepted work after takeover. New authority
rejections are journaled without payload text and remain rejected if control
is later restored. The application exposes this boundary through
`cai_app_connect_input` and `cai_app_resume_input`; returned connections may
retain observations after receiver shutdown, but no runtime or journal access.

libcai_app supplies command classification, authenticated controller roles,
transport request deduplication and the client bridge. It maps native inputs,
slash commands, incoming agent messages and HTML actions onto this queue API.
The generic core queue does not gain HTML, HTTP, Kore, application configuration
or special knowledge of `/model` and `/goal`.

## 3. Constructor-selected Softline backend

Proposed shape, using Softline's existing `sl_config_t`/`sl_create_with_config`
constructor style:

```c
sl_config_t config;
sl_config_init(&config);
config.prompt_queue = 1;
config.queue_backend = &queue_callbacks; /* NULL keeps native local queue */
config.queue_backend_context = cai_queue_bridge;
editor = sl_create_with_config(&config);
```

These constructor fields are available in Softline 0.16.0. The bridge contract is:

- NULL backend preserves local constructors, queue profiles, callbacks, keys,
  delivery and return semantics. No CAI behavior is imposed on other users.
- A backend selects remote authority for that handle before input begins.
  A backend with queueing disabled is rejected explicitly, not ignored.
- Copy the callback table/configuration at construction. Its context is borrowed
  until editor teardown; cancellation/unsubscription must finish before that
  context can be released. Destroying the editor detaches the bridge, not the
  backend application or its accepted queue.
- A callback table may have capabilities for optional operations; unsupported
  native actions report that fact while preserving the draft/item. No silent
  fallback to local queue authority on error or connection loss.
- Initially do not switch queue authorities under a live handle with accepted
  entries or an edit in flight. A session switch explicitly detaches/rebinds
  through a defined quiescent transition, preserving unsent drafts separately.

## 4. Two directions, not a synchronous fetch callback

One outbound request callback can dispatch a typed operation enum:

```c
int request(void *context, const sl_queue_request_t *request);
```

It runs on the editor owner thread. Request fields are borrowed for the call;
the bridge copies bounded fields before returning. It may enqueue work and
signal a descriptor. It must not do storage/network waits, wait for the backend,
reenter the editor or call an inbound update synchronously.

A successful callback return means only "bridge accepted this outbound request",
not "backend accepted the input". A failure means it was not handed off; Softline
preserves the draft/edit and displays a recoverable diagnostic. If the bridge
accepted it, its outcome must subsequently arrive as an explicit response.

The opposite direction is an owner-thread result/update API, conceptually:

```c
sl_queue_backend_result(editor, request_id, result);
sl_queue_backend_update(editor, change);
```

The adapter receives backend events through its existing pollable client fd.
It registers that fd using `sl_watch_add`, drains a bounded number of events,
and invokes these public methods on the Softline owner thread. No polling the
queue at an interval and no calling Softline from a network/controller thread.
The update methods copy borrowed fields, preserve native edit state and do not
invoke the outbound request callback again.

An initial bounded queue snapshot and its queue revision establish the mirror.
The producer emits BEGIN, one ITEM at a time, then END, with bounded receiver
credit. It does not allocate a full queue array. A mid-baseline mutation resets
the incomplete snapshot and starts a new revision; only END publishes the
frontend's explicit editor mirror. Allocation failure fences the affected
observation connection with a reserved terminal notice, preserving accepted
work and other consumers.
Subsequent deltas include the preceding/current revision so a gap is detectable.
A gap requests explicit snapshot resynchronization. Do not enumerate/refill the
whole queue on every update or retain a second unbounded response backlog.

## 5. Operations and required acknowledgements

Every mutating request carries a client request ID, target backend ID where
applicable, expected item revision and operation fields. Queue-wide mutations
also compare a queue revision. IDs used on the wire are opaque backend values;
they need not equal Softline's handle-local `sl_prompt_id_t`.

| Operation | Backend action | Native editor behavior |
| --- | --- | --- |
| Offer text | Durably accept text and requested Q/S intent, classify policy, return item ID/revision and final lane | Retain a provisional send record until acknowledged; continue editing the next draft |
| Begin edit | Compare revision and acquire an edit hold before claim | Open the native queued edit only after hold acknowledgement |
| Replace/end edit | Atomically change text/intent and release the edit hold under the requested approval policy | Preserve ID, queue position and normal native requeue behavior |
| Abort edit | Restore pre-edit state if still owned/revision-valid | Restore the native previous item; keep unsent edited text as a draft where appropriate |
| Approve | Remove human approval hold, respecting all other gates | Update held marker after acknowledgement; approval is not execution |
| Reject/withdraw | Remove the pending item if not already claimed | Remove its acknowledged mirror entry; stale claims report a conflict |
| Change/promote intent | Set S or Q through backend policy without changing item identity | Reflect final lane; a non-steerable command may be acknowledged as Q with a reason |
| Stop backlog | Stop automatic pending delivery through backend policy | Reflect stopped state; cancelling the current model turn is a separate ordered action |
| Resume backlog | Explicitly clear stopped state; approval/readiness still apply | Reflect backend state, preserving native stopped-queue interaction |
| Inspect/synchronize | Return a bounded ordered view and revision | Establish/reconcile the mirror without outbound offers or history auto-add |

Claimed, applied/consumed, failed and removed observations come from the backend.
A remote-mode `take()` cannot simply transfer text and grant execution authority
to the terminal. Any public method that currently mutates the native queue must
be explicitly defined as a remote request, read-only mirror operation, or
unsupported synchronous operation. It must never silently mutate only the
mirror. This includes clear-all and auto-approval/profile changes.

No queue reorder API is required unless a native frontend feature actually
needs it. Ordinary item edits preserve position. If clear-all is exposed, it
is one explicitly acknowledged queue operation, not a client loop hiding partial
failures from the user.

The existing policy callback remains useful for immediate local classification
hints, but remote policy/readiness is authoritative. Such hints cannot approve
or execute work, bypass a backend boundary or reinterpret an application command
as ordinary model text.

## 6. Submission without duplicate model delivery

In local mode, `sl_next_prompt()` keeps its existing direct/queued delivery
semantics and the application submits returned text.

In remote mode, Enter/Alt-Enter/queue keys use the backend request hook through
Softline's native input paths. Backend acceptance can happen while the editor
continues. The same accepted text must not later be returned to CAI as a fresh
prompt by `sl_next_prompt()` or `sl_prompt_queue_take()`.

Softline therefore needs a native handled-asynchronous-submission path that
keeps the editor and its fd watches active. It must not emulate this by returning
empty prompts, synthesizing Escape or restarting the editor on every accepted
submission. Native profile keys, approval UI and edit operations all use the
selected backend. The lifecycle callback reports detach/quit separately from
delivered model text.

Backend application generates the transcript prompt at its actual application
seam, with its stable identity. The remote mirror does not echo a second prompt
or create a second history entry when it receives that event. Startup/stream
presentation continues through the normal Softline output feed.

## 7. Races and draft preservation

An Enter action can optimistically clear the submitted draft after its bounded
text/revision is retained as a provisional request. It must display pending
acceptance. Acknowledging that request cannot clear or overwrite text typed
afterward. If acceptance fails, the retained text remains recoverable as an
unsent item; show a diagnostic rather than replacing the newer active draft.

Alt-E sends BEGIN_EDIT for an exact ID/revision. If the backend consumed it
first, the request fails and Softline does not display a supposedly safe edit.
If edit wins, the hold prevents claim. Preserve native cursor/undo behavior once
the edit opens. Nonempty requeue publishes one replacement under that identity;
empty submission or the native discard action requests withdrawal. A cancelled
queued edit aborts/releases the hold through the acknowledged backend operation.

On disconnect, ordinary accepted Q/S items keep their backend policy and may
run; an active edit hold remains held with its last accepted text. Unsubmitted
edits stay local. A new controlling attachment can explicitly resume/cancel the
hold after seeing current state. A timeout cannot silently execute an edited
item, and old disconnected-controller operations cannot mutate it afterwards.

Backend deltas can race a request response: an item can already be claimed or
consumed before its offer acknowledgement reaches the editor. Results and
updates reconcile through IDs/revisions; a late acknowledgement never recreates
a removed item or regresses a newer revision. Receiving one's own offer as a
subscription event is reconciliation, not an instruction to offer it again.

After connection loss, mark outcomes unknown rather than retry with a new ID.
Query/retry the original request ID, then apply the authoritative queue snapshot
and subsequent deltas. Offline requests do not auto-flush after reconnect until
their actual acceptance is resolved; definitely unaccepted drafts require a
deliberate submission. Queue/update size limits and overflow are explicit.

## 8. CAI bridge and execution seams

The bridge is a small libcai_app adapter over the public core pending-input and
application client APIs. It translates request/result types and IDs and supplies
the watch fd. It does not parse the terminal screen, scrape native queues,
execute actions itself or inspect private Softline/libcai structs.

For an in-process frontend, its outbound callback posts to the application
owner mailbox. That owner applies the core queue transition and durable journal
write, then publishes acknowledgement and delta. For a remote frontend, the
same application request passes through HTTP; the owner performs exactly the
same transition. Softline and the HTML frontend observe the same queue state.

The backend selects an executable item using the shared feed/request/turn policy,
approval state and stopped state. S entries are serviced at actual steerable
model/tool seams; Q entries wait for turns. Independent safe commands use their
declared boundaries. A turn-only S command is reclassified to Q explicitly;
it cannot hold later legitimate S work until the entire turn ends.

When there is no frontend, the owner still pumps runtime input boundaries,
services ready queue items, continues eligible goals and handles incoming agent
messages. A held item stays held. No callback to a missing Softline handle is
needed to execute backend work or acknowledge a message delivery.

## 9. C/Lua surfaces, delivery and verification

Provide the generic core queue receiver and callbacks through the public C API,
and expose its operations/results to the Lua runtime facade in the same change.
The application facade supplies local/remote clients and adapter composition.
Host Lua callbacks run on their owning thread through the existing dispatcher;
the controller or network thread does not call Lua functions directly.

Softline 0.16.0 provides the public constructor/callbacks and matching
result/update methods. The adapter must use those APIs and verify their actual
contract, including uncertain outcomes and native edit holds. This document
does not authorize editing another checkout.

Focused tests should prove:

1. An ordinary constructor still exercises all existing local queue behavior.
   A remote constructor invokes native keys against a mock asynchronous backend.
2. A blocked backend never blocks typing, resize, output feeds, cancel or fd
   readiness handling. No per-request thread, blocking policy callback, timer
   polling or enlarged stack is introduced.
3. Enter, Alt-Enter, empty-editor promotion, Alt-E, requeue, native discard,
   approval and stopped backlog preserve their defined behavior and identities.
4. Delayed/rejected acceptance preserves a newly typed draft; own echoed changes,
   duplicate callbacks and out-of-order results do not enqueue or consume twice.
5. The edit-vs-claim race has one winner. Stale revision, edit disconnect,
   controller handoff, reject-vs-claim and begin-edit failure preserve valid text.
6. Backend queue entries survive frontend destruction; a new Softline or HTML
   view reconstructs them without new runtime/session opens or automatic
   approval. Lost acknowledgements reconcile with the same request ID.
7. Direct core C and Lua hosts use the same queue without libcai_app/Softline;
   application host actions do not become model input; durable transitions
   survive crash at offer/admission/consumption boundaries.
8. No accepted remote entry is also delivered through the old direct-prompt
   callback. Detaching the frontend never sends STOP or clears the backend queue.

Use deterministic deferred acknowledgements and barriers, not timing sleeps or
provider inference. This is an upstream API dependency and a coordinated core/
app refactor, not justification for a CAI-side replacement editor.
