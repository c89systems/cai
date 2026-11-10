# Network prompts and workspace events

This is the implementation specification. These options and tools are not yet shipped.

## Ownership

The Softline frontend owns its TCP prompt listener and all agent-requested outbound TCP prompt connections. The workspace backend owns multicast publication, multicast reception tools, and endpoint registration in the existing history store. A frontend never opens another Pouch client. It announces capabilities and endpoints through its existing authenticated workspace CBOR connection.

Reusable bounded socket, address, UTF-8 and event-writing code belongs in libcai. Tool declarations and host callbacks must also be usable by runtime embedders. Application configuration, Pouch registration, terminal editing and the concrete frontend action adapter belong in libcai_app. Core libcai acquires no storage, terminal or renderer dependency.

## TCP prompts

`--prompt-listener ADDRESS` enables one listener for the frontend. An empty address disables it. Accept `PORT`, `:PORT`, `HOST:PORT`, and bracketed IPv6 addresses. `--proto tcpv4|tcpv6` selects the family; the default is `tcpv4`. Omitted hosts mean `127.0.0.1` or `::1`, never a wildcard. Resolve hostnames once when opening the listener. IPv6 listeners are IPv6-only.

One accepted connection carries one UTF-8 prompt. Ordinary EOF ends the prompt; no framing, request header or client implementation is required. Newlines are text, not Enter key events. Empty connections submit nothing. A reset, malformed/incomplete UTF-8, embedded NUL, or exceeding the existing prompt-size limit rejects the prompt rather than submitting a prefix. While receiving one prompt, accept and immediately reject additional connections rather than leaving them as future prompts in the listen backlog. Extract the existing strict incremental UTF-8 validation from libcai's CBOR handling for reuse rather than depending on Softline internals or adding a second incompatible decoder; keep its existing ASCII fast path.

When the editor draft is empty, insert received complete UTF-8 characters using `sl_insert()` on the editor-owner thread, then call `sl_submit()` once at EOF. When a local draft exists, preserve it and offer the completed remote prompt to Softline's native turn queue. Never reinterpret it as steering or submit directly to the model. Queue admission, editing, cancellation and receipts use the existing Softline backend policy. A connection is pinned to its original editor/conversation; switching must not submit it to another conversation. Local draft changes must prevent automatic submission of mixed local and remote text.

`--simulate-typing [MS]` belongs only to the listener. Omission disables pacing; a bare flag selects 30 ms. Pace complete Unicode scalar values using the frontend's readiness/deadline mechanism, not sleeps in its event callback. EOF submits only after pending characters have been inserted. Retain bounded pending bytes and apply socket backpressure. Do not buffer an unbounded socket stream while typing slowly. Timers must not wake an idle frontend when no input is pending.

`--feed-tcp ADDRESS` is an executable-only stdin-to-TCP utility. Process it before normal CAI configuration, banner, workspace discovery, authentication, storage or agent startup. It understands the explicit address/family flags and ordinary `LOG_*` logging setup. It forwards arbitrary stdin bytes unchanged, handles short writes, and closes after stdin EOF. It has no Unicode parsing or simulated typing. Its libpslog JSON records describe connection, transfer and failure; successful local transmission does not claim application-level delivery or inference completion. nc and socat are equally valid senders.

## Multicast publication

Multicast publication is disabled by default. `--event-multicast` selects automatic allocation; an explicit `GROUP:PORT` selects a fixed destination. Configuration uses an empty string for disabled and `auto` for automatic allocation. An ordinary frontend invocation can supply these settings when starting a new owner, but attaching to an existing owner must not silently reconfigure its publisher.

