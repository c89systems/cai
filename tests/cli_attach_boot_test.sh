#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
boot_fixture=$(realpath "$1")
peer_start
runtime_root=$(mktemp -d "$support/../build/boot.XXXXXX")
stall_pid= extra_terminal_pid=
boot_cleanup() {
  local result=$?
  if [[ -n "$stall_pid" ]]; then
    kill -TERM "$stall_pid" 2>/dev/null || true
    wait "$stall_pid" 2>/dev/null || true
  fi
  if [[ -n "$extra_terminal_pid" ]]; then
    kill -TERM "$extra_terminal_pid" 2>/dev/null || true
    wait "$extra_terminal_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap boot_cleanup EXIT
mkdir "$root/other"
"$boot_fixture" "$runtime_root" "$work" "$root/other"

# An endpoint that accepts connections but never answers its identity probe
# must not prevent construction, draft editing, or quitting in the terminal.
"$boot_fixture" --stall "$runtime_root" "$work" >"$root/stall.out" & stall_pid=$!
wait_for has "$root/stall.out" STALLED
base+=(--runtime-dir "$runtime_root")
terminal_launch
wait_for test -f "$term/text"
wait_for test -f "$term/feed"
send 'EDIT_WHILE_DISCOVERING'
editing_deadline=$((SECONDS+3))
while ! has "$term/prompt" EDIT_WHILE_DISCOVERING; do
  ((SECONDS < editing_deadline)) || fail 'Identity lookup blocked the editor'
  IFS= read -r -N 1 -t .05 -u "$terminal_event_fd" progress_byte || :
done
terminal_close
[[ $(requests) == 0 ]]
kill -0 "$stall_pid"
kill -TERM "$stall_pid"
wait "$stall_pid" 2>/dev/null || true
stall_pid=

# Hold both initial probes before releasing the endpoint together. The two
# real terminals must converge on one quiet owner and retain distinct startup
# inputs, including whichever attachment initially becomes an observer.
: >"$root/stall.out"
"$boot_fixture" --stall "$runtime_root" "$work" >"$root/stall.out" & stall_pid=$!
wait_for has "$root/stall.out" STALLED
plan '{"plan":["RACE FIRST RESPONSE","RACE SECOND RESPONSE"]}'
terminal_launch -i FIRST_STARTUP
wait_for test -f "$term/text"
wait_for test -f "$term/feed"
send FIRST_DRAFT
expect_prompt FIRST_DRAFT
first_term=$term first_pid=$terminal_pid first_event=$terminal_event_fd
extra_terminal_pid=$first_pid
exec {first_input}>&7
terminal_launch -i SECOND_STARTUP
wait_for test -f "$term/text"
wait_for test -f "$term/feed"
send SECOND_DRAFT
expect_prompt SECOND_DRAFT
second_term=$term second_pid=$terminal_pid second_event=$terminal_event_fd
exec {second_input}>&7
kill -TERM "$stall_pid"
wait "$stall_pid" 2>/dev/null || true
stall_pid=
wait_for has "$first_term/text" 'Send /help for usage.'
wait_for has "$second_term/text" 'Send /help for usage.'
controlled() {
  ! has "$term/message" 'Observing workspace' &&
    { has "$term/message" 'Worked for' || has "$term/message" 'Conversation attached' || has "$term/message" 'Ready.'; }
}
requested() {
  [[ -f "$root/peer/requests.jsonl" ]] &&
    jq -se --arg text "$1" 'any(.[]; any(.input[]?; .role == "user" and any(.content[]?; .text? == $text)))' "$root/peer/requests.jsonl" >/dev/null
}
term=$first_term terminal_pid=$first_pid terminal_event_fd=$first_event
extra_terminal_pid=$second_pid
exec 7>&$first_input
send '\025/takeover\r'
wait_for controlled
wait_for requested FIRST_STARTUP
term=$second_term terminal_pid=$second_pid terminal_event_fd=$second_event
extra_terminal_pid=$first_pid
exec 7>&$second_input
send '\025/takeover\r'
wait_for controlled
wait_for requested SECOND_STARTUP
wait_for has "$term/feed" 'RACE SECOND RESPONSE'
[[ $(requests) == 2 ]]
[[ $(find "$runtime_root" -name 'ws-*.info' | wc -l) == 1 ]]
publication=$(find "$runtime_root" -name 'ws-*.info' -print -quit)
terminal_close
term=$first_term terminal_pid=$first_pid terminal_event_fd=$first_event
extra_terminal_pid=
terminal_events=1
exec 7>&$first_input
terminal_close
wait_for test ! -e "$publication"
exec {first_input}>&-
exec {second_input}>&-

# Quiet-owner startup failures are rendered once in the terminal. They must
# still fail the executable, without duplicating the error as raw JSON.
export CAI_TEST_REQUIRE_APPROVAL=1
base=("$cli" -p custom --endpoint "$endpoint" -m gpt-5.6-luna --no-terminal --no-image-generation --no-mcp -C "$root/other" --runtime-dir "$runtime_root")
terminal_launch
wait_for test -f "$term/status"
wait "$terminal_pid" || true
terminal_pid=
contains "$term/feed" 'Workspace is not approved.'
[[ $(grep -c 'Workspace is not approved.' "$term/feed") == 1 ]]
[[ ! -s "$term/stderr" ]]
[[ $(cat "$term/status") != 0 ]]
[[ $(requests) == 2 ]]
