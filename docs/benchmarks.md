# CBOR, Unix transport and presentation benchmarks

`make benchmark` builds an opt-in, static x86_64 musl Release runner and measures the production codec, connected Unix sockets, typed transcript path, and native presentation path separately.
This follows the lifecycle's performance-contract requirement and the compiled C runner / Make / Bash convention used by libmdf.
It adds no benchmark framework or application dependency.
Linux, the normal CMake/Ninja/cpkt toolchain prerequisites, Bash, jq, ripgrep and GNU coreutils are required.
An [initial musl Release baseline](benchmark-baseline.md) records the first measured results and their limits.
A [component profile and CBOR optimization report](performance-cbor-presentation.md) separates the presentation stages and records subsequent codec improvements.

```sh
make benchmark
make benchmark BENCH_ARGS='--rounds 7 --warmups 2 --bytes 1048576 --iterations 2000'
make benchmark BENCH_ARGS='--no-build --message-bytes 4096 --json'
# Use an already configured checkout-owned build, including Debug for harness checks:
bash scripts/benchmark.sh --no-build --binary-dir build/debug --bytes 8192 --iterations 4 --verify
```

The default workload uses 64, 4096 and 32768 byte messages, five measured rounds after one discarded warmup per case, 8 MiB of logical payload per bulk round, and 2000 exchanges per latency round.
Bulk payload is rounded up to complete messages.
`--message-bytes N` selects one size; `--case NAME` selects one case.
The runner's `--list` prints the available cases.
`CAI_BUILD_BENCHMARKS` is off by default and can be enabled in an existing Linux static-library configuration.
The transcript/presentation cases are included when the application is enabled.

The Bash driver preserves every sample as JSONL in a new directory under `build/benchmarks/` and prints that path on stderr, including after failure.
Its default stdout is a Markdown table of medians; `--json` prints the complete JSONL instead.
Metadata records identify the executable SHA-256, source revision and dirty state at invocation, exact driver arguments, target, build type, compiler, CPU model, kernel and workload.
With `--no-build`, the caller must ensure that the existing executable corresponds to the source revision being compared; the invocation revision alone does not establish this.
Failed runs leave provisional partial reports and return failure without a summary.
The executable also returns failure if its report cannot be written.

## Measured boundaries

| Case | Work inside the interval |
|---|---|
| `cbor_encode_bytes`, `cbor_encode_text` | Production encoder into a counting sink, resetting between values. |
| `cbor_decode_bytes`, `cbor_decode_text` | Production decoder reading a bounded pre-encoded fixture, resetting between values. |
| `cbor_roundtrip_text`, `cbor_roundtrip_utf8` | Encoder feeding decoder directly through a sink callback; ASCII and multibyte UTF-8 payloads. |
| `framed_memory` | Production length-prefixed wire encoder feeding the production wire decoder in memory. |
| `unix_raw` | Pre-encoded framed bytes through a nonblocking Unix stream socket to a second process, without CBOR parsing. |
| `wire_peer_bytes`, `wire_peer_text` | Production wire peers encoding, queueing, sending, receiving and decoding through the same socket topology. |
| `unix_rtt`, `wire_rtt` | One request outstanding at a time; the receiver sends the same five-byte framed NULL acknowledgement in both cases. |
| `transcript_peer` | Production protocol and transcript codecs carrying assistant delta/end events over wire peers. |
| `transcript_render` | The same transcript workload plus the actual native presenter, libmdf and Softline, writing to `/dev/null`. |
| `transcript_render_mdf` | The same presenter and libmdf parser/renderer, discarding decided body emissions in a counting sink before CAI's output sink. |
| `transcript_render_cai` | The same presenter/libmdf and real CAI output sink, replacing Softline's write method with a benchmark-only null method. |

Fixture creation, object construction, socket creation and receiver fork happen before measurement.
Receiver teardown and process reaping happen afterward.
Timed socket rounds include a start signal and the final small measurement report over a separate control socket.
CPU time excludes setup and teardown; parent wall time includes receiver completion.
Only fixed-size measurement/control records travel on the control socket.

