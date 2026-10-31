#!/usr/bin/env bash
# The real app opens the same conversation twice, then recovers a killed owner.
# Only the wire provider is a fixture; journal and ownership use real Pouch.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["OWNER READY"]}'
terminal_start -N
prompt 'original owner turn' 'OWNER READY'
id=$(current_session_id)
[[ -n "$id" ]]
before=$(requests)
run --resume "$id" -ni 'MUST NOT REACH INFERENCE'; bad
contains "$root/stderr" 'owned'
[[ $(requests) == "$before" ]]
# Ownership does not prevent another process from exporting the committed view.
run --export "$id"; ok
contains "$root/data/cai/exports/$id/$id.md" 'OWNER READY'
# A different conversation on that same root still works while the owner is idle.
plan '{"plan":["INDEPENDENT READY"]}'
run -Nni 'independent conversation'; ok
contains "$root/stdout" 'INDEPENDENT READY'
send '/status\r'; expect "$id"
wait_for test -s "$term/tty"
read -r canonical foreground app_pid <"$term/tty"
[[ "$app_pid" =~ ^[0-9]+$ && "$app_pid" -gt 1 ]]
kill -KILL "$app_pid"
wait_for test -s "$term/status"
status=0; wait "$terminal_pid" || status=$?
[[ "$status" == 137 && $(cat "$term/status") == 137 ]]
terminal_pid=
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
# Recovery is immediate; no wait for the SDK's thirty-second lease expiry.
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" 'OWNER READY'
absent "$term/feed" 'INDEPENDENT READY'
[[ $(requests) == "$before" ]]
plan '{"plan":["AFTER CRASH"]}'
prompt 'continue after recovery' 'AFTER CRASH'
terminal_close
# Normal shutdown releases ownership as well, permitting a further reopen.
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" 'AFTER CRASH'
terminal_close
