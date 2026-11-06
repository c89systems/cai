#!/usr/bin/env bash
set -euo pipefail
binary=$1
root=$(mktemp -d "$2/benchmark-check.XXXXXX")
trap 'rm -rf "$root"' EXIT
"$binary" --list >"$root/cases"
"$binary" --rounds 2 --warmups 1 --bytes 8193 --message-bytes 4096 --iterations 4 --verify >"$root/samples.jsonl"
jq -se --argjson cases "$(wc -l <"$root/cases")" '
  .[0].kind == "metadata" and .[0].verify and
  ([.[] | select(.kind == "sample")] | length) == $cases * 2 and
  all(.[] | select(.kind == "sample");
    .wall_s > 0 and .payload_bytes > 0 and .records > 0 and
    .sender_user_s >= 0 and .sender_system_s >= 0 and
    .receiver_user_s >= 0 and .receiver_system_s >= 0 and
    .send_calls >= 0 and .recv_calls >= 0 and .poll_calls >= 0 and
    .mib_s > 0 and .cpu_pct >= 0 and .rtt_p95_us >= .rtt_p50_us) and
  all(.[] | select(.case == "unix_raw" or .case == "wire_peer_text");
    .records == 3 and .payload_bytes == 12288 and .wire_bytes == 12309) and
  all(.[] | select(.case == "wire_rtt" or .case == "unix_rtt");
    .records == 4 and .rtt_p50_us > 0) and
  all(.[] | select(.case == "transcript_render"); .output_calls > 0 and .output_bytes > 0)
' "$root/samples.jsonl" >/dev/null
for size in 1 64 32768; do
  "$binary" --rounds 1 --warmups 0 --bytes "$size" --message-bytes "$size" --iterations 1 --verify >"$root/boundary.jsonl"
done
if [[ -n ${CAI_BENCH_TERMINAL_PEER:-} ]] && rg -Fxq transcript_render "$root/cases"; then
  for width in 20 160; do
    mkdir "$root/tty-$width"
    "$CAI_BENCH_TERMINAL_PEER" "$root/tty-$width" "$width" "$binary" \
      --case transcript_render --rounds 1 --warmups 0 --bytes 4096 --message-bytes 4096 --verify </dev/null
    jq -se '[.[]|select(.kind=="sample")]|length==1 and .[0].output_calls>0' "$root/tty-$width/text" >/dev/null
    jq -s '[.[]|select(.kind=="sample")|[.output_calls,.output_bytes]]' "$root/tty-$width/text" >"$root/tty-$width/output"
  done
  cmp "$root/tty-20/output" "$root/tty-160/output"
fi
for args in '--rounds 0' '--warmups -1' '--bytes nope' '--bytes 184467440737095516160' '--message-bytes 32769' '--iterations 0' '--case missing' '--unknown 2' '--rounds'; do
  read -r -a arguments <<<"$args"
  if "$binary" "${arguments[@]}" >"$root/invalid" 2>"$root/error"; then
    printf 'Accepted invalid benchmark invocation: %s\n' "$args" >&2
    exit 1
  fi
done
if "$binary" --case cbor_decode_text --rounds 1 --warmups 0 --bytes 4096 >/dev/full 2>"$root/full"; then
  printf 'Lost report output was accepted\n' >&2
  exit 1
fi