Payload moves directly from producer to encoder, socket, decoder and consumer in bounded chunks.
The explicitly pre-encoded fixture used by decode and raw-socket probes is at most 32 KiB plus headers and is prepared outside the interval.
No complete transcript or bulk stream is retained, concatenated or spooled.
Each case repeats the same bounded fixture, providing a cache-hot comparison rather than a cold-data benchmark.
The latency probe retains only its numeric samples, with a maximum of one million exchanges.

Raw and wire-peer comparisons use the same framed bytes, one completed message per producer flush, sends of at most 4 KiB and reads of at most 2 KiB.
The raw bulk receiver reads a continuous coalesced byte stream rather than artificially issuing a separate read for every small message.
Peer reads use their normal bounded work budget and queue allocation.
Typed transcripts carry the real protocol metadata, delta and end records, with body chunks of at most 4096 bytes.
Each logical message is one completed assistant document.

Presentation uses an 80-column, plain, UTC frontend and Softline's output stream on a nonterminal sink.
It includes Markdown parsing, wrapping, margins, timestamps and actual output writes.
It excludes physical terminal rendering, draft editing/redraw, model inference, authentication, workspace discovery, handshake, Pouch reads, and context restoration.
It is therefore neither a full UI responsiveness benchmark nor a session-startup benchmark.
The two diagnostic render probes leave body parsing, wrapping and Markdown emissions unchanged, but deliberately cut the output path at different boundaries.
`transcript_render_mdf` still emits CAI's per-document timestamp headers normally; `transcript_render_cai` discards those at the null Softline method too.
Both probes count the same renderer body emissions as the full path, with no body buffering or added delivery delay.
They are diagnostic measurements, not alternative application implementations or performance optimizations.

## Metrics and interpretation

Each raw sample includes wall seconds, sender and receiver user/system CPU seconds, payload and encoded wire bytes, logical records, MiB/s, records/s, aggregate CPU percentage, RTT p50/p95, send/receive/poll counts, and renderer output bytes/write counts.
`mdf_calls` and `mdf_bytes` count decided Markdown body sink emissions separately from `output_calls` and `output_bytes`, which count OS output writes.
Socket counters include both endpoints and unsuccessful nonblocking attempts, including EAGAIN; control-socket reads/writes are excluded.
CPU percentage is summed process CPU time divided by wall time and can exceed 100% for the two-process cases.
Output counters measure writes reaching Softline's sink, not Markdown source bytes.
RTT percentiles use nearest rank over actual individual exchanges, not batch averages; they are zero for throughput-only cases.
The table reports medians of each measured round's metrics, including its RTT percentiles.

Use matching Release target, hardware, arguments, verification setting and load when comparing revisions, and keep the raw samples to inspect spread.
Very short in-memory rounds need a larger `--bytes` value for stable clock/CPU resolution.
Unverified byte-string codec probes do not inspect or copy payload bytes: their very high logical MiB/s measures metadata and callback overhead, not memory bandwidth.
Text codecs additionally validate UTF-8; the text peer probes include that work at both endpoints.
Socket scheduling and backpressure affect the results, so subtracting independently measured medians does not isolate pure codec cost.
Compare encode/decode measurements with raw and peer transport, then compare typed transport with presentation to locate the expensive stage.
The harness does not change CPU affinity, frequency governor or system policy.

`--verify` additionally compares encoded/decoded/socket/transcript payload bytes against the known fixture.
It adds consumer work and is intended for correctness checks or explicitly matched comparisons.
The optional offline CTest contracts exercise every compiled case, repeated rounds, warmups, boundary sizes, UTF-8, metric/report structure, invalid options and unwritable output.
Application builds also use the existing terminal fixture to verify that presentation output stays identical under narrow and wide caller terminals.
Render probes must agree on Markdown body counts; the null Softline probe must perform no measured OS output writes.
They deliberately impose no performance acceptance threshold.
