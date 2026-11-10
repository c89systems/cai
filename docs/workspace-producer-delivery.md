# Producer-settled delivery

The controller owns each session's accepted pending input. The frontend
owns drafts, keystrokes, cursor/undo state and local editing. Reserving an
already-queued entry for editing uses the queue's existing BEGIN_EDIT hold;
only the final replacement is sent back. No intermediate draft is transmitted.

Submission and reservation are asynchronous. An accepted OFFER receipt can
trigger a dependent BEGIN_EDIT request after Enter and Alt-E arrive in one
native key burst. Claiming the item between those operations loses the edit.

A producer registers managed delivery when binding. Its connection starts
gated, and each subsequent request closes its gate. Once acknowledgements and
dependent requests settle, the producer sends INPUT_SETTLED with its original
session epoch, control generation and observed processed request watermark. This is
ephemeral queue availability: no request ID, receipt, journal event, draft or
editor state is added. Unchanged notifications are coalesced.

The controller permits delivery after draining the producer's requests at
exactly the announced watermark. An old notification cannot release a later
accepted offer or a new authority/context. Disconnected or revoked producers
cannot hold accepted work indefinitely. Durable edit/approval holds retain
their existing independent rules; other producers remain independent.

Softline's adapter issues this notification from its native idle callback,
after inspecting pending proposals and accepted receipts awaiting follow-up.
It sends no notifications for ordinary typing or draft emptiness changes.
Socket callbacks never substitute for that native boundary. Wake descriptors
update transport interests without reentering Softline from submission.

This adds no timer, grace period, second queue or receipt replay. Reconnect
uses the existing explicit synchronization and original-receipt resolution
before announcing settlement. Producers without dependent asynchronous
requests retain ordinary delivery and need not register managed delivery.

Authority changes rearm the current controlling producer under its new
generation, including takeover without reconnecting. Repeating registration
for the same scope preserves a valid notification received earlier in that
transport pump. Unknown receipts keep the producer unsettled until explicit
reconciliation; rejected receipts do not block unrelated accepted work.

Core hosts register with `cai_input_connection_manage_delivery`, accept
`cai_input_connection_settled` notifications, and combine
`cai_input_controller_producer_ready` with their scheduling policy. Socket
frontends use `cai_app_client_manage_delivery` before binding and
`cai_app_client_settled` with the observed watermark. These C APIs have no
Softline, renderer, Lua or SDK-storage dependency.

The application combines producer settlement with existing feed, request and
turn boundaries. Settlement cannot make an unsafe command ready, change
steering intent, release stopped backlog or bypass approval/edit holds.
