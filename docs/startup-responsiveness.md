# Historical startup and streaming measurements

This report records the pre-server implementation and dependency versions below. It is comparison evidence, not the current architecture or usage guide; see [workspace servers](workspace-server.md) and [current component measurements](performance-cbor-presentation.md).

Measured on 2026-10-29, Linux x86_64, eight logical CPUs. The baseline was
the existing static musl CAI binary saved before implementation (source
`9a58991`). Both versions used liblockdc 0.24.0, Softline 0.15.0, libmdf 0.18.0
and compressed, encrypted Pouch storage. Builds and tests ran serially;
benchmarks ran after those operations finished.

## Changes

The runtime retains ownership of execution, ordered durable events and history.
Softline/libmdf consume observations in bounded batches, and state/metrics
reads remain available during storage callbacks. This is an in-process
foundation for a future client/server host; a transport and server protocol
remain future work.

- `cai_agent_runtime_pump_bounded` limits synchronous callbacks per dispatch;
  Lua accepts the optional limit as `runtime:pump(timeout_ms, max_events)`.
  libcai_app dispatches four observations per interactive wakeup.
- Live text/reasoning observations have at most 1,024 payload bytes and preserve
  complete UTF-8 scalar boundaries. The original durable deltas are unchanged.
- Event appends stay serialized and synchronous, but release the state mutex
  around the host store callback. A failed append never publishes successful
  text. Cancellation and state reads remain responsive while an append waits.
- Recovery directly parses ordered collection entries, discarding commit state
  through a bounded sink during the event pass. It avoids the redundant LQL
  selector/decode pass. Sparse sequence/position hints, one per 128 events,
  bound the later console-tail read through the native collection API.
- Context restoration and event recovery remain two logical ordered passes:
  final trim visibility and the authoritative commit watermark must be known
  before recovered observations are delivered. The hints contain no text and
  add no persisted cache, snapshot, extra client or writer thread.
- A worker can finish timing before its terminal observations are dispatched.
  The UI now says “Finishing turn” during that interval, preserving agreement
  with shell/command readiness.

## Startup comparison

Each invocation used a separate copy of the same user store and resumed the
substantial `~/lab/x` session. Configuration selected an unreachable
loopback provider, disabled goal automation and requested no model work. These
are whole-process elapsed times, including replay and clean EOF shutdown.

| Console replay | Binary | Elapsed | User CPU | System CPU | CPU total | Average CPU | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Disabled | Baseline | 4.72 s | 1.65 s | 2.76 s | 4.41 s | 93% | 18,180 KiB |
| Disabled | Final | 3.42 s | 1.23 s | 2.03 s | 3.26 s | 95% | 19,072 KiB |
| Last 10 turns | Baseline | 6.18 s | 2.24 s | 3.80 s | 6.04 s | 97% | 18,904 KiB |
| Last 10 turns | Final | 3.94 s | 1.49 s | 2.29 s | 3.78 s | 96% | 19,456 KiB |

Ten-turn startup used about 36% less elapsed time and 37% less CPU time.
Initial measurements taken before editing were 5.23 s without console replay
and 7.14 s with ten turns. These warm-host samples vary with filesystem and
host activity; they are evidence, not timing assertions or throughput limits.

## Concurrent input comparison

A disposable driver ran the production binary in a terminal against a local
HTTP fixture. It streamed 128 Markdown paragraphs through the real durable
store while editing a 1,024-character draft, typing 64 characters at nominal
5 ms intervals, and appending a marker. The marker was observed in the current
prompt snapshot. The driver did not alter durability or use a volatile store.

| Binary / width | Entire typing batch | After last key | Stream finished when input became visible | Process elapsed | User CPU | System CPU | Average CPU | Peak RSS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Baseline / 120 | 7,705 ms | 7,075 ms | Yes | 9.02 s | 0.68 s | 0.86 s | 17% | 14,812 KiB |
| Final / 120 | 622 ms | 6 ms | No | 8.93 s | 0.78 s | 0.86 s | 18% | 15,352 KiB |
| Final / 64 | 603 ms | 6 ms | No | 9.38 s | 0.73 s | 0.83 s | 16% | 15,172 KiB |

The typing batch includes deliberate inter-key waits and driver overhead.
The last-key observation is sampled by the terminal driver; it is not a
guaranteed worst-case latency for each key. CPU percentages are GNU time's
process accounting relative to one logical CPU. Responsiveness improved while
CPU seconds stayed comparable; the change does not claim lower streaming CPU.
The initial baseline also reproduced an 8,717 ms typing batch at 15% CPU.

## Verification

- 530 selected native unit/offline checks and the core Lua suite passed.
- All 218 CLI/application checks passed, including C/Lua embedding, steering,
  goals, settings, review handoff, replay, export, shells, messaging and kanban.
  Matching passes were retained while failing cases were repaired individually.
- New condition-gated unit tests cover blocked/failed appends, cancellation,
  concurrent submissions, ordered sequences, callback bounds, readable pending
  wakeups and exact UTF-8 content across a 4,097-byte provider delta.
