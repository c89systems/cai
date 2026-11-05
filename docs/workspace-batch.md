# Batch attachments and work identity

Batch mode must use the same workspace owner, admitted input queue and typed
Unix-socket client as the terminal adapter. It must not open a second inference
runtime against a live workspace, infer completion from a generic idle flag,
match prompt text, or retry accepted work under a fresh identity. Maintenance
utilities remain separate and do not start workspace owners.

## Queued turn correlation

For a non-command queued prompt claimed from the durable input queue, the
runtime's existing execution records identify the originating claim with:

`input:SESSION:PRODUCER:REQUEST:ITEM`

SESSION is the source conversation ID; the three numeric fields are the queue
entry's original producer, OFFER request and item IDs. These are backend-owned
identities, not user-supplied labels. They fit a fixed 256-byte internal field.
Clients know them from their accepted receipt and authenticated producer scope.
Edits preserve the original submission identity. Equal text submitted twice
still identifies different work.

With execution history enabled (`record_transcript`, always set by the app),
`turn.started`, `response.completed`, `turn.completed`, `turn.cancelled` and
`turn.failed` execution records carry this value in their existing `call_id`.
Tool call IDs retain their own identities. Operator commands keep the existing
`command:SESSION:PRODUCER:REQUEST:ITEM` convention. Direct runtime submissions,
startup automation, goal continuations and inline compaction do not acquire an
unrelated producer's identity. A steer joining an existing turn is not a new
queued turn and cannot replace its owner's identity.

The runtime carries provenance from its existing durable queue claim through
delivery to the worker. On restart it reconstructs the same identity from the
claim and its acknowledged local delivery. No new queue, receipt index,
snapshot, full-message accumulator, journal version or compatibility path is
needed. Undispatched accepted claims remain recoverable exactly as before.
An observed start is not completion. A storage failure or process loss without
a terminal record leaves the result unknown; it never implies success or
authorizes a new submission.

## Batch attachment flow

1. Resolve configuration and verify the workspace owner through common
   discovery/startup. Libraries never fork their host. A standalone executable
   may quietly launch the normal nonpersistent owner if absent.
2. Pin the discovered instance and validate startup requirements before
   admission. Fail when observing unless the caller explicitly takes over.
3. Bind a producer, subscribe with no historical console replay, and complete
   the replay/live watermark before offering work. Use the existing startup
   intent compiler for explicit controls and instruction ordering. Await each
   control's actual command outcome before sending dependent input.
4. Submit each instruction as queued work and await its accepted receipt.
   Track only its authenticated scope, request/item identity and bounded output
   cursors. Ignore another producer's turn and reviewer/worker descendants when
   determining the parent instruction's outcome.
5. Match the instruction's lifecycle records. Failure/cancellation is terminal
   for that instruction; successful terminal publication permits advancing to
   the next instruction. An active goal continues under backend policy, with
   explicit paused/achieved/failure outcomes rather than an invented prompt.
6. Produce the selected final assistant result without interactive margins,
   status rows or command receipts on stdout. Do not stage an unbounded final
   message in RAM or a private spool. A bounded native journal range reader
   supplies the final projection after its durable boundaries are known;
   diagnostics remain separate. Preserve explicit output-file behavior.
7. Detach after the invocation's work is settled. A persistent owner continues;
   last detach stops a normal owner. Disconnect or ownership/context loss must
   report a known failure or an unknown outcome, never silently reexecute.

Queued turn correlation, bounded final-response selection/projection, quiet
owner startup deferral and the prompt/goal batch adapter are implemented.
Standalone batch discovers or quietly launches the owner; `-s SELECTOR`
attaches to an existing one. Native `run()` uses the same calling-thread owner
and a default-stack batch adapter. Temporary runs share this composition;
noninteractive cleanup never asks a terminal question. Direct `--review`,
combined `-g` plus `-i` startup, and slash/shell `-i` instructions still use the
old receiver, so its removal and terminal-independent packaging remain cutover
work.

