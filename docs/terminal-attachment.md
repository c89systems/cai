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
submitted as model work or moved into another session's queue.

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
It never starts a server process. Explicit local shell/pager actions use the
same PTY adapter as the executable. The CLI's discovery/fork/exec scaffolding is separate
from this library receiver. Interactive `cai_app.run` now
composes the same controller and socket frontend in-process: the calling
thread owns runtime callbacks and a default-stack worker owns the terminal.
It never forks an embedding host. Its frontend uses no second SDK client or
runtime. A separate lifecycle wake channel cannot consume the host observer's
status notification. Closing this owned frontend stops/joins its controller;
closing a terminal attached to an independently started owner obeys that
owner's persistence policy.
Temporary runs always use nonpersistent ownership and join both sides before
asking their default-no cleanup question. They resolve configuration once,
preserve selected shared auth/messaging stores, and remove their private
runtime discovery directory as well as their temporary history/cache.
The initial terminal displays the host's banner and configured Markdown
introduction through the shared renderer before replay, including introduction
margins and NO_COLOR. Switching workspaces does not repeat that introduction.

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

Each session retains its own complete editor and queue binding, including
draft, undo state, provisional receipts and edit state. Switching parks that
editor and unbinds its transport before selecting another. Returning resumes
the original producer and recall without moving input between sessions.
[Prompt recall](workspace-wire-history.md) loads through a separate bounded
channel on the same reader worker. Alt-Q waits for a durable draft-save receipt;
neither draft text nor stored recall enters model context. Explicit
startup inputs pass through the same editor/backend queue; commands wait for
application before subsequent inputs. A quietly launched owner resolves its
configuration and selects the initial session, then defers invocation
actions to the attaching frontend. The frontend skips already-applied settings
and session selection; goals and instructions receive ordinary queue
identities. An existing owner's explicit setting changes also use the queue.
Explicit headless startup keeps owner-side automation.
Attaching with `-R`/`--resume` or `-N`/`--new` selects through `cai_app_client_select` before producer binding and transcript subscription.
The owner resolves session IDs and listing numbers at this setup boundary; no `/resume` or `/new` input is manufactured.
An invalid selector leaves the current runtime open and returns a binding rejection before replay.
An observer cannot change the shared session, and active work prevents selecting a different session.
The shared invocation compiler translates explicit model, effort,
summary, service-tier, execution-yield, YOLO, reasoning-tool, shell-context,
effort-tool mappings, prompt-history, retention, automatic-queue, Kanban
layout and board-selection controls
into the same public commands used interactively, before submitting `-i`
inputs. Explicit false,
zero and provider-default selections are retained. Inherited defaults leave
live settings unchanged. A rejected control stops the remaining invocation
inputs; generated controls do not enter prompt recall. `/reasoning-summary`
queries or updates the runtime at the same request boundary as model/effort.
The compiler does not choose a second execution policy or bypass admission.
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
and replay. Its bounded first-line thinking title drives the active review
status; root thinking titles come from the transcript presenter. Manual review
headers come from the accepted backend command;
model-invoked review tools use the shared formatter's lifecycle presentation.

## Local terminal handoff

Alt-S opens a local shell while retaining Softline's draft, undo state and native
queue receipts. It is available during generation: presentation stops at the
next complete journal entry, while the workspace owner continues working.
`/shell` and `!sh` still pass through the ordinary turn queue. Enter followed by
`~.` escapes a local foreground program. Resize and interrupt bytes reach its
PTY. The program runs in the selected workspace with the resolved configuration
environment, without changing its embedding host's cwd or environment.

`/log -p` and `/log less` invoke an available `less` directly with the authenticated session log path.
Without `less`, libmdf reads the log on demand with a bounded rendered-content cache (1 MiB by default).
Seekable input lets it recover evicted pages without retaining the entire log.
Its cooperative cancellation hook keeps owner connections progressing and exits if the active owner fails.
No terminal callback reenters Softline queue/history/status APIs.
After return, the original producer synchronizes receipts and resumes from its displayed journal position.
Takeover or session changes during handoff retain their normal authority and context rules.
A focused real-terminal fixture holds the native help pager open until a paused provider finishes, then verifies that the unseen response appears exactly once after the pager closes.
Unshared shell bytes never enter model context.
`/shellshare` and `!sh+` explicitly opt into bounded-tail capture through the
[shell recording channel](workspace-wire-shell.md). They run the same local
PTY, with at most one 4 KiB chunk awaiting owner acknowledgement. Unshared
shells send no output over the socket. Local PTY lifetime belongs to the
frontend; the owner logs its accepted slash/bang commands.
Returning from a handoff reconciles the current session before entering
its editor again, including when a completed status update arrived before the
old producer's reset. The old view retains its local draft and queue receipts;
it cannot receive events belonging to the replacement session.
Completion notices are displayed after native queue synchronization, without
blocking the editor on that synchronization. Transcript catch-up starts after
the notice, preserving its seam. Feed restoration follows descriptor-mode
restoration so a large PTY burst cannot break setup writes on nonblocking
stdout. Saved shell tails use the same terminal-control sanitizer in live
presentation and replay; a local capture's acknowledged journal range prevents
duplicate display without suppressing unrelated assistant/tool entries.
The common presenter tracks whether its open Markdown document belongs to
tool/command output or assistant text. A producer change closes the previous
document before feeding the next, so an assistant reply cannot be consumed as
part of a tool's table. Fragment boundaries within one producer remain ordinary
streaming input and do not force document completion.

