#!/usr/bin/env bash
source "$(dirname "$0")/../cli_test_common.sh"
lua=$cli
script=$(realpath "$1")
peer_start
runtime_root=$(mktemp -d "$support/../build/own.XXXXXX")
runtime_root=$(realpath "$runtime_root")
owner_cleanup() {
  local result=$?
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; else
    tail -35 "$root/lua.err" "$root/empty.err" 2>/dev/null || true
  fi
  cleanup "$result"
}
trap owner_cleanup EXIT
mkdir "$root/second workspace" "$root/third"
printf 'HOST_CWD_MARKER\n' >"$root/host-marker"
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"printf %s \"$CAI_OWNER_VALUE\"","tty":false}},"FIRST OWNER DONE","SECOND OWNER DONE",{"pause":true,"pause_text":"GC STREAM HELD","text":"GC COMPLETE"}]}'
cd "$root"
"$lua" "$script" "$endpoint" "$root" "$runtime_root" >"$root/lua.out" 2>"$root/lua.err"
contains "$root/lua.out" 'LUA OWNERS PASSED'
[[ $(requests) == 4 ]]
absent "$root/peer/requests.jsonl" 'MUTATED AFTER START'
contains "$root/peer/requests.jsonl" 'left=right'
contains "$root/peer/requests.jsonl" 'second'
CAI_REPLAY_TURNS=invalid-must-not-inherit "$lua" "$script" "$endpoint" "$root" "$runtime_root" empty >"$root/empty.out" 2>"$root/empty.err"
contains "$root/empty.out" 'EMPTY ENVIRONMENT PASSED'
[[ $(requests) == 4 ]]
[[ -z $(find "$runtime_root" -name 'ws-*.info' -o -name 'ws-*.sock') ]]
