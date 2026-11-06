# Presentation attribution and CBOR text optimization

Measured on 7 November 2026, following the [benchmark contract](benchmarks.md), on the same AMD EPYC-Rome-v5 / Linux 7.0.0-31-generic host as the initial baseline.
Executables use GNU 15.3.0, static x86_64 musl Release builds.
Sources are parent `3cd3d07` plus this change; raw reports record executable SHA-256 and dirty source state.
No other build or test launched by this task ran during measurements; other workloads on the shared host were not controlled.

## Which presentation component costs the most?

These probes use identical 4096-byte messages, 8 MiB per round, seven measured rounds and two discarded warmups.
Each body follows the same protocol, presenter, libmdf parsing and wrapping path; the diagnostic sinks discard output only after a declared boundary.
The three rendering probes produce exactly 2,291,712 decided body emissions totaling 7,884,800 bytes per round.

| Probe | Median wall, ms | Receiver user CPU, ms | Receiver system CPU, ms |
|---|---:|---:|---:|
| Typed transcript transport only | 21.37 | 11.24 | 9.17 |
| Presenter/libmdf with counting body sink | 1392.30 | 1370.39 | 16.74 |
| Plus real CAI output sink, null Softline method | 1810.59 | 1590.49 | 217.02 |
| Full CAI/libmdf/Softline output path | 2889.56 | 2126.78 | 758.13 |

Libmdf is the largest individual stage in this workload: approximately 1.37 seconds beyond transport, or 47% of the full round.
CAI's downstream sink adds approximately 0.42 seconds (14%), and Softline plus its output syscalls adds approximately 1.08 seconds (37%).
These are approximate differences between independently measured medians, not exact additive cycle accounting.
The remaining transport cost is below 1%.
This attributes the current workload; it does not diagnose terminal-emulator latency or real-session editing/redraw behavior.

Callgrind on a 1 MiB, single-round reproduction records 1,425,325,036 receiver instructions.
The libmdf feed path accounts for 99.13% inclusively, but that includes its output callbacks; subtracting the renderer sink's 16.27% leaves approximately 83% in parsing/rendering and their other helpers.
The Softline write path accounts for 13.24% inclusively.
These percentages describe executed user-space instructions, not native CPU time or kernel cost.

| Largest relevant self-instruction samples | Share of receiver instructions |
|---|---:|
| `ansi_process_text` | 8.89% |
| `ansi_source_has_url_scheme` | 8.05% |
| `mdf_renderer_write_token_internal` | 6.28% |
| `ansi_write_token` | 5.89% |
| `mdf_word_append` | 4.85% |

A separate syscall-count trace on the same 1 MiB reproduction records 575,241 terminal-query `ioctl` calls and 287,492 writes across both processes, including setup/report writes.
The timed output counter records 287,489 actual output writes.
CAI probes output geometry for each sink fragment; Softline also checks terminal state on each write.
The queries fail with ENOTTY on the deliberately nonterminal sink, so they are overhead even without a real terminal.
Tracing changes execution time drastically; its time percentages, including parent waits, must not be used as native component timings.

The concrete presentation opportunities are libmdf's text/URL/word processing and the downstream cost amplified by millions of tiny emissions.
This change instruments those boundaries; it does not change production presentation or modify either dependency.

## CBOR change and measured effect

The initial ASCII text round-trip profile records 40,292,868 instructions per 1 MiB.
`data`, containing the inlined UTF-8 validator, accounts for 99.15% of the instructions.
The optimized validator skips ASCII runs using bounded eight-byte loads and keeps scalar continuation state in local variables until each fragment is accepted.
Loads use `memcpy`, accept unaligned spans and do not read beyond the caller's fragment; the high-bit mask is independent of host byte order.
Multibyte validation, provisional delivery, sticky failures, bounded callbacks and immediate streaming remain in force.
There is no new dependency, allocation, materialized message or wire/API change.

The corresponding profile after the change records 2,482,777 instructions, approximately 94% fewer.
Native throughput measurements below use 64 MiB per round, 4096-byte messages, seven measured rounds after two warmups, without byte-comparison verification.
Before and after use the same workload and native build configuration.

| Case | Before, MiB/s | After, MiB/s | Ratio | CPU per round before / after, ms |
|---|---:|---:|---:|---:|
| ASCII text encode | 455.7 | 7360.2 | 16.2× | 140.45 / 8.70 |
| ASCII text decode | 434.3 | 10215.8 | 23.5× | 147.35 / 6.27 |
| ASCII text round trip | 222.9 | 5084.2 | 22.8× | 287.12 / 12.59 |
| Repeated multibyte UTF-8 round trip | 301.6 | 341.3 | 1.13× | 212.21 / 187.50 |
| Framed text in memory | 222.0 | 4450.6 | 20.0× | 288.14 / 14.38 |
| Text wire peers | 264.1 | 879.5 | 3.33× | 465.33 / 142.18 |

CPU values sum both endpoints where applicable.
For ASCII round trips the measured wall-time ranges are 277.96–306.23 ms before and 12.42–13.35 ms after.
For text wire peers the ranges are 209.97–302.32 ms before and 68.80–83.26 ms after.
Unverified byte-string controls show no regression; their borrowed-span throughput does not measure payload memory bandwidth.
All figures are local observations, not acceptance thresholds.

## Reproduction and correctness

```sh
for probe in transcript_peer transcript_render_mdf transcript_render_cai transcript_render; do
  make benchmark BENCH_ARGS="--no-build --case $probe --message-bytes 4096 --bytes 8388608 --rounds 7 --warmups 2 --json"
done
make benchmark BENCH_ARGS='--no-build --case cbor_roundtrip_text --message-bytes 4096 --bytes 67108864 --rounds 7 --warmups 2 --json'
strace -f -c -o build/presentation.syscalls build/x86_64-linux-musl-release/cai_benchmark --case transcript_render --message-bytes 4096 --bytes 1048576 --rounds 1 --warmups 0
valgrind --tool=callgrind --callgrind-out-file=build/presentation.callgrind.%p build/x86_64-linux-musl-release/cai_benchmark --case transcript_render --message-bytes 4096 --bytes 1048576 --rounds 1 --warmups 0
```

Raw measurements and profiles are retained locally under `build/perf-presentation/` and `build/perf-cbor/`, with per-case JSONL before/after files and the original driver artifact paths in stderr logs.
They are generated state and are removed by `make clean`.

Correctness checks independently feed encoder and decoder, compare acceptance with a separate UTF-8 byte-range grammar, cover all 65,536 byte pairs both whole and split, and mutate boundary vectors across all byte values.
They also cover aligned and unaligned ASCII spans, embedded NUL, split scalars, overlong forms, surrogates, out-of-range values, truncated text and sticky failures.
Protected pages test that neither codec reads past the supplied span.
The checks run on a worker with the platform's default stack, including musl, and retain the existing cancellation/bounded-delivery regressions.
