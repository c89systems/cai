# Native session export

This reference describes the implemented JSONL and Markdown projections.
For user commands, destinations and format selection, see [session export](sessions.md#export-and-import).
Both formats read native journal ranges without constructing a client, agent or runtime, restoring model context, or staging a complete context file.

## Selected view

`cli/session_commands.c` opens one read-only collection receiver and a shared `journal_projection` view for the selected formats.
The receiver stays bound to its collection incarnation and captures an upper position and live-entry count.
Entry bodies and permanent positions are immutable; later appends cannot enter that prefix.
Every full selection/observation pass verifies the original count, while selected metadata and history references additionally verify identity, revision and byte length.
Deleting a required original entry or replacing the collection fails the unpublished candidate.
Harmless new appends do not invalidate it.

The selection pass retains edits, byte-range references and bounded metadata, not provider or event payloads.
It applies `base_bytes`, retained array prefixes, visibility ranges and checkpoint provenance in journal order.
Payload then flows through native range readers and incremental JSON/transcript decoders.
Index memory can grow with entry/range metadata; payload buffers remain bounded independently of session size.
No pass reenters a read callback on its own receiver or opens a separate Pouch client per entry.

## JSONL

`cli/pouch_export.inc` and `cli/journal_projection.c` implement the portable journal projection used by import/export and native recovery.
The output begins with one synthetic `cai_cli_session_init` containing the retained state at its applied watermark.
Only later visible observations follow it; trimmed records stay excluded.
The seed preserves required metadata, selected history, unfinished-tool ownership and checkpoint/recovery provenance.
It preserves exactly-once queued-input recovery and original observation timestamps.

The JSON writer consumes selected history ranges and array fields incrementally.
It never collects the complete seed or arbitrary event bodies in a JSON DOM, C string or spool.
The exporter does not publish a new seed into the live session or alter its import behavior.

## Markdown

`cli/markdown_export.c` feeds the shared `cai_handover_write` formatter in `cai/handover.h`.
It preserves the runtime handover sections, attribution, reasoning, omission markers and structured execution history.
The formatter performs linear metadata and rendering passes over retained history; it does not reread the entire journal for each item.
Large text, goals and tool bodies move through source/sink callbacks.
Model/settings control strings are bounded to 8,191 bytes; oversized or malformed controls fail the candidate.
That control limit is not a limit on session text or goal objectives.

An active `/export` captures its live header, instructions and goal at admission through `cai_agent_runtime_prepare_handover`.
Bounded controls are copied; large instruction/goal text retains immutable references to the existing owned allocations.
Later model/goal changes replace those allocations without changing the capture.
History and observations come from the admitted native prefix.

Offline export prepares the effective preset/instructions without constructing a runtime or opening tools/integrations.
It uses the same instruction preparation and access policy as ordinary agent construction.
Current effective instructions remain distinguishable from recorded session content; Markdown is a handover document, not an assertion that every header field was recorded at the original event time.

## Publication and cancellation

The existing session-command layer owns output paths, private permissions, paired-file publication and replacement.
Source, sink, cancellation and consistency failures preserve existing destination files and clean partial candidates.
Exports execute no model requests or tool side effects.
JSONL imports into a new session rather than overwriting the selected original.

Interactive export uses one retained worker per workspace so the owner can continue accepting status queries, input and cancellation.
The command retains its original accepted-work identity until its real result is journaled.
Further exports queue behind it; no worker is created per chunk or item.
A persistent owner continues admitted export after its frontend detaches.
Shutdown requests cancellation and joins the worker before closing storage.
Utility export remains synchronous on its caller thread.

## Verification

`cai_journal_projection` covers retained state, trim visibility, immutable native prefixes, source/sink errors and generated large values under allocation guards on a default worker stack.
`cai_handover`, `cai_instruction_writer` and capture tests cover the shared formatter, runtime-free instruction preparation, large retained header values and ownership after runtime closure.
CLI export-path, utility-boundary and retained-export-worker tests cover destination preservation, cancellation, batch settlement, shutdown and persistent detach/reconnect.
See [development](development.md) for selecting the current checks.
