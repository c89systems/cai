# Runtime-free conversation export

Workspace-server export cutover, 2026-11-06. Both offline export formats now
read native journal ranges. Markdown preserves the existing handover baseline;
the recorded-only document below remains an optional product change.

## Current boundary

`cli/session_commands.c` opens one read-only collection receiver and one
`journal_projection` view shared by all selected outputs. Neither offline
format constructs a client, agent or runtime, restores model context, or
creates a context backing file. JSONL indexes edits, streams visible
observations and generates one retained initialization from native ranges.
`cli/markdown_export.c` reads bounded settings, streams the goal and history
into the core handover formatter and appends bounded execution records.
Model/settings strings are explicitly limited to 8,191 bytes; oversized or
malformed control values fail the candidate. Goal and provider text have no
such control-value limit.

Active `/export` already uses the native view for JSONL. Its Markdown header,
captured instructions and resident history still come from its existing live
runtime, preserving live configuration semantics. That remaining owner-only
adapter must be addressed when moving long exports out of owner dispatch.

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

An optional proposal is for both `/export` and `--export` to produce a document of recorded
conversation data, saved metadata and execution history. Preserve original
ordering, attribution, timestamps, reasoning and tool failures, with explicit
omission markers for unavailable non-text payloads. Reuse the existing
incremental transcript/execution decoders. Do not load current workspace
instructions or initialize tools, authentication or a model to generate it.

Until that change is explicitly selected, preserve the current handover
semantics, including the instructions/configuration available to that export.
Preserving this existing contract needs no new approval. The outstanding work
is an implementation gap, not a reason to wait for a product decision.

## Markdown implementation baseline

Separate these responsibilities without adding another runtime or keeping a
parallel Markdown formatter:

1. Extract the existing handover renderer from `cai_agent_runtime.c` into a
   reusable core formatter. Live runtime export supplies its existing metadata,
   instruction view and history producer; the application supplies native
   journal projections. Keep the handover sections, attribution, omission
   markers and execution history rather than silently replacing them with a
   different document.
2. Factor instruction preparation from `cai_smith.c` so the offline application
   can prepare the same effective preset/configuration without constructing a
   client, agent, tool runtime or model context. Reuse the same instruction
   compiler and access policy; do not duplicate templates, broaden workspace
   instruction approval, execute tools or open integrations. Configured global
   instructions remain distinguishable from recorded conversation content.
3. Expose the existing native journal index through a private retained view:
   one collection receiver, one captured head and metadata/byte references.
   Supply retained history to the formatter through bounded source/sink
   callbacks. Both application formats use that same view and publication
   lifecycle; no context spool or full JSON value is introduced.
4. Provider object member order is arbitrary. The current renderer solves this
   with a metadata scan and render scan per seekable history item. Replace
   that seek assumption with a linear metadata pass over retained items, keeping
   only bounded item identities, followed by a linear streaming render pass.
   Do not reread the whole journal for each item. Arbitrarily large text, goal
   objectives and tool bodies still flow in chunks; only bounded configuration
   and control records may use their existing explicit limits.

Compare the resulting handover with the existing core exporter in tests,
including current instructions, fields after large text, opaque compaction,
trimmed observations, interrupted tools and non-text omission markers. Remove
the application's offline runtime helper and its utility-boundary exception
only after both formats use the new projection. The recorded-only proposal
must not be selected merely because it is easier to implement.

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

This owner-dispatch cutover is not implemented: `/export` still calls the
shared exporter synchronously, including the JSONL selection pass. A retained
job/reader context must preserve its accepted command identity, keep the SDK
client open through its actual lifetime and marshal completion back to the
owner. It must not start a thread per request or call the runtime's owner-only
API from an export worker. A deterministic gated-read fixture must prove
concurrent status and cancellation progress. Durable queue admission itself
appends journal records, so prove snapshot behavior under those appends and
destructive edits before changing head preconditions. Do not substitute a
complete context buffer or temporary journal copy for a missing read contract.

## Required proof

- `cai_cli_utility_boundary` rejects runtime construction for every utility and
  client construction for every export, including Markdown-only and paired output.
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

