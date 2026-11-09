# Workspace process handoff

Quiet automatic startup defers invocation actions until the frontend has
attached and admitted them through the ordinary queue. READY proves a restored
owner and selected conversation, not execution of its original `-i` arguments.
This applies to terminal and batch attachments. Explicit `-x` startup
retains its owner-side automation and persistence policy.

This is executable scaffolding for the workspace frontend cutover. Native
embedding continues to use `cai_app_start_server`; libraries never fork a
workspace owner. The executable's
[terminal attachment](terminal-attachment.md) now uses this handoff for automatic
startup after workspace discovery.

Workspace discovery and startup create no frontend worker threads. After nonblocking
discovery, it may launch its own absolute program path on that single frontend
thread by executing `cai -x --_workspace-owner FD -- ...`. The editor
can already be open; this launcher must remain outside threaded library hosts.
The child becomes a session leader, restores normal child signal dispositions
and masks, redirects standard input/output/error to `/dev/null`, and execs.
Only the explicit Unix socket is deliberately retained across that exec; the
child protects it with CLOEXEC before creating the owner or running tools.
The private bootstrap entry point is recognized only immediately after `-x`, with a
strict decimal descriptor of at least 3 and an explicit separator. A connected
same-user Unix stream is required. It is not a public startup flag.

The child uses the common native owner lifecycle and signal loop. It creates
owned session logs without a frontend notice queue and publishes readiness
only when both runtime restoration and the workspace listener are ready.
Persistence uses the shared resolved configuration: normal false default,
explicit headless true default, and explicit overrides. Starting an owner
does not itself create an attachment or a producer. Frontend prompts must
eventually be submitted once through the bound producer, not forwarded as
both backend startup instructions and frontend input.

Automatic launches pass the resolved `--no-persist`/`--persist` policy to the
headless process. The normal default exits after its last client disconnects;
explicit `cai -x` defaults to persistent. Both use the same native server lifecycle.

`cai -X` (`--terminate-headless`) stops the current directory's server through
the Unix socket, including persistent servers and servers with attached clients.
It takes controller authority explicitly, requests shutdown and waits for the
connection and original endpoint to close. It never launches a server or opens conversation storage.
The endpoint is `ws-<hash-of-canonical-workspace>.sock` in the configured runtime
directory, so resolving the current directory (including symlink aliases) is
sufficient; no conversation ID or process scan is involved.

`cai -X --all` visits the configured runtime directory's workspace registry and
stops its owners after verifying their identities. Vanished hints are ignored;
instance checks prevent a stale record from targeting a replacement owner.
`--all` also modifies `--list`: `cai --list --all` is equivalent to `cai -L`.
`cai -R --all` lists conversations across the history store, and `cai -D --all`
lists closed conversations eligible for deletion across that store. These global
lists show IDs and directories without workspace-local row numbers. Neither
command changes or deletes conversations.

Conversation IDs are unique within the history store. `cai -D ID` deletes that
conversation from any working directory, with or without `--all`; open
conversations remain protected. Numeric deletion choices stay workspace-local
and require omitting `--all`. Resuming still uses the conversation's workspace:
use `cai -C DIR -R ID` for an ID from a global listing.
Other uses of `--all` are rejected.

An explicit local shell/pager action is a separate terminal operation, available
to native embedding too. Softline relays its PTY while native owner connections
continue progressing quietly. The adapter prepares argv and captured environment
before fork; the child uses only async-signal-safe operations before exec. One
default-stack waiter observes that exact child with `WNOWAIT`, keeping its PID
owned until terminal restoration, join and reap. It neither installs a SIGCHLD
handler nor changes the host cwd/environment. The host must leave these children
waitable and must not reap them.
When `less` is unavailable, libmdf's on-demand log pager uses a bounded rendered-content cache and temporarily owns and restores terminal/signal state instead.

The bootstrap channel uses the existing incremental CBOR/framing/protocol
implementation, not a second serializer or a JSON value. STATUS records have
zero metadata except sequence 1 for READY and 2 for EXIT, and a definite
nine-element payload:

`[ready, exit_code, error_code, instance, session, log_path, message, clipped, persist]`

Ready/clipped/persist are booleans; codes are nonnegative integers; instance is
exactly 16 bytes. Session/log/message are byte strings, respectively bounded
to 128/4095/1024 bytes and excluding NUL. Only the diagnostic message may be
clipped, explicitly. These fixed-size metadata fields are copied into one
bounded parser result after a validated record boundary. Payload tokens are
encoded directly to the bounded nonblocking wire adapter; there is no complete
encoded-message buffer or sizing serialization. This is metadata exchange,
not a transport for transcript bodies. Session/path values need literal
escaping if displayed.

READY appears at most once, followed by at most one EXIT; EXIT may also report
startup failure before READY. The final error follows the process failure or
the failed application outcome. A retained recovered diagnostic does not turn
a successful STOP into failure. Parent death before READY requests orderly STOP.
After READY, bootstrap disconnection does not stop an owner: attachment count,
first-attachment grace, explicit shutdown and persistence govern it normally.
The completed read half is removed from poll, while its descriptor remains
owned until server shutdown; a half-closed launcher can still observe EXIT and
does not mistake bootstrap read completion for process exit.
EXIT is followed by process exit. An unresponsive bootstrap reader cannot
block shutdown indefinitely.

The parent owns a single child handle, socket interest and startup deadline
(zero selects 60 seconds). It does not poll idle live children or search PID
tables. Startup cancellation/expiry signals only its unreaped child, then
escalates after a bounded grace if required. After channel EOF, waitpid for
that exact child may need a short exit-reconciliation timer: EOF can precede
the kernel's waitable exit. Terminal handles have no descriptor/deadline.
Keep the watch through the child lifetime; releasing a live watch is reserved
for immediate executable exit, when surviving persistent children are adopted
normally. Close must not silently kill a ready server or discard an unreaped
child in a running frontend. Workspace locks and conditional discovery, not
bootstrap PIDs, decide whether an owner already exists.

Initial discovery and the child watch use the same frontend prepare/pump
adapter. Lookup never acquires controller authority. A losing pre-READY child
with a claim-limit error is reaped before one fresh verified lookup; only a
successful identity response selects the winner and changes the frontend to
the existing-owner startup-input path. Other launch failures are preserved,
and a failed reconciliation does not start another child. Resolved lookups
without an owned child have no descriptor or timer, so they add no idle polls.

Verification covers fragmented and malformed bootstrap records, publication
only after COMPLETE, abrupt exit and startup failure, duplicate ownership,
pre-ready cancellation/channel loss, ready half-close, persistence overrides,
last detach, quiet standard descriptors, owned log paths, exact reaping and
native/musl execution. No Chrome or live provider is required.
