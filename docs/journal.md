# Session journals

`libcai` persists ordered changes. `libcai_app` stores them in one liblockdc
collection per conversation, alongside the runtime's replayable observation
events. Recovery reads the collection in order; it neither loads recurring
full-state snapshots nor executes recorded tools.

Console replay uses the same native tool renderer as live output. The recovery
pass indexes bounded command/path labels and completion metadata, not result
bodies, and evicts calls outside the selected replay tail. The rendering pass
decodes command output directly from ordered execution chunks into the display
sink, including JSON escapes split between records. There is no additional
collection scan, spool or per-tool thread. Completed command output is grouped
across `exec_command`/`write_stdin` calls by source session, terminal and command
identity; compact output keeps the normal preview limit for that whole command.
Known small structured results use the live application presenter. Other
non-command results flow through the Markdown feed: diagnostics are literal
blockquotes, JSON-looking bodies are code, and MCP text content is Markdown.
Chunked fallback bodies retain only rendering state, never a complete payload.
Command runners alone use the raw terminal preview.

Successful patches additionally record `tool.patch_locations` and `tool.patch`
execution fields from the confirmed live preview. These preserve context and
line numbers without rereading workspace files during replay. Submitted patch
arguments remain available separately for inspection. Invalid saved command
JSON is reported as a replay diagnostic, leaving the original JSONL available.

A durable `commit` record contains an incremental state edit and the watermark
of input/control events already reflected in that edit. Normal edits append only
the new canonical context bytes. History trim atomically removes events and
dependent commits, retaining an independently valid commit prefix.
Model-context compaction explicitly replaces context with the new
summary and retained provider items. Turn boundaries, hidden event ranges and
event ownership are also prefix/append edits. Small commit metadata preserves
settings, continuation handles, measured context usage and goal state.
Cumulative token counts and USD/EUR estimates are restored independently of
the most recent response's usage.

Revisions must increase consecutively. Restoration checks prefixes, lengths,
metadata and complete record boundaries. The payload's headers precede its
`history_append` string so the parser can decode it directly into retained
context. Sources and parsers use bounded chunks; there is no whole-journal
download, full-state reconstruction file or producer thread per entry.

The collection journal is authoritative. Session/directory documents are small
indexes for selection and listing. If an index publication fails after a durable
journal append, the append remains committed; dirty-index recovery streams commit
headers to repair selection. A stale index watermark cannot suppress newer
committed state. Pending events after the recovered watermark remain eligible
for the existing input/control replay path.

Recovery reads mixed commit/event entries directly through the collection's
ordered reader. The event pass discards commit state through a bounded parse
sink; it does not run an LQL range scan that decrypts and parses each selected
event before parsing it again. Context restoration precedes event recovery so
final trim visibility and the authoritative commit watermark are known.
During recovery the application retains one sequence/collection-position pair
per 128 events, with no text. Console-tail replay seeks native position bounds
using those transient hints and checks exact sequence/visibility bounds before
rendering. It never applies a stale captured upper bound to unbounded exports
or newer events. These hints add no persisted index, collection or snapshot.

Internal review/subagent scopes have no workspace-latest index. Their journals
and session metadata remain available by ID and in the parent's replay timeline;
workspace auto-resume is only for ordinary conversation scopes. Child commit,
publication and deletion therefore do not acquire a shared latest-index lease.
An old crash-held child index lease can expire without blocking a new reviewer.

The dependency-free `cai_agent_session_store` contract follows the same model.
Its `commit` callback atomically appends an edit, and its load callbacks return
the entire ordered journal. Its optional `trim` callback is required for persisted
runtime trimming; stores without it reject nonzero trim without changing history.
The built-in core backend appends JSONL for ordinary commits and events.
`cai_session.restore_journal_source` and Lua `session:restore_journal(reader)`
perform restoration without running tools. Explicit complete-state import/export
APIs remain available for callers requesting snapshots; they are separate from
runtime persistence and use spools.

## Atomic history trim

Trim removes the newest whole entries, including their tool results, reasoning,
steering and child observations. Inputs queued before the turn they belong to
are also removed, without removing the preceding turn's later output. Model
compaction remains one indivisible entry; trim never reconstructs opaque history.

The store streams record headers through discard sinks to select a valid
retained commit prefix and position ranges. It then asks the runtime for one
replacement edit, containing current settings, goals, cumulative usage and
history indexes. The edit appends only context bytes absent from that prefix.
Ordinary cuts therefore append an empty history suffix. An imported journal may
have only one context initialization after all its events: without a reusable
prefix, that cut writes the retained context fragment once. There are no recurring
full-state snapshots. Provider continuation handles are cleared so deleted
context cannot return through a provider-side response or conversation ID.
Commit metadata also retains the explicit-input-acknowledgement boundary, so
removing the original marker cannot discard queued inputs on a later restart.

Pouch uses the held collection lease, an expected-head bounded read, and one
native `edit` that deletes the selected ranges and appends the replacement.
Surviving positions and event identities remain unchanged; replacement commit
revisions continue consecutively from the retained prefix. The SDK allows at
most 64 disjoint edit ranges; an excessive cut fails before publication.
The collection keeps its configured inactivity retention. Entry removal changes
logical visibility; physical disk reclamation follows Pouch maintenance rather
than secure erasure. Diagnostic logs and external tool effects remain unchanged.

