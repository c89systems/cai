# Terminal attachment

The normal interactive executable now discovers the selected workspace owner
and attaches through the native Unix socket client. `-s` / `--server SELECTOR`
selects an existing owner by its verified name, path or stable workspace ID;
it never invents a directory or starts a missing named workspace. Without a
selector, an absent owner is started quietly by the existing executable handoff.
The editor opens before the first identity probe and remains available during
discovery, restoration and the child's READY announcement. Explicit `-x` owners
persist; ordinary automatic owners stop after their
last attachment leaves unless persistence was explicitly configured.

`/workspace` lists verified live owners and identifies the current directory.
`/workspace SELECTOR` switches by name, stable ID or path relative to the
currently viewed workspace. Lookup and the target handshake run through
descriptor/deadline watches without blocking the editor. Missing, ambiguous or
failed selections leave the current owner available. Like takeover and detach,
this is a terminal connection operation, available to observers; it is never
submitted as model work or moved into another conversation's queue.

Visited workspaces keep their attachment, producer and native editors while
this terminal remains open. This also keeps an ordinary nonpersistent owner
alive while viewing another workspace. Parked attachments continue processing
queue and authority observations, but unsubscribe from presentation. Returning
requests the journal suffix after the last authenticated displayed position,
including accepted frontend actions that arrived during that absence. This is
continuation of the same subscription/producer, not reexecution of historical
commands. Quit detaches every visited owner; their configured last-detach
policy then applies. A persistent owner remains running.

Native hosts can use `cai_app_terminal_run` from `cai/app_terminal.h` with an
explicit transport target. It uses the same terminal implementation, resolves
frontend preferences and opens no runtime, authentication store or SDK client.
It never starts a process. The CLI's discovery/fork/exec scaffolding is separate
from this library receiver. Existing `cai_app.run` composition has not yet been
cut over; asynchronous server lifecycle APIs remain the backend entry points.

The adapter drives public client interests from Softline descriptor watches.
Wake bytes schedule socket progress after editor requests; they do not contain
input or create another queue. A timer serves transport/startup deadlines and
active elapsed-time display only; there is no idle polling timer. Role snapshots
make observers explicit. `/takeover` requests control and `/shutdown` requests
controller-authorized owner shutdown. `/quit` and Alt-Q detach locally even
when observing; editing an accepted queue item retains normal native semantics.

Enter and Alt-Enter use Softline's queued-turns profile, with the backend owning
admission, revisions, approval and delivery. Escape uses Softline CANCEL, then
requests cancellation of the remote foreground operation. Cancellation IDs are
separate from queue IDs. Exact duplicate cancellation acknowledgements do not
cancel a later turn; stale generations and observers cannot cancel work.

Each conversation retains its own complete editor and queue binding, including
draft, undo state, provisional receipts and edit state. Switching parks that
editor and unbinds its transport before selecting another. Returning resumes
the original producer and recall without moving input between conversations.
[Prompt recall](workspace-wire-history.md) loads through a separate bounded
channel on the same reader worker. Alt-Q waits for a durable draft-save receipt;
neither draft text nor stored recall enters model context. Explicit
startup inputs for an existing owner pass through the same editor/backend queue;
commands wait for application before subsequent inputs. A newly launched owner
receives the startup arguments itself, avoiding duplicate submission.
If simultaneous launches both observed absence, a losing child is reaped and
its claim-limit failure triggers one fresh identity-only lookup. A verified
winner is selected with its instance pinned; that terminal's own startup inputs
then follow the existing-owner queue path. Failure to verify preserves the
original launch error. No second launch, PID takeover or unbounded retry is
implied. Named discovery also replaces the editor's provisional directory for
status and file completions before accepting inputs.
Explicit startup identity now uses the shared
[pre-admission requirement inventory](workspace-wire-claims.md), including
private roots, stores, namespaces, config identity and integration/tool policy.
Conflicts identify the owner directory and setting before binding a producer;
native embedding can supply additional requirements through the same client.

Skill completion names arrive through the owner's
[catalog channel](workspace-wire-catalog.md). Only the bounded native completion
consumer retains those names; the transport never collects the catalog. The
frontend does no skill discovery or resource reads. `/`, `@` and `$` use the
same native completion providers and prefix-removal rules as the synchronous
receiver. Both receivers also use one shared help generator. Socket `/help`
runs through the backend command handler, records its Markdown reply in the
journal and adds connection operations; SDK consumers receive that same reply.

Transcript spans feed the common incremental presenter. Execution records feed
the common tool formatter in one pass, with a bounded completed-call metadata
cache. Only decoded subprocess output uses raw command presentation. Command
status and duration can arrive after its output, producing one final
`Ran COMMAND (TIME)` footer without a preparatory scan. Ordinary prompt and
assistant bodies are not collected. Accepted slash commands retain at most the
same 66,048-byte bound as input admission so their display and frontend actions
can be applied atomically. Initial historical replay and another producer's
actions never execute on the attaching terminal. A parked attachment's cursor
continuation only applies its own previously undisplayed actions.

Initial replay applies the frontend's configured last-ten-entry limit on the
owner, with zero and replay-all overrides. The owner reuses restoration/append
metadata and starts a native range read at the selected event; it does not scan
the entire journal for each attachment. This selection never truncates model
context. Returning to a parked workspace continues its accepted cursor without
reapplying the initial limit. Native trims remove those ranges from the owner's
index as well as storage; later attachments cannot redisplay the deleted entries.
Append bursts preserve ordered live delivery by switching a slow ring to native
catch-up through the existing reader worker. The normal path stays direct;
neither ring capacity nor the number of reader workers grows with output.
Reviewer reasoning uses the same incremental Markdown path during live display
and replay. Manual review headers come from the accepted backend command;
model-invoked review tools use the shared formatter's lifecycle presentation.

## Remaining cutover work

This is a working terminal/socket composition, not a claim of complete feature
parity. Remaining key bindings, pager/layout delegates, review progress parity,
immediate live command-progress events, full startup-control parity and
validation of extended configuration outside the startup inventory still require
integration. Automatic executable-path discovery currently
uses Linux `/proc/self/exe`. Batch/temp and the original synchronous app receiver
still use their existing composition. These remain part of the active goal.

Verification uses real Unix sockets, a local HTTP provider and native PTYs.
`cai_cli_attach` exercises named attach, observer rejection/takeover, conversation
switching, invalid selections, draft restoration, startup input, cancellation,
request-boundary steering, reconnect, formatted command output, quiet autostart
and last-detach shutdown. Its public-header-only embedding host aborts on any
attempt to open an SDK storage client. Separate tests cover cancellation
duplicates/authority, queue reconnect identities and one-pass output with late
status, sink failure and metadata-cache pressure on default-stack workers.
`cai_cli_workspaces` and `cai_app_terminal_workspaces` exercise the executable
and public terminal receiver across two actual owners, with isolated drafts,
observer takeover, invalid/relative selections, queued work continuing while
parked, suffix-only catch-up and nonpersistent/persistent detach lifetimes.
`cai_cli_attach_boot` verifies claim reconciliation and failure preservation
without a runtime/storage client, then exercises a stalled probe and two actual
terminal launches, distinct startup inputs and last-detach shutdown. Named
completion tests attach from another directory and resolve files at the owner.
