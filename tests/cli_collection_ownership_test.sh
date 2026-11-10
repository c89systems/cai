#!/usr/bin/env bash
# The CLI refuses competing controllers, then recovers a killed workspace owner.
# Only the wire provider is a fixture; journal and ownership use real Pouch.
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-native}
peer_start
plan '{"plan":["OWNER READY"]}'
terminal_start -N
prompt 'original owner turn' 'OWNER READY'
id=$(current_session_id)
[[ -n "$id" ]]
before=$(requests)
run --resume "$id" -ni 'MUST NOT REACH INFERENCE'; bad
if [[ "$mode" == socket ]]; then contains "$root/stderr" 'controller authority'
else contains "$root/stderr" 'server owner'; fi
[[ $(requests) == "$before" ]]
# Ownership does not prevent another process from exporting the committed view.
run --export "$id"; ok
contains "$root/data/cai/exports/$id/$id.md" 'OWNER READY'
# A second frontend cannot switch the controlling terminal's session.
run -Nni 'independent session'; bad
if [[ "$mode" == socket ]]; then contains "$root/stderr" 'controller authority'
else contains "$root/stderr" 'server owner'; fi
[[ $(requests) == "$before" ]]
send '/status\r'; expect "$id"
wait_for test -s "$term/tty"
read -r canonical foreground app_pid <"$term/tty"
[[ "$app_pid" =~ ^[0-9]+$ && "$app_pid" -gt 1 ]]
# Quiet startup gives the frontend an exact child-process watch. Inspect only
# that known process's direct children; never search the host process table.
owner_pid=$app_pid
if [[ "$mode" == socket ]]; then
  owner_children=()
  read -r -a owner_children <"/proc/$app_pid/task/$app_pid/children" || :
  [[ ${#owner_children[@]} == 1 ]]
  owner_pid=${owner_children[0]}
  [[ "$owner_pid" =~ ^[0-9]+$ && "$owner_pid" -gt 1 ]]
fi
kill -KILL "$owner_pid"
wait_for test -s "$term/status"
status=0; wait "$terminal_pid" || status=$?
expected=137
[[ "$mode" != socket ]] || expected=1
[[ "$status" == "$expected" && $(cat "$term/status") == "$expected" ]]
terminal_pid=
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
# Recovery is immediate; no wait for CAI's twenty-four-hour lease expiry.
before=$(requests)
terminal_start --resume "$id"
expect 'Session resumed.'
contains "$term/text" 'OWNER READY'
absent "$term/feed" 'INDEPENDENT READY'
[[ $(requests) == "$before" ]]
plan '{"plan":["AFTER CRASH"]}'
prompt 'continue after recovery' 'AFTER CRASH'
terminal_close
# Normal shutdown releases ownership as well, permitting a further reopen.
terminal_start --resume "$id"
expect 'Session resumed.'
contains "$term/text" 'AFTER CRASH'
terminal_close