`/help -p` and `/help less` always use libmdf in Markdown mode, including when
`less` is installed. The same finite help source drives inline and paged help;
CAI retains paragraph cursors and a bounded startup-example buffer.
The help source is seekable: libmdf can revisit any byte offset without first collecting the document.
Paged help uses the same bounded rendered-content cache as logs, without a temporary file or model request.

`[ui] prefer_native_pager` defaults to false. Setting it through configuration,
the application-prefixed `PREFER_NATIVE_PAGER` environment variable, or
`--prefer-native-pager` bypasses `less` for log paging. The native log path
always uses `MDF_PAGER_FORMAT_TEXT`, preserving Markdown punctuation literally;
help always uses `MDF_PAGER_FORMAT_MARKDOWN`. `--no-prefer-native-pager` restores
external-first selection. Preferences belong to each frontend attachment.

## Implementation boundaries

This is a working terminal/socket composition, not a claim of complete feature
parity. Startup controls use ordered acknowledgements, and supplemental schema
configuration uses the same pre-admission inventory as resolved CLI settings.
Automatic executable-path discovery uses Linux `/proc/self/exe` or macOS
`_NSGetExecutablePath`, canonicalizing dyld's potentially relative or symbolic
path. It does not trust PATH or `argv[0]` when restarting itself. Batch prompts,
standalone goals, reviews, slash
commands, pipe-shell commands and combined goal-plus-prompt startup use the
owner/socket adapter. Plain redirected input without `-i` now shares that
producer and renders live output without an editor. Native `run --server`
uses verified discovery and the same socket frontends, without a local runtime
or storage client; STOP detaches only that client. The old combined readline
loop and unused terminal helpers are removed. The owner compiles without
Softline/libmdf include paths or editor/renderer references. Restoration calls
the host replay observer once; it does not render or construct another console
replay. Presentation tests use the actual shared transcript/presenter path.
The standalone [typed client package](application-client.md) requires only core
libcai and reuses the same codec objects as the combined application. The
combined application includes the terminal facade/adapters and depends on the
independent [workspace owner SDK](application-server.md). Owner lifecycle,
configuration and semantic command services have no renderer dependency.
Maintenance uses the separate [utility dispatcher](application-utilities.md); its logging/browser helpers use captured views without installing process state.
Both export formats read native journal ranges without context reconstruction.
Interactive exports use a retained worker so owner controls remain available; utility exports remain synchronous.
See [troubleshooting](troubleshooting.md) for current dependency limitations.

Softline 0.16's unconditional trigger stripping also needs a pre-activation
eligibility API: typed shell words starting with `$` (when skills are loaded)
or `@` can lose their introductory byte even without selecting a completion.
Empty skill catalogs no longer register `$`, preserving ordinary shell syntax
in that case. The remaining context-sensitive activation gap affects both
terminal compositions and is described in the local request
`stash/softline-completion-context-and-literal-triggers.md`.

The socket presenter now preserves reviewer/worker start receipts, relative
theme regions, cancellation notices and source headings in replay. Manual and
model-invoked reviewers share boundary titles. Operator queries do not reset a
running child's theme. Replay waits for actual streamed records and includes
failed manual preparation documents, matching what was displayed live.
Provider, turn and compaction failures, retries and compaction lifecycle notices
use the core structured execution renderer; their saved diagnostic fields do
not disappear when only the socket frontend is attached.

Verification uses real Unix sockets, a local HTTP provider and native PTYs.
`cai_cli_attach` exercises named attach, observer rejection/takeover, session
switching, invalid selections, draft restoration, startup input, cancellation,
request-boundary steering, reconnect, formatted command output, quiet autostart
and last-detach shutdown. Its public-header-only embedding host aborts on any
attempt to open an SDK storage client. Separate tests cover cancellation
duplicates/authority, queue reconnect identities and one-pass output with late
status, sink failure and metadata-cache pressure on default-stack workers.
Observer `/status` and theme commands are covered before takeover, including a
new attachment retaining its own original theme. The native server fixture
checks read-only command edits across an unrelated takeover, stale generations,
attempts to edit another producer's work or turn a query into model input, and
refusal of approval barriers/provenance keys/global queue controls. These checks
make no inference requests and retain the original store-client count.
`cai_cli_workspaces` and `cai_app_terminal_workspaces` exercise the executable
and public terminal receiver across two actual owners, with isolated drafts,
observer takeover, invalid/relative selections, queued work continuing while
parked, suffix-only catch-up and nonpersistent/persistent detach lifetimes.
`cai_cli_attach_boot` verifies claim reconciliation and failure preservation
without a runtime/storage client, then exercises a stalled probe and two actual
terminal launches, distinct startup inputs and last-detach shutdown. Named
completion tests attach from another directory and resolve files at the owner.
`cai_cli_attached_handoff_native` and `cai_cli_attached_handoff_embed` exercise
draft retention, active generation, resize, interrupts, local-program errors,
takeover, context changes and suffix-only replay across shell/pager handoff.
Both help and log pagers are tested with work completed while the alternate
screen is open: no transcript output enters that screen, and the missed output
appears exactly once after closing it.
`cai_cli_temp_owner` exercises the shipped executable with argv and config-only
temporary mode, retained policy files, pipe shell output, default-no/yes
cleanup, inherited persistence and failed socket startup. `cai_app_run_terminal`
also checks EOF/force cleanup, callback affinity and one SDK client in temporary
runs. The broader temp fixture checks shared authentication/messaging stores
and host STOP during the final question. Runtime-name replacement tests refuse
cleanup through a symlink to an unrelated tree.
The embedding guard rejects editor API reentry during the Softline relay and
checks that the host environment and cwd remain unchanged. The queue unit test
resolves a receipt lost during paused routing without offering the input twice.
