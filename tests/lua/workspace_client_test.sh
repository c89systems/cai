#!/usr/bin/env bash
source "$(dirname "$0")/../cli_test_common.sh"
lua=$(realpath "$1")
script=$(realpath "$2")
peer_start
runtime_root=$(mktemp -d "$support/../build/luaws.XXXXXX")
owner_pid=
client_cleanup() {
  local result=$?
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; else tail -30 "$root/owner.log" >&2; fi
  cleanup "$result"
}
trap client_cleanup EXIT
export LOG_LEVEL=info
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"printf '\''%16384s'\'' LUA_EXEC_RESULT","tty":false,"max_output_tokens":8192}},"LUA_SOCKET_ANSWER"]}'
"${base[@]}" -x -N --terminal --yolo --runtime-dir "$runtime_root" >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
# The client must not load config/auth or create a separate private store.
HOME="$root/uncreated-client-home" "$lua" "$script" "$runtime_root" "$work"
[[ ! -e "$root/uncreated-client-home" ]]
[[ $(requests) == 2 ]]
wait "$owner_pid"
owner_pid=
