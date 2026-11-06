#!/usr/bin/env bash
set -euo pipefail
script=$1
build=$2
root=$(mktemp -d "$build/benchmark-driver-check.XXXXXX")
trap 'rm -rf "$root"' EXIT
options=(--no-build --binary-dir "$build" --rounds 2 --warmups 0 --bytes 129 --iterations 2 --case wire_peer_text)
bash "$script" "${options[@]}" --json >"$root/report.jsonl" 2>"$root/error"
jq -se '
  .[0].kind == "run" and (.[0].revision|length) == 40 and
  (.[0].binary_sha256|test("^[a-f0-9]{64}$")) and
  ([.[]|select(.kind == "metadata")]|length) == 3 and
  ([.[]|select(.kind == "sample")]|length) == 6 and
  ([.[]|select(.kind == "sample")|.message_bytes]|unique) == [64,4096,32768]
' "$root/report.jsonl" >/dev/null
bash "$script" "${options[@]}" --message-bytes 64 >"$root/table" 2>"$root/error"
rg -Fq '| wire_peer_text | 64 |' "$root/table"
for args in '--unknown' '--rounds' '--binary-dir /tmp' '--no-build --binary-dir /tmp'; do
  read -r -a arguments <<<"$args"
  if bash "$script" "${arguments[@]}" >"$root/bad" 2>"$root/error"; then
    printf 'Accepted invalid benchmark driver invocation: %s\n' "$args" >&2
    exit 1
  fi
done
