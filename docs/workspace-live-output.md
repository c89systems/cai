# Live shell output

Backend bang commands (`!COMMAND`) keep running on the owner. Their raw output
flows incrementally to subscribed frontends through the existing bounded live
rings and socket backlog. No extra worker, SDK client, output spool or complete
command-output value is created. The runtime still retains only its configured
tail; live raw bytes are neither journaled nor logged.

Two server-to-client CBOR records use the active presentation subscription's
request/stream and conversation. Envelope sequence anchors the recording to its
durable `user_shell_started` entry. `SHELL_OUTPUT` has payload `[offset, bytes]`,
with 1..4096 arbitrary bytes and consecutive raw-byte offsets starting at zero.
`SHELL_OUTPUT_END` has payload `[offset]`, the final raw byte count. Both are
exposed as typed `cai_transcript_event` variants in C and Lua. These live-only
variants have no durable position and never acknowledge or advance a cursor.
Malformed identity, offset, ordering or callback rejection terminates that
attachment; bytes are delivered only after their bounded wire frame validates.

The same ring orders shell-start, transient bytes, transient end and saved tail.
A start appended after a replay watermark can queue its bytes behind that start
while historical replay is still draining. A late subscriber cannot join an
already-partial raw recording: it receives the retained tail when it completes.
No transient output enters prompt recall or the model context.

A frontend which accepted every live chunk and its end marker suppresses only
that recording's saved tail during immediate display. It still consumes the
durable records and their cursor positions. Later replay/export uses the tail.
One accepted bang-command receipt is shown; its recording-start metadata does
not print a second copy of the command. Imported journals without command
receipts still show their shell command.

If a subscriber falls behind its fixed ring or a native catch-up is required,
the owner stops transient delivery for that recording. It does not wait for
that frontend or drop durable events. The terminal preserves any displayed
prefix and prints an interruption notice before the retained tail. A valid
end marker is never invented after a gap. Transport replacement/disconnect
discards partial live state; normal replay/catch-up recovers durable output.

Verification includes byte-fragmented CBOR, binary NUL/C1 data, invalid offsets
and identities, callback failure, cursor invariants, duplicate end rejection,
one fast and one stalled subscriber, actual gated shell processes, cancellation,
status queries while a shell runs, bounded context, export and restart. A
separate Softline completion limitation currently affects literal `$`/`@` in
typed shell drafts when the corresponding completion provider is available;
empty skill catalogs no longer activate `$`. See the upstream
activation-predicate CR. This limitation is unrelated
to output capture, and CAI does not rewrite submitted shell text to conceal it.
