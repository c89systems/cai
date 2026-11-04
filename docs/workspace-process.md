# Workspace process handoff

This is executable scaffolding for the workspace frontend cutover. Native
embedding continues to use `cai_app_start_server`; libraries do not fork or
install process signal handlers. The terminal frontend and automatic startup
selection remain separate composition work.

Before starting UI/runtime threads, the executable may launch its own absolute
program path through a private `--_workspace-owner FD -- ...` entry point.
The child becomes a session leader, restores normal child signal dispositions
and masks, redirects standard input/output/error to `/dev/null`, and execs.
Only the explicit Unix socket is deliberately retained across that exec; the
child protects it with CLOEXEC before creating the owner or running tools.
The private entry point is recognized only as the first argument, with a
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

Verification covers fragmented and malformed bootstrap records, publication
only after COMPLETE, abrupt exit and startup failure, duplicate ownership,
pre-ready cancellation/channel loss, ready half-close, persistence overrides,
last detach, quiet standard descriptors, owned log paths, exact reaping and
native/musl execution. No Chrome or live provider is required.