The configurable CAI default pool is `239.193.89.1` through `239.193.89.254`, with destination port `49189`. These are application conventions inside organization-local multicast space, not globally assigned CAI addresses. The organization-local block is defined by [RFC 2365](https://www.rfc-editor.org/rfc/rfc2365.html). Allocate the first free group using an atomic store lease, not a count of existing records or a check-then-write. Concurrent publishers sharing the registry cannot claim the same automatic slot. Other applications or independently configured stores can still choose the same destination. Explicit destinations do not silently increment. Exhaustion is an actionable startup error. Permit overriding the pool and port.

The sender uses nonblocking datagram writes with a bounded payload. The payload is journal-style event JSONL, preserving the stored event fields and semantics. A JSON line may cross several datagrams; do not add packet wrappers, extra newlines, full-event buffers or spools. This is best-effort delivery: loss, duplication or reordering can invalidate a line. Receivers joining mid-event discard the initial partial line and resume at the next line boundary. This stream is observation, not a journal replication/recovery protocol. Internal commit/state records are not presentation events and are not published as conversation output.

Use the existing event serialization and live publication path, without rereading or rematerializing the conversation. Include conversation identity at stream switches so a receiver can identify a changed active session. Publication must continue with no frontend attached and must never block inference or durable journal writes. Network errors update counters and produce bounded diagnostics; do not recursively multicast publisher diagnostics.

Configuration uses socat-related names: `ip_multicast_if`, `ip_multicast_ttl` (default 1), and `disable_multicast_loop` (default false). Infer the multicast family from the destination independently of the TCP `proto` setting. Validate multicast destinations and explicit interface selection. A wildcard TCP bind address is not an advertised reply address.

## Discovery and attachment metadata

Register enabled TCP listeners and multicast publishers under the application namespace prefix, normally `cai.network`, using the backend's existing history client. Each record identifies endpoint kind and ID, protocol, bind and usable advertised addresses, host identity, workspace path/name, server instance, frontend attachment where applicable, active conversation, and applicable socat instructions. Do not publish credentials. An optional advertised TCP address handles wildcard binds and externally reachable names; loopback addresses remain explicitly host-local.

The process holding the capability owns its registration through the backend. Publish only after successful socket setup; withdraw on normal listener/frontend/server close. Registration lifetime and automatic allocation claims must be crash-safe and use retained leases, not per-event acquisition. Stale records are discovery hints, never proof of a live endpoint. Prevent an old owner from deleting a successor's record. Conversation switches update attribution without opening a second store or registering a second listener.

`cai -l` and `cai -L` include registered endpoints with their workspace/conversation listings, observing existing local/all scope. Headless startup logs the actual publication destination, interface and ready-to-use socat command. Every attaching frontend receives typed current network metadata over CBOR, including an already-running owner's actual allocation. Render that metadata as a startup notice; do not parse log messages or synthesize user prompts to communicate it.

Example receive command, with its interface selected from the actual configuration:

```sh
socat -u 'UDP4-RECV:49189,reuseaddr,ip-add-membership=239.193.89.1:127.0.0.1' STDOUT
```

## Agent tools and frontend actions

`event_stream_listen` is enabled by default. It receives multicast on the backend, with explicit bounded duration/output and cancellation. Enabling this tool does not enable a publisher or start a socket automatically. Treat received events as external data. Preserve journal JSONL and report loss/parse/truncation conditions instead of claiming a complete response.

The opt-in network tool set provides `network_list`, `prompt_listener_info`, and `prompt_send`; it is disabled by default. Listing returns registered endpoints, host locality, workspace/name/session and usage instructions. Listener info returns the selected session/frontend's usable reply-to address, stable endpoint identity and an example reply tool call. Do not guess one endpoint when the caller's selection is ambiguous.

`prompt_send` sends a typed CBOR action through the workspace connection to the selected frontend. That frontend performs the TCP connection and transfer under its explicitly enabled sending capability, then returns a correlated completion/failure receipt. The backend never falls back to making the TCP connection itself. No frontend means a clear capability error. Target identity, request identity, maximum size and deadline are checked; disconnect/uncertain delivery is never automatically retried. This is a network action, not an arbitrary shell command. It must not take over the terminal or block its event loop.

Reply instructions are included only when requested. They identify the originating listener and the tool invocation needed to reply. Multiple attached frontends require explicit selection or the unambiguous controlling frontend. No automatic fan-out, automatic context discovery, or autonomous reply loop is introduced.

## Settings and validation

Use `[ui] prompt_listener`, `proto`, and `simulate_typing_ms` for frontend settings. Use `[server] event_multicast`, `event_multicast_base`, `event_multicast_port`, `ip_multicast_if`, `ip_multicast_ttl`, and `disable_multicast_loop` for publication. Use `[agent] network_tools = false` and `disable_event_stream_listener = false` for tool capabilities. Add matching application-prefixed environment variables and CLI options. Zero typing delay means disabled. Empty listener/destination means disabled.

Verify the TCP path against raw socket senders as well as `--feed-tcp`: multiline input, every split of UTF-8 characters, incomplete/invalid input, empty input, reset, size limits, concurrent connection rejection, existing draft preservation, turn-queue admission, busy generation, conversation/workspace switching, cancellation, pacing/backpressure, and frontend responsiveness. Prove the feeder bypasses broken CAI config/auth/storage and forwards arbitrary binary bytes unchanged.

Verify multicast JSONL byte equivalence, records larger than a datagram, late joins, bounded memory, send failure/nonblocking behavior, no-frontends operation, automatic allocation races, pool exhaustion, registration cleanup/crash staleness, reconnect notices, interface/family settings, listing scopes, tool opt-in defaults, receive limits/cancellation, and correlated frontend-only sends including missing/disconnected clients. Use ordinary Debug checks and focused musl tests with default worker stacks; rebuild and exercise the production musl binary. Keep the full release matrix deferred.
