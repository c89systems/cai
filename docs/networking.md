# Network prompts and events

CAI can receive prompts through a frontend TCP listener and publish workspace events through backend UDP multicast. Both are disabled by default. The frontend owns prompt editing and outbound prompt connections; the backend owns event publication, observation tools and endpoint registration.

## Receive prompts

```sh
cai --prompt-listener 1911
printf 'Explain the failing test\nThen suggest a fix.\n' | nc localhost 1911
# If nc is unavailable:
printf 'Explain the failing test\n' | cai --feed-tcp 1911
```

One TCP connection carries one UTF-8 prompt. Normal EOF, including a sender's write-half close, submits it. Newlines remain part of the prompt. Empty connections do nothing. Invalid or incomplete UTF-8, NUL, oversized input and detected receive errors discard the entire receipt. Only one connection can be receiving or playing out a prompt at a time; additional connections are rejected.

When the ordinary draft is empty, incoming text appears there as it arrives. Enter cannot submit a partial receipt; Escape cancels it and clears its incoming draft. When a local draft or queue-item edit exists, that text is preserved and the complete incoming prompt enters the turn queue. The input uses ordinary backend admission and queue policies, never steering. A session switch or control takeover during receipt discards it. A parked controlling attachment receives through its own backend's queue; an observer cannot receive prompts.

Addresses accept `PORT`, `:PORT`, `HOST:PORT`, or `[IPv6]:PORT`. An omitted host binds only to loopback. Hostnames are resolved when opening the endpoint. Use `--proto tcpv6` for IPv6, with an IPv6-only listener. A wildcard bind requires an explicit address, for example `--prompt-listener 0.0.0.0:1911`. Set `--prompt-advertise reachable-host:1911` if the bind address is unsuitable for replies; wildcard addresses are never advertised as usable reply destinations automatically.

`--simulate-typing` selects 30 ms between Unicode characters. `--simulate-typing 5` selects 5 ms; zero disables it. Playback runs on readiness/deadline events with bounded pending bytes and TCP backpressure. It does not sleep in the UI event loop or wake an idle listener.

The executable-only `--feed-tcp ADDRESS [--proto tcpv4|tcpv6]` utility bypasses CAI configuration, authentication, storage, the banner and agent startup. It forwards arbitrary stdin bytes unchanged and emits ordinary JSON diagnostics. It does not validate Unicode or simulate typing. Success means local TCP transmission completed, not that a prompt was admitted or an agent replied. Detected stdin/send errors use an abortive close so the receiver can reject incomplete input.

## Publish and observe events

```sh
cai -x --event-multicast --ip-multicast-if 127.0.0.1
```

The bare multicast flag allocates the first available destination in `239.193.89.1` through `239.193.89.254`, port `49189`. An explicit destination, such as `--event-multicast 239.193.89.20:49189`, reserves that destination in the same store. The pool, port, interface, hop limit and loopback behavior are configurable. Allocation uses retained 24-hour collection leases, checks actual expiry and stops publication if ownership is lost. Leases renew every 12 hours, independently of event frequency. After a crash, automatic allocation skips the old claim; an explicit destination remains reserved until its lease expires. Public registrations have 24-hour retention and abandoned claim collections have 48-hour retention, renewed with ownership. Normal shutdown removes owned registrations and claims.

The backend logs its actual endpoint and a shell-quoted socat command. Every attaching frontend receives the same publisher metadata, including on reconnect. For a loopback IPv4 publisher, the command resembles:

```sh
socat -u 'UDP4-RECV:49189,bind=239.193.89.1,reuseaddr,ip-add-membership=239.193.89.1:127.0.0.1' STDOUT
```

Publication uses the same JSONL event representation as the session journal, after durable append. It streams through bounded 1,200-byte datagrams without materializing an event, rereading the journal or publishing internal commit records. Child output appears through the parent's tagged events; private child journals are not published a second time. It runs without attached frontends. UDP is deliberately best effort: missing, reordered or truncated datagrams can corrupt a line. There is no delivery acknowledgement, replay handshake or guaranteed loss detection. Receivers bind to the chosen group; Linux sockets also disable delivery from other joined groups sharing the port.

