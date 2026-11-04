#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
client=$(realpath "$1")
terminal_host=$(realpath "$2")
runtime_root=$(mktemp -d "$support/../build/att.XXXXXX")
base+=(--runtime-dir "$runtime_root")
server_pid= holder_pid=
attach_cleanup() {
  local result=$?
  if [[ -n "$holder_pid" ]]; then
    kill -TERM "$holder_pid" 2>/dev/null || true
    wait "$holder_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_pid" ]]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap attach_cleanup EXIT
export LOG_LEVEL=info
plan '{"plan":["ATTACHED answer"]}'
"${base[@]}" -x -N --workspace-name attach-fixture </dev/null >"$root/server.out" 2>"$root/server.log" & server_pid=$!
wait_for has "$root/server.log" 'Workspace server ready'
"$client" "$runtime_root" "$work" hold >"$root/holder" 2>"$root/holder.err" & holder_pid=$!
wait_for test -s "$root/holder"
terminal_start -s attach-fixture
expect 'Observing workspace'
send 'this observer input must never run\r'
expect rejected
[[ $(requests) == 0 ]]
send '/takeover\r'
wait "$holder_pid"
holder_pid=
prompt 'hello through the socket' 'ATTACHED answer'
send '/status\r'; expect 'Session ID'
original=$(head -n1 "$root/holder")
send '/new\r'; expect 'New session'
send "/resume $original\r"; expect 'ATTACHED answer'
send '/resume missing-session\r'; expect 'was not found'
send '/status\r'; expect 'Session ID'
send '/theme\r'; expect 'Softline themes and pairings'
terminal_close
[[ $(requests) == 1 ]]
kill -0 "$server_pid"
plan '{"plan":["STARTUP answer"]}'
saved_base=("${base[@]}")
base=("$terminal_host" "$runtime_root" "$work" "${base[@]:1}")
terminal_start -i 'startup input for an existing owner'
expect ATTACHED
expect 'STARTUP answer'
plan '{"plan":[{"pause":true,"pause_text":"Waiting for cancellation. "}]}'
send 'wait until I cancel\r'; expect 'Waiting for cancellation.'
send '\033'; expect 'Turn cancelled.'
plan '{"plan":[{"pause":true,"pause_text":"Before steer. ","tool":"read_file","arguments":{"path":"steer.txt"}},"AFTER STEER"]}'
printf 'fixture content\n' >"$work/steer.txt"
send 'verify steering\r'; expect 'Before steer.'
send 'apply this steer at the request boundary\033\r'; expect 'S 1.'
release
expect 'AFTER STEER'
jq -se 'any(.[]; (.input | tostring | contains("apply this steer at the request boundary")))' "$root/peer/requests.jsonl" >/dev/null
plan '{"plan":[{"pause":true,"pause_text":"Draft switch boundary. ","text":"Draft turn finished."}]}'
send 'prepare a conversation switch\r'; expect 'Draft switch boundary.'
send '/new\rRETAINED_DRAFT'; expect_prompt RETAINED_DRAFT
release
expect 'New session'
send "/resume $original\r"; expect_prompt RETAINED_DRAFT
send '\025'
terminal_close
base=("${saved_base[@]}")
kill -0 "$server_pid"
kill -TERM "$server_pid"
wait "$server_pid"
server_pid=
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"printf COMMAND_BODY","tty":false}},"AUTOSTART answer"]}'
base+=(--terminal)
terminal_start
prompt 'start the owner automatically' 'AUTOSTART answer'
expect COMMAND_BODY
expect 'Ran printf COMMAND_BODY'
publication=$(find "$runtime_root" -name 'ws-*.info' -print -quit)
[[ -n "$publication" ]]
terminal_close
wait_for test ! -e "$publication"
[[ $(requests) == 8 ]]
