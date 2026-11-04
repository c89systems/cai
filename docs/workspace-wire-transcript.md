# Workspace transcript subscriptions

Application binding for the existing `cai.workspace` envelope. This shares the
workspace socket with input observations; it introduces no protocol version,
additional journal or per-client runtime. Both controllers and observers may
subscribe without opening a queue producer. Context restoration runs on the
server, never on attachment.

Payloads are definite CBOR arrays. Integers are unsigned except the signed UTC
offset. Session/type byte strings are limited to 128 bytes, collection identities
to 20, and error reasons to 191; those strings cannot contain NUL. Data chunks
are binary byte strings of at most 4,096 bytes and may split UTF-8 characters.
Encoders derive frame lengths from known fields and chunks, then write directly;
there is no sizing serialization or complete-event accumulator.

| Envelope kind | Payload |
| --- | --- |
| SUBSCRIBE | `[session, expected_collection, after_position, after_sequence]` |
| UNSUBSCRIBE | `[]` |
| REPLAY_BEGIN | `[session, collection, watermark, visibility]` |
| STREAM_BEGIN | `[timestamp_ns, utc_offset_seconds, type, has_data]` |
| STREAM_CHUNK | `[offset, bytes]` |
| STREAM_END | `[position, length]` |
| REPLAY_END | `[watermark]` |
| SUBSCRIPTION_ERROR | `[failure, code, reason]` |

SUBSCRIBE uses a fresh, strictly increasing, nonzero request ID per attachment
and stream zero. It names the active persistent session explicitly; conversation
zero resolves that session's current epoch. An initial read may leave the
expected collection empty. Resuming from a position requires the original
collection identity and last accepted event sequence. Input/control request IDs
have separate scopes. Every response uses the subscription ID as both request
and stream, with its resolved conversation epoch. Rejected requests may report
conversation zero. UNSUBSCRIBE uses the original request/stream; repeating an
old cancellation never cancels a newer subscription. A stale subscribe request
also cannot cancel a newer stream. A fresh replacement abandons the old stream,
even if that replacement is refused; it never falls back to the old selection.

Only STREAM_BEGIN/CHUNK/END carry a nonzero envelope sequence: the durable event
sequence, equal across that record's spans. Chunks have contiguous offsets from
zero. STREAM_END validates the total length and increasing collection position;
it is emitted only after the SDK authenticates the complete stored entry.
BEGIN/DATA are provisional. The client commits its reconnect cursor only after
the consumer accepts END. A completed REPLAY_END advances the position to the
captured watermark, including skipped commit records, while retaining the last
accepted event sequence. It never resubmits model input.

REPLAY_BEGIN captures the collection incarnation, upper position and recovery
revision. Historical events end at that watermark. Committed live events after
the watermark are armed at the same journal append seam and delivered after
REPLAY_END exactly once. Commits are discarded incrementally, not decoded into
complete context objects. A history cut invalidates subscriptions, including
when publication succeeded but its durable acknowledgement was lost. Dropped,
expired or replaced collections fail explicitly; readers do not silently bind
to a replacement with the same name.

The publisher retains one default-stack replay worker per conversation and one
public collection receiver using the existing SDK client. Native reads yield
between windows of 32 stored records, rotating ready subscribers. There is no
worker per frontend, read window or event. This worker is separate from context
restoration. The existing small session-identity loader can create a temporary
push-to-pull helper when the receiver first opens; subsequent windows and
subscriptions reuse the receiver.

Each of at most 16 subscribers has separate bounded history/live rings of 32
spans, each holding at most one 4 KiB chunk plus fixed metadata. A historical
producer waits at most one second without ring credit, then fails only that
subscription. Agent append notifications never wait for frontend credit: live
overflow fails the subscription immediately. A single live record larger than
its ring capacity also requires resynchronization from its durable position;
the native historical reader can stream arbitrarily large logical records.
No whole event is copied into a subscriber queue. Memory does not grow with
history or logical record length. The host gates writes on transport credit,
uses descriptor readiness for new spans, and does not poll idle subscriptions.