`event_stream_listen` is available to the agent by default, but opens no socket until called. It accepts `address`, `interface`, `duration_ms` (1–60,000; default 1,000) and `max_bytes` (1–1,048,576; default 32,768). Its structured result includes external data, byte/parse/truncation information and partial-line status. Reception supports cancellation and bounded memory even for event lines larger than its output limit.

## Discover endpoints and send from tools

`cai -l` lists sessions and registered endpoints in the current workspace; `cai -L` or `cai -l --all` includes other workspaces in the configured store. Metadata is stored in `${namespace_prefix}.network`, normally `cai.network`, through the backend's existing history client. An endpoint names a live attachment and server instance separately from the currently selected saved session. Stale records after a crash are discovery hints, not proof of liveness.

`--network-tools` enables three additional agent tools:

- `network_list` lists registered endpoints, with optional `workspace` filtering and a continuation `cursor`.
- `prompt_listener_info` returns the selected attachment's usable reply address and an example tool call. It requires explicit selection when no unambiguous controller exists.
- `prompt_send` accepts `address`, `prompt`, optional `attachment_id`, `timeout_ms` (1–60,000; default 10,000) and `include_reply` (default false).

`prompt_send` sends a correlated CBOR action to the selected controlling frontend. That frontend resolves, connects and transmits; the backend never opens an outbound prompt TCP connection. The UI remains responsive during transfer. A missing, unsupported, disconnected or revoked attachment fails explicitly. Deadlines and cancellation abort unfinished transfers, and receipts distinguish local transmission from remote admission and mark uncertain delivery. A cancelled request is never transferred to a new attachment or retried automatically. DNS runs off the UI thread; an operating-system resolver call already in progress returns on its resolver's own deadline, while backend request cancellation prevents subsequent transmission.

`include_reply` appends explicit reply instructions identifying the sending attachment's usable listener. No footer is added otherwise. Endpoints follow the attachment's current session, so a reply after a session switch reaches the newly selected session. There is no automatic reply loop or fan-out.

## Configuration

```ini
[ui]
prompt_listener =
prompt_advertise =
proto = tcpv4
simulate_typing_ms = 0

[server]
event_multicast =
event_multicast_base = 239.193.89.1
event_multicast_count = 254
event_multicast_port = 49189
ip_multicast_if =
ip_multicast_ttl = 1
disable_multicast_loop = false

[agent]
network_tools = false
disable_event_stream_listener = false
```

Each setting has an application-prefixed environment variable, such as `CAI_PROMPT_LISTENER`, `CAI_SIMULATE_TYPING_MS`, `CAI_EVENT_MULTICAST` and `CAI_NETWORK_TOOLS`. The corresponding flags use hyphens; the delay flag is `--simulate-typing [MS]`. Boolean settings have the ordinary positive/negative flag forms. `cai --dump-config` describes the complete settings. An empty listener or multicast destination disables it; use `auto` in configuration to request automatic group allocation. Headless backends ignore inherited UI listener settings; explicit listener flags require a Softline frontend.

## Embedding

Core libcai exposes reusable nonblocking socket operations in `cai/network.h`, strict incremental validation in `cai/utf8.h`, journal-event pull serialization, and agent tool registration/host callbacks in `cai/network_tools.h`. These have no storage, terminal or renderer dependency. Tools execute through the calling runtime's cancellation scope.

libcai_app supplies the Pouch registry, backend action mailbox and Softline adapters. Other frontend adapters can use `cai/app_network.h` to announce capabilities, receive publisher metadata and send correlated action receipts over the existing authenticated workspace connection. Strings are borrowed during each callback/write; the envelope preserves the original attachment, control generation and session epoch.

The `NETWORK` payload is a definite CBOR array: `[operation, timeout_ms, transferred, code, flags, address, advertise, interface_name, socat, prompt, reason]`. Operations are capability (0), publisher (1), send (2), receipt (3), cancel (4) and diagnostic notice (5). Flags are send support (1), input availability (2) and uncertain delivery (4). Prompt text is bounded to 66,048 bytes; other strings to 4,096 bytes each. Fields are emitted directly through the protocol writer and decoded incrementally. The envelope's `conversation` field means session epoch. Connection identity, attachment and authority checks apply independently of payload parsing. Frontend diagnostics use the backend's existing session log.

See the [implementation specification](network-prompt-events.md) for the complete ownership and verification contract.
