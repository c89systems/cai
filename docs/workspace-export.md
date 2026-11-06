# Storage-only conversation export

Workspace-server export cutover, 2026-11-06. The JSONL projection is implemented;
the Markdown content decision below is awaiting confirmation.

## Current boundary

For Markdown, `cli/session_commands.c` opens an `offline-export` client and an
agent runtime when exporting a closed conversation. No inference is requested,
but runtime construction still restores the retained provider context, prepares application
instructions and reconstructs session state. The context uses the ordinary
history backing store. Streaming its subsequent serialization does not make
that reconstruction a storage-only export.

`cli/pouch_export.inc` opens one collection receiver and calls the private
`journal_projection` module. JSONL-only export constructs no client or runtime.
It indexes edits, streams visible observations, and generates one retained
initialization directly from native ranges. Active `/export` uses the same
projection as offline `--export`, without serializing its live runtime.

The existing core runtime handover/export APIs remain useful for callers that
explicitly own a live runtime. Removing the application's dependency on them
does not require removing those public APIs.

## Preserve the portable JSONL contract

The current portable document contains retained observation records followed by
one initialization commit. Preserve its schema, sequence identities, opaque
provider items, usage, goal state, input checkpoint and recovery semantics.
Do not create a new journal version, compatibility reader or export mode.
Trimming must continue to remove the discarded prompts, steering, tool data
and context from the exported bytes, including after import and compaction.
An exact copy of every stored commit is not equivalent: older commits can
contain superseded metadata and inputs that the portable projection excludes.

Use the existing runtime exporter as a test oracle at stable boundaries, not
as an implementation dependency. Compare normalized exported state and actual
import/resume behavior, including pending input and checkpoint boundaries.
Do not assume that copying a checkpoint or choosing the largest event sequence
is sufficient to reproduce the existing recovery semantics.

## Read a stable native view

Open one collection receiver for the export and capture its head identity and
upper position. Reuse that receiver for bounded native range reads. Apply the
head precondition across passes so a concurrent append, trim, expiry or
replacement produces an explicit failure instead of two inconsistent files.
Use the existing store/client registry; do not open a client per pass or entry.

The selection pass reads journal edits incrementally and retains references
and metadata only. A history reference identifies its native commit position
and the selected decoded byte interval of `history_append`. A later edit's
`base_bytes` truncates that reference sequence; its suffix adds a new reference.
Track revision, byte-length and applied-watermark invariants while discarding
the actual provider payload. Fold the three metadata-array prefixes according
to `blocks_keep`, `hidden_keep` and `owned_keep`. Retain event-visibility ranges
and checkpoint provenance without retaining conversation text.

Generate the seed by reading the selected source fields and history intervals
directly into the JSON writer. Forward decoded text through the writer's
chunked string interface; do not concatenate it first. Arrays and checkpoint
fields must also use incremental readers/writers. Index memory may grow with
entry/range metadata; payload buffers remain bounded independently of text
size. The complete seed, arbitrary event payload, context and document must
never be collected in RAM, a JSON DOM or a context spool. Bounded individual
input/control records retain the existing controller's limits and codecs;
this is not permission to collect unrestricted tool or model output.

Order native reads so each pass is linear, rather than reopening/scanning the
whole journal for each context item or array member. No storage read may be
reentered from another read callback on the same receiver. Selection can
precede output; the selected payload still flows from its producer to its
consumer incrementally. Describe the selection pass separately from payload
delivery and measure both.

## Markdown decision

The current application Markdown is a runtime handover. In particular, its
“Active developer instructions” section can include today's workspace files
rather than what was recorded in the conversation. A storage-only projection
cannot claim those instructions came from the saved session.

Recommended: both `/export` and `--export` produce a document of recorded
conversation data, saved metadata and execution history. Preserve original
ordering, attribution, timestamps, reasoning and tool failures, with explicit
omission markers for unavailable non-text payloads. Reuse the existing
incremental transcript/execution decoders. Do not load current workspace
instructions or initialize tools, authentication or a model to generate it.

The alternative is to preserve the current handover semantics, including
current instructions. That requires an explicit instructions/configuration
projection separate from the stored transcript. The pending product decision
selects which document the application promises; implementation must not
silently substitute one for the other.

## Publication and lifecycle

Retain the existing basename/suffix rules, default destination, private output
permissions and sibling-file publication. These files are the requested output
artifacts, not a hidden staging substitute for a streaming context reader.
Generate every selected projection successfully before publishing destinations.
Failures preserve the previous outputs and remove incomplete export artifacts.

Keep long export work separate from rendering and normal owner dispatch.
Frontend typing must remain responsive. Cancellation must interrupt bounded
read/format work without reentering command execution or mutating the selected
conversation. Completion/failure retains the admitted command identity, and
resources close once the export context exits. Native utility callbacks retain
their documented thread affinity.

## Required proof

- `cai_cli_utility_boundary` rejects client and runtime construction during
  JSONL-only export. Its Markdown exception remains until that cutover.
- Compare retained state and JSONL import/resume against the existing core
  exporter across ordinary turns, steering, tool results, reasoning, goals,
  compaction, trim, queued input and input-checkpoint recovery boundaries.
- Check literal absence of trimmed data in both projections, including data
  inside older context edits and embedded execution records.
- Generate large text from a bounded producer. Prove that output begins before
  the selected payload producer finishes, and that memory/staging does not grow
  with text size. Exercise awkward UTF-8/escape splits and one-byte chunks.
- Cover invalid edit prefixes/revisions/lengths, truncated records, changed
  collection heads, missing/expired data, cancellation and source/sink failure.
  Verify no inference, input consumption, journal writes or unexpected files.
- Exercise the actual public application/CLI paths and static x86_64 musl
  executable on default worker stacks. Run normal Debug and affected focused
  tests, formatting and whitespace checks; rebuild musl and commit. The full
  live/release matrix remains outside this goal's verification scope.

`cai_journal_projection` covers native edit prefixes, folded metadata, original
usage/goal values, checkpoint carry-forward and actual queue restoration,
one-byte UTF-8/escape splits, revision/range/identity/framing failures,
incomplete reads, changed heads, cancellation and sink failures. A generated
fixture interleaves 8 MiB context, event and metadata strings while rejecting
individual allocations above 64 KiB and aggregate tracked live allocations above
1 MiB. It asserts payload delivery before the
producer finishes and a constant number of native read passes. Tests run on a
default worker stack; metadata indexes remain proportional to entry counts.
CLI journal/import, trim and destination tests cover both provider protocols,
literal absence of removed data, real resumed inference inputs and publication.
The destination test also runs against the standalone executable, including its
startup dispatcher. Validation failures retain the parser's specific diagnostic
instead of reporting only a generic invalid status.