The standalone file store streams retained records to a private sibling file,
appends the replacement, syncs and atomically renames it, then syncs its parent.
A stable per-session writer lock serializes both ordinary appends and replacement;
readers already holding the prior inode can finish their pinned read. No full
journal or retained context is materialized in memory by either cut planner.

Failures before publication leave the original journal and live history intact.
A published edit with a failed acknowledgement, or a rename whose directory sync
fails, fences later writes until reopening. Edit-status receipts establish logical
publication but do not override a failed durable-sync acknowledgement.

## Pouch storage and ownership

Pouch compression defaults to streaming zlib before encryption. Disable it with
`--disable-compression`, `CAI_DISABLE_COMPRESSION=true`, or:

```ini
[storage]
disable_compression = true
```

Embedders can set the same schema field through application settings. Standalone
kanban receivers expose `disable_compression` in their C/Lua configuration too.
Equal Pouch roots still share one process client; conflicting compression modes
within that process are rejected. On a closed root, liblockdc converts a
changed explicit mode synchronously, preserving stored values, positions and
retention. Competing writers prevent conversion and trigger a warning with
adoption of the stored mode. Remote clients receive no local compression settings.

Journal appends atomically renew whole-collection inactivity retention through
the native append request. There is no second collection-metadata write per
event, and identical retries do not extend the collection deadline. Conversation
lookup metadata has no independent expiry: the collection owns the lifecycle.
Listing removes stale lookup records and associated prompt recall once the
backing collection is gone, while native use guards protect open conversations.
Finite generated records use liblockdc's direct pull append. Each source reads
already available event/state data; it does not wait for future model output.
Push and staged pull in 0.25.0 share rolling segments but add encoded staging;
CAI does not need that staging for these sources. No full-message buffer or
spool is introduced. The configured compression must match the root manifest.
Old CAI snapshot-backed journals remain unsupported. Existing append-only
collection roots use liblockdc's native control-head upgrade, described in
[the 0.25.0 upgrade notes](liblockdc-0.25.0-upgrade.md). The current
[0.27.0 SDK](liblockdc-0.27.0-upgrade.md) retains the atomic edits and recovery
heads introduced in 0.26.0 without rewriting existing 0.25.x roots; clients
predating those operations must be upgraded before edited collections are shared.

Writable restore acquires a native SDK collection lease before replaying model
context or choosing event IDs. All commits/events use that owned receiver until
the application conversation closes, including its child journal contexts.
Prompt recall has a separate leased collection. Public committed reads and
exports do not acquire a writer lease. New collections are created with their
initial lease atomically; retention, import and deletion use credentialed
receivers too.

CAI holds twenty-four-hour leases until the conversation context closes. A
store-context worker renews them every twenty-three hours, leaving an hour of
expiry margin, independently of journal writes and the UI. It does not acquire
the journal mutex or create a worker/client per collection or append. Lease
loss/renewal failure is sticky: stop the restored context, cancel active work,
and reopen before further writes. A native liveness guard, held until SDK release/close,
permits immediate conditional takeover of CAI's own abandoned ownership. It is
not a replacement for the SDK lease. Live or foreign owners are never silently
replaced. Keepalive does not renew inactivity retention, and release does not
commit or roll back already acknowledged appends.

Append receivers stay open for the application conversation and its children.
Steady-state event appends do not acquire, load, renew or release a state-object
lease. First-prompt publication and other logical metadata changes still use
short transactions, with public committed reads available throughout.

Each native metadata transaction also holds a private, close-on-exec file lock
under the same Pouch root. This serializes participating CAI writers across
processes; process death releases the lock immediately. After acquiring that
lock, a replacement can recover an abandoned lease through public SDK describe
and fenced rollback/release operations, then acquire normally. It does not wait
for the lease TTL or guess process liveness from a PID. Live guard holders and
foreign logical owners are not reclaimed. Native SDK transaction recovery
preserves committed data and discards uncommitted staging.
The guard files contain no session data or lease credentials and remain stable
to prevent lock-inode replacement races. Writers of application-owned metadata
must participate in this protocol; remote state retains server lease semantics.

Portable JSONL export excludes trimmed entries. It streams the visible
observation events and then one initialization commit for the retained
context. This deliberate export-only initialization is separate from live
incremental persistence. Import assigns a new conversation ID in the selected
workspace, validates the journal, and publishes it only after successful
restoration. Imported conversations are ordered by import time for selection;
their original event/commit timestamps remain available in the records.
Markdown continues to use the existing libcai conversation/execution exporters.

`cai_agent_runtime_export_journal_state(runtime, sink, error)` (Lua
`runtime:export_journal_state(writer)`) streams that initialization commit at a
stable owner-thread boundary. It neither commits live state nor advances its
persistence cursor. Restore into a fresh session; discard that session if
restoration fails, because earlier valid edits may already have been applied.

If a Pouch append loses its acknowledgement, the adapter asks liblockdc for
that append ID's status before acknowledging it. An unresolved status prevents
further journal writes through the shared client until reopening. The local
JSONL store similarly rejects further writes after a sync failure on a complete
record; reopening recovers complete records and ignores an incomplete tail.