Failures follow `cai_transcript_failure`: FAILED, SLOW, HISTORY_CHANGED, or
CONTEXT_CLOSED, paired with an actionable CAI error code/reason. ERROR abandons
any provisional record; accepted cursors survive. It terminates only that
transcript subscription. Input admission, pending work and other attachments
remain independent. Explicit resubscription is required. Invalid wire ordering,
framing or a rejecting consumer instead terminates the receiving attachment.

`cai/app_client.h` exposes borrowed spans through its independent transcript
callback and fixed cursor metadata through `get_state`. It creates no SDK
client, worker, complete-event mirror or renderer. Callbacks run on the opening
thread and may query state/send requests, but cannot reenter pump/detach/close.
Transport loss abandons an incomplete record. Reconnect requires a new verified
connection and an explicit subscription to the saved collection/cursor.

The optional typed `execution` callback decodes `execution` and `subagent`
entries incrementally using the core `cai_execution_decoder`. It retains only
bounded metadata and the defined 4 KiB decoded execution chunk. No encoded JSON,
DOM or complete logical result is collected. Source identity is mandatory for
forwarded subagent entries. Delivery occurs at authenticated EVENT_END, before
the raw END callback and cursor acknowledgement; refusal by either consumer
prevents cursor advancement and fails that attachment. Malformed execution
content is a typed historical warning with a NULL record, code and bounded
reason; accepting it advances past that record and preserves its raw data.
Local allocation failures and malformed transport remain terminal.
Partial/replaced streams
produce no typed chunk. Subscription replacement inside either callback cannot
overwrite the new stream's cursor, or shorten a borrowed callback lifetime.

The shared terminal presenter now has a span consumer for accepted parent
inputs, assistant Markdown and reasoning. It strips the bounded
`input_queue_delivery` provenance header incrementally, displaying the actual
delivery rather than queue-admission JSON. Native strings and socket fragments
share one literal rendering path. BEGIN's original timestamp is retained across
DATA/END, which carry no timestamp themselves. Markdown can continue across the
replay/live watermark without an artificial finish. Reset abandons pending
renderer documents and leaves the local draft alone. Pending Markdown words
can span authenticated delta records, so a reset view needs replay from a
document boundary; the raw transport cursor alone does not capture renderer
state. The complete frontend's reconnect policy remains part of the cutover.
Typed execution/tool
records use their separate presentation path; encoded JSON is never dumped by
the raw span consumer.

This delivery provides explicit native position cursors. Default last-ten-turn
selection, final visibility filtering, typed tool/command presentation and the
complete terminal attachment loop remain subsequent parts of the cutover. Do
not treat the current raw subscription as the final user-facing replay policy.

Verification: `cai_transcript_wire` exercises all shapes, bytewise fragmentation,
binary data, signed offsets, caps, malformed values and incomplete completion;
`cai_app_client` checks cursor acknowledgement, bad ordering, partial EOF,
callback cancellation and stream replacement. `cai_transcript_store` exercises
multi-window native reads, slow-reader isolation, replay/live handover, resume,
shutdown and one-SDK-client reuse. `cai_cli_atomic_trim` covers notifications on
successful and uncertain publication, without notifications on rejected cuts.
`cai_app_workspace_server` uses actual Unix sockets and fixture inference for
typed replay, observer resume, live output, cancellation and last-client stop.
`cai_cli_geometry` proves fragmentation-invariant literal output, Unicode/control
repair, interruption, incremental output before a multi-megabyte producer ends,
delivery-header validation and replay/live Markdown continuity. Its presenter
link has no application/storage objects. `cai_cli_headless` additionally renders
live and restarted replay from the actual executable's Unix socket, and aborts
if its frontend attempts to open a storage client.
