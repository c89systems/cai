# Workspace command execution

Commands use the conversation's existing input queue, policy inventory and
handler. Admission, edit/cancel, claim and safe-boundary selection remain in
the backend. A server never runs a second interpreter or creates a renderer.

The headless owner records replies as execution chunks outside model context:

| Kind | Meaning |
| --- | --- |
| `command.input` | Original accepted command, before execution. |
| `command.markdown` | Streamed Markdown reply; each field ends explicitly. |
| `command.completed` | Synchronous handler succeeded. Asynchronous runtime controls still publish their own eventual status. |
| `command.failed` | Handler failed: lifecycle/code/HTTP status, followed by message, detail, provider code and request ID fields when available. |
| `command.frontend` | Hand off the original command to its originating frontend. |
| `command.handoff` | Close output in a conversation that is being left. |
| `application.markdown` | Owner-generated operator notice outside command dispatch. |

The call identity is `command:SOURCE_SESSION:PRODUCER:REQUEST:ITEM`.
These are the already-admitted queue identities, never
new request IDs. Replies reuse the same call identity across a context switch;
the execution record supplies the actual session receiving each chunk.
Large documents flow in bounded UTF-8 chunks without a complete document or
JSON accumulator. The runtime's existing journal serialization owns sequence
assignment and persistence; there is no second store or writer lease.

Frontend-local theme/output preferences, quit, pager and terminal handoff are
delegated instead of executed by the server. A frontend may act only on a live
record addressed to its own producer, after verifying its current connection
and conversation. Replay displays history and never repeats frontend actions.
Other observers do not apply another terminal's theme or quit request. The
Softline command consumer now applies live actions from its own producer. The
terminal's remaining delegate/parity work is listed in
[terminal attachment](terminal-attachment.md).

Backend commands keep the current native handler semantics, including context
selection, goals, inference settings, catalog queries, retention and shell
recording. Owner readiness watches reviews and ordinary user-shell processes
without Softline watches. Ordinary command errors become visible results and
leave the owner available for later work; persistence/allocation failures keep
their existing fatal handling.

This is an incremental cutover. The legacy terminal still uses its existing
presenter adapter. The headless document adapter emits semantic history for
socket/native consumers. Automatic terminal attach and frontend-local theme/
output actions are connected. Observer query routing and full delegate parity
remain part of the overall frontend work.

`cai_agent_runtime_execution_open` supplies the reusable core writer used by
this adapter. It copies metadata, accepts fragmented UTF-8, emits bounded chunks
to the existing journal, and requires an explicit finish. Closing abandons a
pending suffix; an append failure makes the writer unusable so an uncertain
append cannot be retried. It never adds operator output to model context.

Verification: `cai_tests.runtime_operator_execution` runs on a default-stack
worker and covers incremental emission, split UTF-8, explicit final/empty
records, abandoned suffixes and sticky append failures. The `server_commands`
scenario in `cai_app_workspace_server` uses real Unix sockets and aborting
renderer constructors. It verifies feed queries during a paused provider
response, settings held until a request boundary, failures followed by
successful commands, observer rejection, frontend delegation, shell and review
completion, and a `/new` reply replayed from the new conversation. Store-client
open counts must remain constant through these operations.
