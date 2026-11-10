# Initial transport and presentation baseline

Measured on 6 November 2026 with the [benchmark workload contract](benchmarks.md).
These are local development measurements, not release guarantees or CI timing thresholds.

```sh
make benchmark
```

The run used the default 8 MiB per bulk round, 2000 individual RTT exchanges, one warmup and five measured rounds, at 64, 4096 and 32768 byte message sizes.
Byte comparisons were disabled during measurement; separate contract tests exercised integrity verification.
No other build or test launched by this task ran during measurement.
Other workloads on the shared host were not controlled.

| Environment | Value |
|---|---|
| CPU | AMD EPYC-Rome-v5 Processor |
| Kernel | Linux 7.0.0-31-generic, x86_64 |
| Target | x86_64-linux-musl, static Release |
| Compiler | GNU 15.3.0 |
| Source | Parent `724937035166f64c2f4cde757f600618858011d0` plus this benchmark change; dirty state recorded |
| Executable SHA-256 | `1ab2be2777e64dae8a98fdd04290ff01eec08d4c30451962c70eb4e1390eb09a` |
| Raw local artifact | `build/benchmarks/run.SdFH6M/results.jsonl` |

The local artifact contains all 210 measured samples and three workload metadata records, plus the driver invocation record.
It remains generated state and will be removed by `make clean`.

## Representative 4096-byte results

Throughput and CPU are medians across the five measured rounds.
Wall ranges are the minimum and maximum measured bulk-round times.
CPU is the combined sender and receiver CPU divided by wall time; two-process cases can exceed 100%.

| Case | MiB/s | Wall range, ms | CPU % |
|---|---:|---:|---:|
| CBOR text encode | 471.1 | 16.34–20.56 | 100.0 |
| CBOR text decode | 490.5 | 15.77–16.96 | 100.0 |
| CBOR text round trip in memory | 219.1 | 33.87–41.61 | 100.0 |
| Framed text round trip in memory | 216.9 | 33.28–47.86 | 100.0 |
| Raw Unix stream | 816.2 | 8.50–11.19 | 184.6 |
| Wire peers, byte strings | 868.9 | 8.80–9.79 | 189.8 |
| Wire peers, text strings | 286.7 | 24.99–32.66 | 191.4 |
| Typed transcript peers | 368.1 | 20.57–23.89 | 183.4 |
| Typed transcript with native presentation | 2.73 | 2855.39–2978.95 | 100.6 |

| 4096-byte request / five-byte acknowledgement | Median round p50, µs | Median round p95, µs |
|---|---:|---:|
| Raw Unix stream | 36.05 | 63.17 |
| Text wire peers | 55.49 | 88.15 |

## Interpretation

For this workload, presentation dominates the measured end-to-end path.
At 4096 bytes it takes roughly three seconds per 8 MiB, compared with approximately 22 milliseconds for typed transcript transport alone.
The renderer round issues 2,299,905 output writes for 7,907,329 output bytes, making presentation write granularity a concrete profiling candidate.
This measurement includes real libmdf/Softline output work but excludes a physical terminal, editing/redraw, model inference and session storage.
It does not establish the cause of any particular live UI stall.

CBOR text validation has measurable cost: byte-string peers approach the raw transport baseline at this size, while text peers validate the payload at both endpoints.
The typed transcript transports its body as byte spans and therefore is not equivalent to the minimal text-frame workload.
Differences between these probes must not be attributed entirely to their metadata framing.

Small messages amplify per-document and metadata work: the 64-byte typed transcript case measures 8.93 MiB/s, versus 368.06 MiB/s at 4096 bytes and 327.14 MiB/s at 32768 bytes.
Each logical message completes an assistant document, so the small-message presentation case also repeats document finalization and timestamp overhead.
This is a declared workload characteristic, not a model of every token feed in a real session.

The unverified byte-string codec-only probes do not traverse payload memory and must not be interpreted as memory-bandwidth measurements.
The observed spread, particularly for the fast transport rounds, also argues against fixed timing thresholds on this shared host.