The runtime-free instruction boundary in item 2 is implemented as
`cai_agent_preset_write_instructions` in `cai/smith.h`. It shares preparation
and the template writer with ordinary agent construction; custom reviewer
templates also use that writer. The public call writes at most 4 KiB per sink
callback and never creates a client, agent, tools or rendered instruction
buffer. Global/workspace files and skill metadata retain their existing
bounded preparation limits. The ordinary agent constructor still allocates
its required final instruction string, using a counting pass followed by the
same writer. The prepared `cai_agent_instructions` handle reads policy once and
can retarget the model-specific base while retaining its original capability,
file and skill policy. This matches runtime resume, including model-dependent
capabilities, without rereading files or accumulating rendered instructions.

`cai_instruction_writer` runs on a default worker stack and exercises an
8 MiB caller-owned extension while rejecting allocations above 64 KiB and
guarding client/agent construction. It covers substitutions, capability-fragment
filtering across substitutions, repeated tokens, sink cancellation, missing
arguments, allocation failure and callback-backed policy/skill preparation.
`cai_workspace_approval` compares the complete output and instruction status
with live agents across approved and unapproved roots. `smith_model_prompts`
compares complete instruction output across the model catalog/fallback,
disabled patch capability and custom templates while retaining its model-switch
and captured-policy assertions.

The shared formatter in item 1 is implemented as `cai_handover_write` in
`cai/handover.h`. The live runtime now supplies borrowed header fields, text
producers and a retained-history cursor adapter. Its previous private formatter
and per-item seek/drain implementation are removed. The document format,
headings, text filtering and omission markers remain the same.

The formatter takes a synchronous history producer and calls it twice over the
same stable retained view. The first pass keeps only an item category, role,
bounded tool name and frame length; the second streams the text. Provider member
order does not require per-item journal rereads. Goal objectives and developer
instructions are separate text producers, including arbitrarily large values.
Cancellation is checked during both source consumption and output. The caller
owns stable-view validation and discards partial output on failure.

`cai_handover` exercises one-byte and awkward UTF-8/escape splits, exact history
rendering, all message roles, reasoning, tools, non-text omissions, absent text,
source/sink failure, malformed and changed frames, cancellation and each observed
allocation failure. It generates a 32 MiB provider text field with identities
after the text, observes text delivery before the second producer call finishes,
and rejects allocations above 64 KiB or aggregate tracked allocations above
1 MiB. A separate 1,000-item case asserts exactly two history producer calls.
Generated 8 MiB goal and instruction sources also flow without whole-value
allocation under those guards.
Both this check and the journal projector use the same allocation guard and run
on default worker stacks. Existing runtime Markdown checks cover the thin live
adapter, header fields and file publication.

The application's native Markdown adapter implements item 3 for closed
conversations. It preserves restored model/effort/preset and goal semantics,
current authorized instructions, and the execution-history decoding/omission
contract. It uses two linear history passes, one metadata pass (a second when
streaming a goal) and one execution pass over the same indexed view. Paired
exports then reuse that view for JSONL. No read is performed per context item.

`cai_journal_projection` also exercises this actual application adapter on a
default worker stack. It generates an 8 MiB goal and 8 MiB provider text while
rejecting allocations over 64 KiB and tracked live memory over 1 MiB, observes
delivery during production, verifies a constant read count and checks bounded
execution chunks, malformed-record notices, instruction approval, invalid and
oversized settings, early allocation failures, and source/sink failures.
`smith_model_prompts` compares complete retargeted prepared instructions with
the live agent across ten model profiles after changing the source file;
the original captured policy survives in both.

The long-export owner-dispatch requirement remains incomplete. Removing the
offline runtime does not by itself make active `/export` asynchronous.

`cai_cli_journal_view` now owns the native retained selection for one borrowed
collection receiver and captured head. It exposes retained JSONL, final metadata,
decoded framed history and visible event producers. The JSONL entry point uses
this view. Successful calls can be composed without rescanning the selection or
opening another receiver; read or consumer failure makes the view terminal.
All range reads retain the captured head precondition. Cancellation options are
copied at open, forwarded to the SDK, and checked before/during payload parsing
and output, including the initial index scan. No read callback reenters storage.

The journal check now composes the view's history producer with the core
handover formatter, checks repeated JSONL/metadata/history/event operations,
trim visibility and original timestamps, and rejects nested reads. A generated
case streams 8 MiB metadata and history values through repeated projections
under the existing allocation guards, with exactly one head capture and a
constant number of reads. Cancellation stops a generated index/read after
8 KiB rather than consuming the whole value. This private view is the adapter
boundary now used by both offline formats, including header/instruction
preparation and streamed execution-record rendering.