- Both protocol variants verify draft editing while a real store callback is
  blocked, failed/cancelled output, and finalization without an early idle line.
  Parser tests cover oversized commit strings and stale/cross-session bounds.
- Stale fixtures now request a PTY when testing interactive stdin, use the
  actual returned process handle, and explicitly request replay-all when checking
  old shell entries. A thirteen-invocation Lua steering check completed in
  36.37 s; its aggregate deadline is 60 s, retaining every assertion.
- The final production musl binary passed isolated review/handoff/reopen tests
  with its default worker stack settings, without an enlarged stack.
- A copied real history passed replay, status, draft editing, inventory,
  theme/history/retention queries and JSONL export. The actual `cai` symlink
  resumed the real `~/lab/x` history, displayed status and accepted draft edits;
  it was ready in 4,041 ms. Workspace Git status was identical before and after.
  No model work was requested. These probes do not measure live-provider latency.

The artifact from that initial dispatch change was
`build/x86_64-linux-musl-release/cai`, statically linked
with no ELF interpreter. SHA-256:
`700d17af7f9e4e35ea89fa452e57e720c8e3866e0057269834312d8ea8da2d9d`.
The saved baseline SHA-256 was
`2949a9b7032054914f926d8609be8cdd981948ba8f6e48c578f708b0f60c9253`.
No release matrix, cross-target gate or live-provider test was run for this task.

## Follow-up: metadata ownership and streaming CPU

The following measurements use the saved static musl binary from `fb883e9`
as baseline and retain zlib, encryption and production durable-sync settings.
A disposable Bash driver streamed 128 Markdown paragraphs through a loopback
provider and real Pouch storage. No benchmark was added to the regular suite.

Software CPU sampling of the corresponding native Debug application attributed
0.528 of 0.873 sampled CPU seconds to event append. Nested within that path,
lease release used 0.261 seconds and separate metadata retention used 0.129;
the native collection append itself used only 0.052. These inclusive times
overlap. A separate musl syscall-counting run observed 42,810 opens, 41,669
directory-creation attempts and 46,204 directory reads for the interactive
probe. Traced wall times include instrumentation overhead and are not normal
latency measurements.

CAI was acquiring, loading, renewing and releasing a lookup object for every
delta. That transaction was unnecessary: a native collection append already
persists the entry and renews whole-collection retention atomically.
Session-scoped collection receivers now remain open, lookup metadata has
no independent expiry, and leases cover logical metadata changes only.
The follow-up native sample used 0.093 CPU seconds, with 0.041 in event append
and 0.010 in lease release across the entire invocation. Samples are diagnostic;
production measurements below are the CPU comparison.

| Production musl probe | Before elapsed | After elapsed | Before CPU total | After CPU total |
| --- | --- | --- | --- | --- |
| Markdown and concurrent draft typing | 8.46 s | 1.95 s | 1.60 s | 0.59 s |
| Batch, without the terminal UI | 7.96 s | 1.62 s | 1.05 s | 0.20 s |

Streaming plus typing used about 63% less CPU time; batch used about 81% less.
These are single comparable samples, not performance thresholds.
CPU percentage alone is misleading for the burst fixture because the optimized
invocation finishes much sooner.

The real `~/lab/x` session was resumed through the installed musl symlink.
Status, tool inventory, theme inventory and an ordinary shell command worked.
Live ChatGPT generation completed without tools or workspace edits. A draft
typed during output became visible in 8 ms; the request interval used 1.20 CPU
seconds over 34.04 wall seconds (about 3.5% of one CPU). After finalizing metadata
release cleanup, the rebuilt binary repeated live generation: 5 ms draft
visibility, 0.93 CPU seconds over 18.59 wall seconds (about 5%).
Three verification prompts/replies were added to that session.

Startup/replay remains a separate cost: the final live-generation probe took
4.77 seconds to become ready. A subsequent reopen replayed its completed answer,
accepted a fresh draft and quit successfully; after settling, it consumed no
measurable CPU ticks over five idle seconds (10 ms accounting resolution).
That reopen took 4.86 seconds. Context is restored once, followed by event
recovery and selected console replay as described above. This change does not claim a
single combined recovery pass or complete UI/backend separation.

Twenty-five affected native Debug CLI/application checks passed, including
C/Lua embedding, messaging, kanban, history, export and responsiveness.
Additional condition-gated crash cases exercise live ownership, SIGKILL during
staging, committed-state preservation, death immediately after SDK release,
abandoned failed-release recovery, scope metadata and foreign-owner refusal.
The storage regression also passed on static musl, and the production musl
binary passed isolated review/reopen checks using default worker stacks.

The rebuilt static musl executable at the same path has SHA-256
`7364588b3ac34359cbdf94fe091c6e9ab90a03a16c53de742c20193340e7f9e1`.
The workspace Git status was unchanged by the live tests. No release matrix or
cross-target gate ran for this follow-up.
