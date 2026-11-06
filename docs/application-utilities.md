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
of a [storage-only export projection](workspace-export.md) remains part of the
separation work. Its Markdown content contract needs the product decision
recorded in that design before implementation.
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

Independent utility invocations use captured environment/directory views, like
workspace owners and batch adapters. No global utility lock or process-state
save/restore wrapper remains. `[env]` applies to that invocation's logger,
paths and browser child; it is never installed into the embedding process.
Logging uses the same public libpslog configuration/output adapter as owners,
including workspace-relative file sinks. Chrome native messaging masks only
that logger's `LOG_OUTPUT`, preserving stdout framing without `unsetenv`.
Default Chrome registration paths use the captured `CHROME_CONFIG_HOME`, XDG
or HOME value. Explicit manifest paths keep their normal precedence.

OAuth's public browser config accepts an immutable environment and absolute
child cwd. PATH resolution and mutable exec arguments are prepared before
fork; the child performs descriptor/signal setup and exec only. `LD_PRELOAD`
is removed from the child copy. Host environment, cwd and signal dispositions
are untouched. Session/history row sizing uses a scoped UTF-8 thread locale,
restoring the caller's locale before invoking its output sink. No process
locale is installed. The optional upstream lookup-constructor request in
`stash/cr-libpslog-captured-environment.md` would consolidate the logger overlay
in libpslog; it no longer blocks utility isolation.

`cai_cli_utility_boundary` seeds a conversation using a real local-provider
runtime, then runs maintenance through an embedding host that permits only
that offline restoration profile and aborts if a runtime loop, workspace-server,
editor or renderer is started. It checks
listing, independent import, export projections, deletion, skill replacement,
standalone messaging, Chrome registration, failures, Unicode rows and no extra
inference. Its guard rejects transient environment, locale and signal mutations,
not just differences after return. Two concurrent utility calls must reach
their stores with different configured environment values while the host
retains its own value. The host runs on default-stack workers.
`cai_auth_browser_environment` independently exercises simultaneous browser
launches, relative/empty/absolute/oversized PATH components, executable and cwd
failures, retained caller views, child signal reset and unchanged host state.
Other focused fixtures cover OAuth
STOP at accept/partial-request waits, messaging STOP before/after enqueue,
native Chrome frames, active-session deletion rejection and filesystem cleanup.
