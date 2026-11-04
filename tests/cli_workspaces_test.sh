#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
client=$(realpath "$1")
terminal_host=$(realpath "$2")
runtime_root=$(mktemp -d "$support/../build/wsa.XXXXXX")
base+=(--runtime-dir "$runtime_root")
owner_base=("${base[@]}")
owner_a= owner_b= holder_pid=
workspaces_cleanup() {
  local result=$? pid
  for pid in "$holder_pid" "$owner_a" "$owner_b"; do
    if [[ -n "$pid" ]]; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap workspaces_cleanup EXIT
export LOG_LEVEL=info
mkdir "$root/other"
plan '{"plan":["FIRST workspace answer","SECOND workspace answer","FIRST continued answer"]}'
"${base[@]}" -x -N --terminal --no-persist --workspace-name workspace-a </dev/null >"$root/a.out" 2>"$root/a.log" & owner_a=$!
wait_for has "$root/a.log" 'Workspace server ready'
"${base[@]}" -C "$root/other" -x -N --workspace-name workspace-b </dev/null >"$root/b.out" 2>"$root/b.log" & owner_b=$!
wait_for has "$root/b.log" 'Workspace server ready'
"$client" "$runtime_root" "$root/other" hold >"$root/holder" 2>"$root/holder.err" & holder_pid=$!
wait_for test -s "$root/holder"
attach_args=(-s workspace-a)
if [[ ${3:-native} == embed ]]; then
  base=("$terminal_host" "$runtime_root" "$work" "${base[@]:1}")
  attach_args=()
fi
terminal_start "${attach_args[@]}"
prompt 'hello in the first workspace' 'FIRST workspace answer'
send '/help\r'; expect 'Workspace connections'; expect 'Queue and approval keys'; expect Alt-Enter
send '/workspace\r'; expect 'Current workspace'; expect workspace-b
send '/workspace no-such-workspace\r'; expect 'No running workspace'
send '/status\r'; expect 'Session ID'
send '/workspace workspace-b\rDRAFT_A'; expect 'Observing workspace'
kill -0 "$owner_a"
send '\025/takeover\r'
wait "$holder_pid"; holder_pid=
prompt 'hello in the second workspace' 'SECOND workspace answer'
send '/workspace workspace-a\rDRAFT_B'; expect_prompt DRAFT_A
send '\025'
prompt 'continue the first workspace' 'FIRST continued answer'
send '/workspace workspace-b\r'; expect_prompt DRAFT_B
send '\025/workspace ../work\r'; expect 'Conversation attached'
send '/workspace workspace-a\r'; expect 'Already viewing this workspace'
plan '{"plan":[{"pause":true,"pause_text":"OLD workspace still working. ","text":"OLD workspace done."},"QUEUED A answer"]}'
send 'work in the first workspace while I am away\r'; expect 'OLD workspace still working.'
send 'queued only in workspace A\r'; expect 'Q 1.'
send '/workspace workspace-b\r'; expect 'Conversation attached'
release
wait_for has "$root/a.log" 'QUEUED A answer'
if has_since "$send_offset" 'OLD workspace done.' "$send_feed_offset"; then
  fail 'Parked workspace rendered into the selected workspace'
fi
send '/workspace workspace-a\r'; expect 'OLD workspace done.'; expect 'QUEUED A answer'
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"read line; printf TOOL_AFTER_CURSOR","tty":true,"yield_time_ms":1}},{"pause":true,"pause_text":"Tool yielded before switch. ","tool":"write_stdin","arguments":{"session_id":"terminal-1","chars":"ok\n","yield_time_ms":1000}},"TOOL GAP FINISHED"]}'
send 'finish a yielded command while I view the other workspace\r'; expect 'Tool yielded before switch.'
send '/workspace workspace-b\r'; expect 'Conversation attached'
release
wait_for has "$root/a.log" 'TOOL GAP FINISHED'
send '/workspace workspace-a\r'; expect TOOL_AFTER_CURSOR
expect 'Ran read line; printf TOOL_AFTER_CURSOR'
expect 'TOOL GAP FINISHED'
terminal_close
wait "$owner_a"; owner_a=
kill -0 "$owner_b"
[[ $(requests) == 8 ]]
jq -se '
  (.[0].input | tostring | contains("hello in the first workspace")) and
  (.[1].input | tostring | contains("hello in the second workspace")) and
  (.[1].input | tostring | contains("hello in the first workspace") | not) and
  (.[2].input | tostring | contains("continue the first workspace")) and
  (.[2].input | tostring | contains("hello in the second workspace") | not)
' "$root/peer/requests.jsonl" >/dev/null
# Distinguish an acknowledged shutdown from an unexpected transport loss.
# Neither path may silently claim that a crashed attachment exited normally.
base=("${owner_base[@]}" -C "$root/other")
if [[ ${3:-native} == embed ]]; then
  base=("$terminal_host" "$runtime_root" "$root/other" "${base[@]:1}")
  terminal_start
  terminal_close /shutdown
  wait "$owner_b"; owner_b=
else
  terminal_start
  kill -KILL "$owner_b"
  wait "$owner_b" 2>/dev/null || :
  owner_b=
  expect 'workspace server disconnected'
  wait_for test -s "$term/status"
  wait "$terminal_pid" 2>/dev/null || :
  terminal_pid=
  [[ $(cat "$term/status") == 1 ]]
fi