`turn.settled` follows the complete terminal record, including every failure
field, and carries its exact call identity, code and goal status at that
boundary. A client must not detach upon the initial `turn.failed` header and
lose the following diagnostic fields. Active goals continue through unowned
turns; the adapter installs the next selector at the accepted END before the
same socket pump can deliver another start. Child sessions and unrelated tagged
turns cannot complete that selector. A replacement goal may join an already
active unowned turn at its next response boundary; it does not wait for a start
record that predates attachment. Only post-cursor text is observed, and later
responses still replace earlier partial text. Explicit paused/blocked/limited
goals produce a failed batch result rather than waiting for an invented prompt.

After dispatching callbacks, trusted host controls and automatic pursuit, the
owner publishes `application.settled` when the runtime is quiescent and no ready
application input remains. `cai_agent_runtime_is_quiescent` checks the runtime
side under its lock; it does not claim an application's queue is empty or a
goal achieved. This ordered marker lets synchronous native batch runs include
already accepted host follow-ups without treating an independent idle-status
snapshot as completion. The latest direct/goal response is selected while
waiting; an explicitly accepted host follow-up can resume cancelled work under
the existing host API. Otherwise failures remain failures. Attachment lifetime
shutdown preserves the run's outcome, while an explicit host STOP retains its
STOP outcome.

Focused `cai_cli_batch` coverage exercises existing-owner controls, `/new` and
`/resume` handoff, rejected startup controls, observer rejection, explicit and
model-created goals, pausing, and connection loss after admission. The native
batch fixture checks default-stack callback affinity, one SDK client, untouched
host process state, restoration, temporary cleanup and startup failure. Existing
final-output tests cover both provider protocols and the last-response rules;
the host-control suite additionally checks follow-up work, failures and STOP.

Proof covers equal-text submissions, unrelated work before the claimed turn,
multiple producers, live and restored claims/deliveries, success, provider and
preflight failures, cancellation and following unowned work. Claims and outcomes
must retain their identity through existing streaming transcript/execution
channels. Existing callback-free runtimes must record the same boundaries.
Default-stack musl checks and bounded sink/callback failure cases remain part of
the contract. No test may substitute an idle-state observation for the matched
terminal record.

The correlation implementation is covered by `agent_runtime_input_identity`
(equal text, two producers, edits, joining steering, success/failure/cancel,
pre-inference rejection, and unowned turns). The fixture runs on a worker with
default pthread attributes, including musl. `agent_runtime_pending_input`
reopens a persisted claim and a journal cut immediately after local delivery;
both must publish the original identity. `cai_app_workspace_server` checks the
accepted OFFER's exact identity at the typed client's completed-turn boundary,
including producer reconnection and approval after an edit lease is released.
These prove correlation, not the subsequent batch adapter cutover.

## Streaming final-response projection

Core `cai/transcript_selection.h` supplies two owner-thread adapters, independent
of sockets, application owners, SDK clients, renderers and storage backends.
The caller gives the selector an accepted subscription cursor and its queued
work identity before admitting the prompt. The selector consumes ordinary
transcript spans, incrementally decodes bounded execution records, and matches
only that session's exact turn identity. Other turns, tools and child execution
records cannot complete the selected work.

The selector retains fixed cursors and byte counts for the latest top-level
assistant response. It never retains response text. Every completed provider
response replaces the selection, including an empty/reasoning-only response.
A failure or cancellation may retain the current accepted partial response.
Completion, failure and cancellation remain distinct results; a preflight
failure does not require a start record. Success does require one. Missing
terminal records never imply success. Span offsets, ordered identities and
accepted event boundaries are validated before publishing selection metadata.

After the matching terminal record, the caller may open a range projector with
that fixed result and a borrowed sink. For nonempty output it requests ordinary
cursor continuation from the range's `after` cursor, using the original session
and collection, then feeds the resulting replay to the projector. Only
top-level assistant text through the selected final position reaches the sink.
Other event kinds and later replies are skipped. Replay must contain the whole
selected range; collection/context loss, missing boundaries, changed lengths,
bad offsets and sink failures are explicit failures. Empty output is complete
without opening another reader. No retry or model input is generated.

Both adapters use bounded state regardless of response or history size. Text
is written as it arrives on the range pass; a failing source or sink can leave
partial output, as with other streaming APIs. There is no whole-message RAM
buffer, response spool, temporary file, hidden full-history scan or new wire
format. The caller owns subscription lifetime and stops it once projection is
complete. This projection is a prerequisite of the batch cutover; it does not
itself choose startup, owner creation, takeover or goal continuation policy.
