# Lua workspace attachment

`require("cai.app").client(config)` wraps the public `cai_app_client` API.
It connects to an existing workspace owner. It never starts a process, opens
an SDK/store/runtime, creates a terminal or invokes Lua on a worker thread.
Configuration accepts `workspace`, `runtime_directory`, optional 16-byte
`expected_instance`, `handshake_timeout_ms`, `backlog_limit`, and sorted
`requirements = {{name = NAME, digest = SHA256_BYTES}, ...}`. Requirement
digests have exactly 32 bytes and use the same pre-admission checks as C.

Optional `on_event`, `on_transcript`, `on_execution`, `on_prompt_history` and
`on_catalog` functions each receive one table during `pump` or explicit `wait`.
Each call copies only that bounded metadata row or borrowed byte span into Lua.
No complete transcript, tool result, prompt history, catalog or queue snapshot
is accumulated by the binding. The consumer owns any retained presentation.
Transcript `data` is a binary Lua string with its original `length`: do not
assume a transport span ends a UTF-8 character, word or event. BEGIN and DATA
remain provisional until END; preserve the C API's cursor/validation rules.

All unsigned 64-bit identities, revisions, positions and times are decimal
strings on output. Inputs accept those strings or nonnegative Lua integers;
floats, negative values and overflow are rejected. Lua integers represent
ordinary enum/status codes, signed offsets and entry counts. Enum fields keep
their public C numeric values. The module exports `CLIENT_*`, `WORKSPACE_*`,
`TRANSCRIPT_*`, `INPUT_OBSERVATION_*`, `INPUT_ACCEPTED/REJECTED/UNKNOWN` and
`CATALOG_*` constants for comparing those fields. `operation` accepts the
public queue operation
names used by `cai.pending_input`; `forget_origin` is not a frontend operation.

Methods follow C ownership and explicit request identities:

| Method | Arguments/result |
| --- | --- |
| `prepare()` | Returns `{fd, events, deadline_ms}`; descriptor is borrowed. |
| `pump([now_ms])` | Nonblocking bounded progress; defaults to the local monotonic clock. |
| `wait([timeout_ms])` | Explicit blocking convenience: poll the prepared descriptor/deadline, then pump. Default -1 waits without an extra timeout. |
| `get_state()`, `get_view()` | Copy one metadata view; view is nil before the first completed observation. |
| `bind{request_id, conversation, producer, session}` | Select producer; omitted conversation/producer are zero. |
| `request{conversation, generation, request_id, operation, ...}` | Queue request, including item/revision/edit/text/intent/enabled/target/origin fields. No automatic IDs, retries or generation refresh. |
| `takeover{request_id,generation}`, `shutdown{...}`, `cancel{...}` | Connection controls, independent of queue IDs. |
| `subscribe{request_id,conversation,session,collection,after_position,after_sequence,replay_turns}` | Transcript subscription; omitted replay limit is -1. |
| `prompt_history{request_id,conversation,session,limit}` | Independent bounded recall stream; omitted limit uses owner policy. |
| `save_prompt{request_id,generation,text}` | Durable draft request; await the history result before claiming it was saved. |
| `unsubscribe()`, `detach()` | End transcript only, or drain an orderly attachment detach using subsequent pump/wait calls. |
| `directory()` | Canonical owner workspace. |
| `close()` | Immediate idempotent local release, no implicit flush, retry or draft submission. |

Success returns true (or the documented value). Failure returns
`nil, message, cai_status_code`. Callback exceptions fail that attachment and
return through pump/wait; they do not escape through the C transport stack.
Error messages retain a bounded 1,024-byte preview. Callbacks are synchronous
and cannot yield; exceptions with non-string values use a fixed diagnostic.
Callbacks may query metadata or submit requests. Reentrant pump/wait, detach,
unsubscribe and close are rejected; another client remains independent.
Callback functions are retained as userdata values, so callback cycles are
collectable. No permanent registry reference keeps an abandoned client alive.

`event` includes the C event `type`, request identity, validated `state`, and
optional `input` or `status`. `execution` contains one decoded `record`, its
authenticated `boundary`, `replay`/`subagent` flags, or a semantic warning with
`code` and `reason`. Catalog/recall callbacks preserve their C BEGIN/ITEM/END
semantics. Status is presentation data, never authority. Use current instance
and original generation/producer scopes exactly as with the C client.

As with C, a successful request handoff is not approval. Wait for the matching
input/control/history outcome or validated ROLE change. An observer can send a
request and receive a rejection; an old queue request retains its old authority
scope. Taking control after detachment requires observing the new generation
before explicitly choosing that scope. The binding never refreshes it for you.

This provides the client side of Lua embedding. Asynchronous Lua owner
lifecycle/observer dispatch and conversion of the synchronous `app:run`
composition remain separate cutover work.
