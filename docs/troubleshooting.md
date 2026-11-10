# Troubleshooting

## Find the evidence

`/status` reports session and provider response IDs, effective settings, and the full session log path when file logging is configured.
`/log` prints just that path; `/log -p` and `/log less` open it in an external `less` when available, otherwise libmdf's text pager.
`prefer_native_pager=true` selects libmdf directly.
`/help -p` always uses libmdf's Markdown pager.
Model work continues in the owner while the pager is open, and newly arrived output appears after it closes.
Export with `/export incident.jsonl` to inspect tool arguments, results, failures and ordered harness events without the runtime log.
These records are separate from model context.
See [session export](sessions.md#export-and-import).

## Authentication and workspace approval

Use `cai --login` when ChatGPT credentials are unavailable.
The CLI can import Codex auth, but refreshes its own stored copy; explicit `--auth-json` selects another import source.
API-key providers require their configured key variable.
See [providers](providers.md).
A new workspace requires explicit approval with `-y`/`--yes`/`--force`, or invocation-only `-k`/`--insecure`.
Approval and YOLO are different policies; see [configuration](configuration.md).

## Storage and ownership

Back up the encrypted Pouch root and its original key together.
A lost key cannot be regenerated to read the existing root.
Do not delete roots as an index-cache repair.
Only one writer may own a session.
A second writer is refused; committed reads and exports remain available.
CAI can conditionally reclaim its own crashed owner, but cannot steal another live writer merely because it appears idle.
See [leases and recovery](journal.md#pouch-storage-and-ownership).
Missing or invalid Pouch query indexes are derived-cache failures, not proof that the underlying journal is corrupt.
Startup prepares them through the SDK's wait flush; listing can retry recovery.
Preserve the original root/key and log details before investigating storage failures.
All processes sharing a root must use a compatible liblockdc version.
Native chunked append records and atomic edit heads cannot be read by every older SDK; see [the journal compatibility contract](journal.md#pouch-storage-and-ownership).

## Input waits and steering

A queued `Q` entry waits for an ordinary turn boundary; an `S` entry waits for a response/tool boundary.
Steering does not splice into streamed text.
Some commands are read-only, some change settings at a safe request boundary, and others need an idle runtime; [the command policy table](interactive-command-audit.md) lists them.
Escape stops active work and pauses an active goal.
Use `/goal resume` to restart pursuit; achieved goals cannot resume.
Stopped backlog entries remain stopped unless explicitly promoted or submitted through the editor's normal policy.

## Completion triggers

Softline 0.17 has no pre-activation completion veto.
With completion providers enabled, an unmatched `@` or `$` shell word can lose its trigger before CAI receives it; returning no candidates cannot prevent activation.
This remains an upstream limitation, not a repaired CAI feature.
Shell handoff avoids interpreting an external shell's input as prompt completion.
The active request for an upstream API is tracked in the repository's `stash/softline-completion-context-and-literal-triggers.md`.

## Terminal and browser issues

Record a raw terminal reproduction when output loses margins, resize leaves stale cells, or editing stalls.
`/theme plain` and `/status` identify the rendering context.
An intermittent live-margin report remains unconfirmed in the issue tracker; do not assume every visual issue is a Softline or libmdf defect.
Chrome integration depends on the installed extension's internal protocol.
Use `cai --chrome-setup`, check the extension connection, and preserve the method/error and session log when claiming a tab or reading a screenshot fails.
A kept tab remains open but must be claimed again after its browser lease is released.
See [Chrome compatibility and setup](chrome.md).
For CPU or latency investigations, use [the benchmark contract](benchmarks.md) and [the measured component profile](performance-cbor-presentation.md); aggregate rendering throughput alone does not identify a component bottleneck.
