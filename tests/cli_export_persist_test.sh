#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/ej.XXXXXX")
server_pid= batch_pid=
finish() {
  local status=$?
  if [[ -n "$batch_pid" ]]; then
    kill -TERM "$batch_pid" 2>/dev/null || true
    wait "$batch_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_pid" ]]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ "$status" != 0 ]]; then tail -50 "$root/server.log" >&2 || true
  else rm -rf "$runtime_root"; fi
  cleanup "$status"
}
trap finish EXIT
mkdir "$root/gate"
mkfifo "$root/gate/release"
export CAI_TEST_EXPORT_GATE="$root/gate" CAI_TEST_EXPORT_GATE_PASS=3 LOG_LEVEL=info
base+=(--runtime-dir "$runtime_root")
"${base[@]}" -x -N </dev/null >"$root/server.out" 2>"$root/server.log" & server_pid=$!
wait_for has "$root/server.log" 'Workspace server ready'
plan '{"plan":["PersistSeedReply"]}'
terminal_start
prompt PersistSeedPrompt PersistSeedReply
send '/export persisted\r'
wait_for test -f "$root/gate/entered"
terminal_close
# Detach must not cancel a persistent owner's in-flight file export.
timeout 2 bash -c 'printf r >"$1"' _ "$root/gate/release"
wait_for test -s "$work/persisted.md"
wait_for test -s "$work/persisted.jsonl"
contains "$work/persisted.md" PersistSeedReply
kill -0 "$server_pid"
terminal_start
contains "$term/text" 'Session export'
[[ $(requests) == 1 ]]
terminal_close
# Batch admission must remain attached until the held export actually finishes.
rm "$root/gate/entered"
"${base[@]}" -ni '/export batch-persist.md' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for test -f "$root/gate/entered"
kill -0 "$batch_pid"
[[ ! -e "$work/batch-persist.md" ]]
absent "$root/batch.out" 'Session export'
timeout 2 bash -c 'printf r >"$1"' _ "$root/gate/release"
wait "$batch_pid"
batch_pid=
contains "$root/batch.out" 'Session export'
contains "$work/batch-persist.md" PersistSeedReply
[[ $(requests) == 1 ]]
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
[[ ! -s "$root/server.out" ]]
[[ -z $(find "$work" -name '.export-*' -print) ]]
