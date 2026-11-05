# Application maintenance utilities

`cai_app.run` routes maintenance through one utility dispatcher. The ordinary
argv parser classifies list/resume-list/delete-list, delete, import/export,
login, one-shot messaging, skill installation and Chrome registration. Help,
version, dumps and invalid argv use the same dispatcher's configuration path.
Chrome's native-host entry is recognized only in the first argument, retaining
its strict arity and stdout framing contract. An option value resembling a
utility flag cannot select a different mode.

Utilities open only the stores and resources needed for their operation. They
never run an agent loop or create a workspace server, Softline editor or libmdf
renderer. Offline export still uses the shared helper's read-only runtime to
restore saved context, with no-op write callbacks and execution-only replay;
it cannot consume pending model input. Removing that reconstruction in favor
of a storage-only export projection remains part of the separation work.
Session selection/export/deletion share the existing storage helpers
used by application commands; there is no second persistence implementation.
Import/export retain their existing source/sink behavior and paths. Utilities
do not acquire an inference attachment or start an automatic server. Lifecycle
status has no active runtime/session, and host STOP uses the ordinary control
channel, including OAuth callback waits and standalone enqueue completion.

Utility path aggregates have one heap owner. Work and callbacks remain on the
thread calling `run`; the default musl worker stack needs no enlargement.
Utilities and the agent owner use the same maintenance-mode classifier, also
used by executable dispatch and terminal-receiver validation. The old utility
implementation has been removed from the agent receiver.

This extraction does not claim independent concurrent utility execution.
The existing process-resource wrapper still serializes utility calls and
restores configured environment/locale state. The utility logger still uses
the dependency's process-environment constructor, and the OAuth browser opener
inherits that environment. Workspace owners and prompt/goal batch adapters
instead resolve captured `LOG_` values through public libpslog configuration
and output APIs; they do not enter this wrapper. The upstream lookup-constructor
request in `stash/cr-libpslog-captured-environment.md` would allow consolidating
that overlay in the dependency. Direct review/command batches, utility process
helpers and removal of the remaining combined receiver are separate cutover work.

`cai_cli_utility_boundary` seeds a conversation using a real local-provider
runtime, then runs maintenance through an embedding host that permits only
that offline restoration profile and aborts if a runtime loop, workspace-server,
editor or renderer is started. It checks
listing, independent import, export projections, deletion, skill replacement,
standalone messaging, failures, environment restoration and no extra inference.
The host runs on a default-stack worker. Other focused fixtures cover OAuth
STOP at accept/partial-request waits, messaging STOP before/after enqueue,
native Chrome frames, active-session deletion rejection and filesystem cleanup.
