#!/usr/bin/env bash
set -euo pipefail
host=$(realpath "$1")
root=$(mktemp -d "$(dirname "$host")/core-stack.XXXXXX")
trap 'rm -rf "$root"' EXIT
mkdir "$root/tree"
deep="$root/tree"
for ((level=0; level<180; level++)); do
  deep="$deep/d"
  mkdir "$deep"
done
printf 'deep leaf\n' >"$deep/leaf"
ln -s "$root/tree" "$deep/cycle"
"$host" list "$root/tree" unused >"$root/list.json"
jq -e '.entry_count == 182 and (.entries | length == 182) and (.truncated | not)' "$root/list.json" >/dev/null
jq -e '[.entries[] | select(.name == "leaf")] | length == 1' "$root/list.json" >/dev/null
cat >"$root/sandbox" <<'SH'
#!/bin/sh
while [ "$#" -gt 0 ]; do
  if [ "$1" = /bin/sh ]; then shift; exec /bin/sh "$@"; fi
  shift
done
exit 55
SH
chmod 700 "$root/sandbox"
for mode in sandbox plain; do
  "$host" "$mode" "$root" "$root/sandbox" >"$root/exec.json"
  jq -e '.exit_code == 0 and .output == "WORKER_EXEC_OK"' "$root/exec.json" >/dev/null
done
"$host" schema "$root" unused
