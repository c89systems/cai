# Lua owner lifecycle

`require("cai.app").open(config)` also supplies the public C owner's
terminal-free lifecycle. The same native controller owns configuration,
runtime, stores, journal, queue and server. Lua does not add a worker, event
broker, process launcher or transcript accumulator. Start returns before
restoration; status describes readiness or eventual startup failure.

| Method | Contract |
| --- | --- |
| `start(argv, [context])` | Start a non-listening controller; stays alive until native STOP. |
| `start_server(argv, [context], [lifetime])` | Start that controller with its Unix workspace listener. |
| `status_fd()` | Borrow the level-triggered status descriptor. Poll for input; never read, write or close it directly. |
| `observe_status()` | Copy the newest status and atomically drain its notification. |
| `wait_status([timeout_ms])` | Explicit blocking poll, then observe. Returns `status, notified`; timeout returns the current snapshot and false. Default -1 waits without an extra timeout. |
| `get_status()` | Existing non-draining copied snapshot, safe during startup and work. |
| `control(name, [text])` | Existing trusted native host control; acceptance ID is distinct from application outcome. STOP works during startup. |
| `join()` | Explicitly wait for the started owner, returning its CLI exit code. Does not request STOP. Required before another start/run. |
| `close()` | For asynchronous owners, request STOP and join before releasing resources. May block until cancellation/workers finish. Idempotent after release. |

Arguments exclude the program name, just as with `run`. Context accepts
`directory` (absolute) and `environment` (a map of string names to string
values). Missing context members capture cwd/environment once at start.
An empty environment map means an empty environment. Names must be nonempty,
contain no `=` or NUL, and values must not contain NUL. Values are copied
literally, including empty strings, `=` and newlines. The process cwd,
environment, locale, signals and terminal remain owned by the Lua host.

Lifetime accepts `persist` (boolean, default false) and
`first_attachment_timeout_ms` (unsigned integer or exact decimal string; zero
uses the native 30-second grace). An omitted lifetime uses configuration.
Persistence is an owner decision, independent of attached clients.

Stream observations use the [Lua workspace client](lua-workspace-client.md)
after `start_server`. Attach after `runtime_ready`/server READY, keeping all Lua
callbacks on the thread calling the client pump. A non-listening `start` has
native controls and status; its full output is retained in the journal.
Inline `on_event` and `on_replay` callbacks belong to synchronous `run` and are
rejected at asynchronous start. They are neither silently ignored nor invoked
on a native owner thread. A host that needs streaming Lua observation uses
`start_server` and a client attachment.

An unjoined owner blocks another start/run. Final status retains error detail,
STOPPING/reason, owner identity and attachment state after the endpoint closes.
Server instance bytes and unsigned generation/controller/deadline fields use
the same binary/decimal representations as the typed client. Explicit close,
scope close and garbage collection all stop/join an asynchronous owner before
releasing its logger and native configuration. A synchronous callback still
cannot close its running receiver.
The module's `ATTACHMENT_STARTING/READY/STOPPING` and
`ATTACHMENT_NOT_STOPPED/LAST_DETACHED/STARTUP_ABANDONED/STOP_REQUESTED`
constants name those native phases and stop reasons.

Native STOP has one reserved allocation-free mailbox command. It can be
accepted during startup and at ordinary capacity (64), and repeated pending
STOP calls return the same ID. Shutdown cannot depend on finding another
ordinary mailbox slot. Ordinary commands accepted before shutdown retain their
normal application acknowledgements, including cancellation outcomes.

Start returns true for native startup acceptance, or `nil, message`. A later
startup failure is observed in status and finalized by `join`; it does not
become a synchronous start failure. Lifecycle methods use the public native
APIs and never implicitly fork, acquire a terminal or launch a frontend.
