# Frontend-owned shell recording

`/shellshare` and `!sh+` run in the frontend's local PTY and explicitly send
output to the controller's existing bounded-tail runtime capture. `/shell`,
`!sh`, Alt-S and pagers send no output to this channel. Ordinary backend bang
commands retain their separate execution path.

`USER_SHELL` carries the authenticated session epoch, original authority
generation, nonzero request ID, and zero envelope sequence/stream fields.
Its definite CBOR array is:

```
[operation, recording, sequence, exit_code, duration_ms, bytes]
```

BEGIN (0) supplies 1..8192 command bytes without NUL. Recording/sequence are
zero; the accepted request ID becomes the attachment-local recording ID.
WRITE (1) supplies at most 4096 arbitrary bytes, including NUL. FINISH (2)
supplies no bytes, an exit code in 0..255 and monotonic duration milliseconds.
WRITE/FINISH name the original recording and next consecutive sequence,
starting at 1. The owner checks authority, binding, context, increasing request
IDs and exact recording order before consuming a completed frame.

Each operation receives a shell RESULT before another chunk is submitted.
The PTY relay retains at most one 4 KiB chunk and drives progress through its
descriptor callback. It never blocks inside a Softline callback, accumulates
complete output, spools it, or opens an SDK client. BEGIN/FINISH handshakes run
outside editor/relay callbacks, service visited owners, and have a ten-second
acknowledgement deadline. Native queue synchronization after return remains
asynchronous; its completion notice precedes transcript catch-up once input is
usable again.

Acknowledgement means owner consumption, not persistence of raw output. The
runtime incrementally sanitizes its configured rolling tail. Only FINISH saves
the tail and tagged user-role context; discarded raw prefixes are neither
journaled nor logged. Unknown operations are not retried under new authority.
Disconnect, revocation and context closure finalize an interrupted capture
with exit 129 and elapsed owner time, releasing its hold on new model turns.
Only the frontend stops/joins its actual process; no PID crosses this protocol.

C clients call `cai_app_client_shell`, observe `CAI_APP_CLIENT_SHELL_RESULT`,
and copy named status/recording/range metadata with
`cai_app_client_get_shell_result`. The private RESULT encoding uses its generic
revision/token slots for journal range endpoints, not queue capabilities.
`cai_agent_runtime_user_shell_range` obtains that range without a journal read.
Lua exposes `client:shell{...}` and `client:get_shell_result()` using binary
strings and decimal identities.

Immediate catch-up suppresses only shell records in the local PTY's acknowledged
range. Interleaved assistant/tool entries remain visible. Unknown range ends
stop at that recording's completion. Other attachments and later restarts show
the saved tail through the shared UTF-8/control sanitizer. These presentation
ranges never alter the journal or its cursor.

Tests cover binary chunks, malformed metadata, sequence rejection, takeover,
disconnect cleanup, bounded-tail context, storage-client count, PTY backpressure,
restart/export, split C1 controls and interleaved-message preservation.
