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

The first implementation step establishes queued turn correlation in core
runtime execution history. The batch adapter and final-output range projection
are subsequent cutover work; this document does not claim the old noninteractive
receiver has already been removed.

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
