#!/usr/bin/env bash
# Startup selection precedes replay, for fresh and already running owners.
cli_fixture_parent=$(dirname "$(dirname "$(realpath "$1")")")
source "$(dirname "$0")/cli_test_common.sh"
peer_start
base+=(--runtime-dir "$root/run")
selection_owner=
selection_cleanup() {
  local result=$?
  if [[ -n "$selection_owner" ]]; then
    kill -TERM "$selection_owner" 2>/dev/null || true
    wait "$selection_owner" 2>/dev/null || true
  fi
  cleanup "$result"
}
trap selection_cleanup EXIT

terminal_start -N
plan '{"plan":["FirstConversationOnly"]}'
prompt 'first conversation' FirstConversationOnly
first=$(current_session_id)
send '/new\r'; expect 'New session'; expect 'ID:'
plan '{"plan":["SecondConversationOnly"]}'
prompt 'second conversation' SecondConversationOnly
second=$(current_session_id)
[[ "$first" != "$second" ]]
terminal_close
owner_stopped() { ! compgen -G "$root/run/ws-*.sock" >/dev/null; }
wait_for owner_stopped

row_for() { "${base[@]}" -R | awk -v id="$1" '$2 == id {print $1}'; }
assert_selected() {
  contains "$term/feed" "$1"
  absent "$term/feed" "$2"
  absent "$term/feed" '$ /resume'
  absent "$term/feed" 'Conversation already active.'
}
close_eof() {
  send '\004'
  wait_for test -s "$term/status"
  wait "$terminal_pid" || fail 'Ctrl-D exit failed'
  terminal_pid=
  [[ $(cat "$term/status") == 0 ]]
  contains "$term/text" '! Goodbye.'
  [[ ! -s "$term/stderr" ]]
  exec 7>&-
}

# A fresh owner receives the numeric selector directly; there is no generated
# /resume input, and Ctrl-D completes the same goodbye path as /quit.
before=$(requests)
terminal_start -R "$(row_for "$first")"
assert_selected FirstConversationOnly SecondConversationOnly
close_eof
wait_for owner_stopped
[[ $(requests) == "$before" ]]

mkdir "$root/foreign"
printf '/quit\n' | "${base[@]}" -N -C "$root/foreign" >"$root/foreign.out" 2>"$root/foreign.err"
foreign=$("${base[@]}" -C "$root/foreign" -R | awk 'NR==1 {print $2}')
[[ -n "$foreign" ]]
wait_for owner_stopped

# Keep an owner alive so selection must cross the socket before binding.
export LOG_LEVEL=info
"${base[@]}" -x --resume "$second" >"$root/owner.out" 2>"$root/owner.log" & selection_owner=$!
wait_for has "$root/owner.log" 'Workspace server ready'
terminal_start --resume "$(row_for "$first")"
assert_selected FirstConversationOnly SecondConversationOnly
close_eof
kill -0 "$selection_owner"
terminal_start --resume "$second"
assert_selected SecondConversationOnly FirstConversationOnly
terminal_close

# Selecting the already active conversation by number is silent too.
terminal_start -R "$(row_for "$second")"
assert_selected SecondConversationOnly FirstConversationOnly
terminal_close

# --new shares setup selection; it creates a distinct empty context rather
# than replaying the previous one and submitting a synthetic /new command.
terminal_start -N
absent "$term/feed" FirstConversationOnly
absent "$term/feed" SecondConversationOnly
absent "$term/feed" '$ /new'
created=$(current_session_id)
[[ "$created" != "$first" && "$created" != "$second" ]]
terminal_close
terminal_start -R "$second"
terminal_close

# Invalid selectors fail before any transcript is replayed. The owner keeps
# its selected conversation and can accept a subsequent valid attachment.
for choice in missing-conversation 0 999999 "$foreign"; do
  terminal_launch -R "$choice"
  wait_for test -s "$term/status"
  wait "$terminal_pid" || true
  terminal_pid=
  [[ $(cat "$term/status") != 0 ]]
  if [[ "$choice" == "$foreign" ]]; then
    contains "$term/text" 'session belongs to another directory'
  fi
  absent "$term/feed" FirstConversationOnly
  absent "$term/feed" SecondConversationOnly
  exec 7>&-
  kill -0 "$selection_owner"
done
terminal_start -R "$second"
assert_selected SecondConversationOnly FirstConversationOnly
terminal_close
[[ $(requests) == "$before" ]]

# Noninteractive attachment uses the same upfront selection, even while the
# headless owner currently has another conversation selected.
plan '{"plan":["BatchSelectedConversationOnly"]}'
run -n --resume "$first" -i 'batch selection'
ok
contains "$root/stdout" BatchSelectedConversationOnly
absent "$root/stdout" '$ /resume'
terminal_start -R "$first"
contains "$term/feed" BatchSelectedConversationOnly
absent "$term/feed" SecondConversationOnly
terminal_close
terminal_start -R "$second"
absent "$term/feed" BatchSelectedConversationOnly
terminal_close
before=$((before+1))
[[ $(requests) == "$before" ]]

# A headless owner can be working before any client attaches. A different
# selector must fail instead of interrupting that work or replaying its text.
kill -TERM "$selection_owner"
wait "$selection_owner" || true
selection_owner=
wait_for owner_stopped
plan '{"plan":[{"pause":true,"text":"HeadlessWorkFinished"}]}'
"${base[@]}" -x -R "$second" -i 'headless work' >"$root/busy-owner.out" 2>"$root/busy-owner.log" & selection_owner=$!
wait_for has "$root/busy-owner.log" 'STREAM in flight'
terminal_launch -R "$first"
wait_for test -s "$term/status"
wait "$terminal_pid" || true
terminal_pid=
[[ $(cat "$term/status") != 0 ]]
contains "$term/text" 'Wait for the current turn to finish.'
absent "$term/feed" FirstConversationOnly
absent "$term/feed" SecondConversationOnly
exec 7>&-
release
wait_for has "$root/busy-owner.log" '"event":"run_completed"'
contains "$root/busy-owner.log" HeadlessWorkFinished
terminal_start -R "$first"
assert_selected FirstConversationOnly SecondConversationOnly
terminal_close
[[ $(requests) == $((before+1)) ]]
