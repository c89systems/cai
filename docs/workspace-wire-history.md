# Prompt recall over workspace sockets

Prompt recall uses the context's existing encrypted `prompts` collection. It is
independent of the session transcript and model context. The server owns
its writer and configured count retention; terminal clients open no store,
lease or SDK client. The context's existing transcript reader worker also
services prompt reads, with a separate public collection receiver retained
until that context closes. There is no thread or receiver per entry/window.

## Read path

`cai_app_client_prompt_history` sends HISTORY_GET with the same definite
`[session, collection, position, sequence]` shape as SUBSCRIBE. For this operation
collection is empty, position is the requested count (zero uses server policy),
and sequence is zero. Envelope request is a fresh subscription ID; stream and
sequence are zero. The session epoch is either the expected epoch or zero to resolve the
explicit session. Reads are allowed to observers and do not require a producer.
The ordinary transcript and recall share the subscription ID namespace but have
independent active streams and callbacks.

The reply uses existing REPLAY_BEGIN/STREAM_BEGIN/STREAM_CHUNK/STREAM_END/
REPLAY_END frames, correlated by request/stream. REPLAY_BEGIN supplies the
session, collection identity, captured upper position and effective recall
limit in visibility. Each entry has type `prompt_history`, sequence equal to
its collection position and bounded data spans. STREAM_END accepts one input;
REPLAY_END completes the suffix. Zero/off produces an empty completed load.
Errors use SUBSCRIPTION_ERROR and affect recall rather than model context.

Reads capture an upper position, select the count-bounded suffix and advance
through native range/cursor windows. They do not scan earlier prompt bodies or
collect the whole history. Journal and recall jobs share the retained worker
and round-robin window scheduling; each attachment/channel has its own bounded
output ring. A slow reader is explicitly rejected without blocking the owner
or another reader indefinitely. Context close and detach cancel both channels.

An individual prompt is an already bounded input (maximum 66,048 bytes). The
mapped storage reader decodes one such input, and the terminal assembles one
input for Softline's string-based history API. Neither codec/client retains
the history as a combined value. Softline owns the configured in-memory recall.

## Writes and draft exit

HISTORY_APPEND has a definite `[text]` payload. Its envelope carries a nonzero
connection-scoped request ID, original authority generation and session epoch
epoch, with sequence/stream zero. It requires a bound controlling attachment.
The owner checks authorization immediately before append. The collection
identity includes the verified server instance, attachment and save request;
the SDK append is idempotent. Repeating the last identical request returns its
cached result, while changed content/scope or older IDs are rejected. It never
submits the text to the agent.
Saving text identical to the newest durable prompt returns that existing
position. This preserves native recall deduplication when an unchanged draft
is saved again after reconnect. The owner reads only that last entry through
the already-owned collection; it does not reload prompt history or reopen the
store.

RESULT category 4 reports the save outcome independently of queue watermarks.
Its item ID is the committed prompt collection position. Only this successful
result acknowledges durability. A failed/uncertain handoff is never silently
resubmitted using a new identity.

For native submissions, the terminal records canonical prompt attempts after
Softline finalizes completion prefixes and before handing off model work.
Recall failure is reported separately and cannot relabel queue admission.
Generated startup settings do not become recalled user prompts. CLI instruction
text still does. Alt-Q sends the current draft and waits for its successful save
receipt before detaching. Editing the draft while the save is pending cancels
exit; rejection also leaves the editor/draft available. With recall unavailable
or disabled, a nonempty draft is retained; clearing it makes quitting explicit.

Recall loading never blocks model submission or editing. Native history
auto-recording is temporarily disabled during initial import. New canonical
inputs occupy a suffix bounded by the recall count and merge into Softline
after the snapshot. Their committed positions exclude entries already included
at that watermark, avoiding duplicates and inverted chronology. Returning to an
already initialized session reuses its editor/history instead of loading
it repeatedly. A live `/history COUNT` refreshes the current recall policy.

Verification covers fragmented CBOR, maximum input size, multi-window loads,
duplicate/changed saves, observer reads/rejected writes, count/off/on policies,
unchanged SDK-client counts and exclusion from inference requests. Native PTY
tests save an unsent Alt-Q draft, reconnect, retrieve it with Arrow-Up and verify
that no inference request was caused by saving or recalling it.
