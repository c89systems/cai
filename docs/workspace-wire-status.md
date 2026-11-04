# Workspace status projection

This is the next application binding to implement. Queue and transcript
delivery already exist; the socket client does not yet expose this status
projection. Finite binary64 is its codec prerequisite.

Status is an ephemeral presentation view of the existing owner, outside model
input and the journal. Attaching does not restore context, read logs or open a
storage client. One server-side cached view serves controllers and observers.
Clients may cache explicitly bounded status metadata. Authority still comes
from the verified connection's ROLE generation.

## Observation and framing

Use STATUS in the existing envelope: request/stream/generation/attachment zero,
a nonzero per-owner revision in sequence, and the current conversation epoch
(or zero before readiness). Revisions are scoped to the verified owner instance.

The payload is a definite `[available, body]` array. A successful body contains
these definite groups in order:

| Group | Values |
| --- | --- |
| Lifecycle | running, runtime_ready, application_outcome, exit_code, runtime_state |
| Identity | session_id, provider, last_response_id, provider_conversation_id, owned_log_path |
| Effective settings | present_mask, model, reasoning_effort, reasoning_summary, service_tier |
| Pending settings | same shape as effective settings |
| Context and turn | context_window_tokens, context_used_tokens, has_context_usage, turn_active, turn_elapsed_ms, has_last_turn, last_turn_duration_ms, last_turn_finished_unix_seconds |
| Session usage | input_tokens, cached_input_tokens, output_tokens, reasoning_tokens, total_tokens, estimated_spend_usd, estimated_spend_eur, limit_exceeded |
| Goal summary | has_goal, status, has_token_budget, token_budget, tokens_used, remaining_tokens, elapsed_seconds, created_at, updated_at |
| Quota cache | has_short_window, short_window_seconds, short_window_remaining_percent, has_weekly, weekly_remaining_percent, has_credit_balance, credit_balance, credits_unlimited |
| Execution policy | yolo, foreground, yield_time_ms |

Goal objectives and detailed error bodies remain in their command/transcript
paths. They are not copied into every status update. Missing strings use null;
present strings use byte strings without NUL and retain exact contents.
Frontends sanitize/escape display text. Never silently truncate canonical
models, response IDs or log paths. Session/path limits follow existing native
contracts. Booleans are CBOR booleans; enums/nonnegative counters are unsigned;
signed native context/timestamp fields retain signed representation.
Currency/quota values use finite binary64 without scaling or decimal round
trips. Presence flags distinguish unavailable values from zero.

Derive frame sizes from known fields and encode directly through the shared
CBOR/transport path, without sizing serialization. The metadata record uses
the existing 128 KiB cap. If it cannot be represented, send available=false
with `[code, reason]`, with reason bounded to 191 bytes. This invalidates only
the status view, not the attachment, accepted input or transcript subscription.
A malformed frame or rejecting consumer still causes protocol/transport failure.

Stage one typed metadata candidate, with string allocation bounded by the frame
cap. Publish only after protocol COMPLETE; partial EOF discards the candidate.
The last accepted view may remain inspectable after disconnect, explicitly
stale. Observing status requires no queue producer and allocates none.

## Publication and clocks

Publish semantic changes in lifecycle, settings, IDs, goal state/budget,
context/usage, quota, execution policy or log path. Coalesce superseded unsent
views for a slow client, respect transport credit and isolate backpressure.
Coalescing applies before encoding: an already-started record must finish or
the connection must close. Revisions increase, but may have gaps when newer
unsent views replace older ones.
No worker, runtime, timer or provider request is created per attachment.

Elapsed-time-only changes do not trigger publication. Clients anchor received
elapsed values to their own monotonic clock and advance active turn/goal
displays locally. A pause/completion/stop publishes the final duration.
A new observation reanchors the display.

An unsent view may request one immediate owner-loop pass. Do not schedule from
unconditional bridge-publication calls: an unchanged publication followed by a
zero-timeout pump must not cause an idle feedback loop. Only a changed semantic
view, an unsent newer revision and available credit make status work ready.

Quota collection belongs to the backend's shared debounced worker. Frontends
neither query the provider nor parse pslog. Moving that worker out of the
legacy Softline watch wiring is part of this cutover. Initial absence is
explicit; an arriving quota cache produces a new status revision.

## Verification

Prove real-socket delivery without frontend SDK clients; initial observation
without a producer; effective/pending setting changes; goals without objectives;
exact finite numbers; null/adversarial strings; callback failure; malformed and
truncated frames; stale state after disconnect; takeover without status-based
authority; slow-reader isolation; no idle/per-token status flood; frozen final
durations and locally advancing active clocks. Keep C/Lua access and the
Softline/HTML adapters on this same projection.
