#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
host=$(realpath "$1")
controller=$(realpath "$2")
mode=${3:-native}
runtime_root=$(mktemp -d "$support/../build/handoff.XXXXXX")
owner_pid=
handoff_cleanup() {
  local result=$?
  if [[ -n $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; then
    "$controller" "$runtime_root" "$work" stop >"$root/stop.out" 2>"$root/stop.err" || true
  fi
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap handoff_cleanup EXIT
export LOG_LEVEL=info
export SHELL="$work/user shell"
export CAI_HANDOFF_VALUE='literal=environment value'
mkdir "$root/programs"
export PATH="$root/programs:$PATH"
cat >"$SHELL" <<'SH'
#!/bin/sh
test "$1" = -i && test -t 0 && test -t 1 || exit 77
printf 'NATIVE_SHELL_READY\n'
PS1='HANDOFF_PROMPT> '
export PS1
exec /bin/sh -i
SH
cat >"$root/programs/less" <<'SH'
#!/bin/sh
test "$1" = -- && test -f "$2" || exit 78
printf 'NATIVE_PAGER_READY|%s|%s\n' "$PWD" "$2"
IFS= read -r reply
printf 'PAGER_INPUT_%s\n' "$reply"
SH
chmod 700 "$SHELL" "$root/programs/less"
printf '[env]\nCAI_HANDOFF_CONFIG = derived value\n' >"$root/handoff.ini"
base+=(--runtime-dir "$runtime_root" --persist --config "$root/handoff.ini")
native_base=("${base[@]}")
base=(/usr/bin/env "PATH=$root/programs" "${native_base[@]}")
terminal_start
if [[ $mode == embed ]]; then
  terminal_close
  base=(/usr/bin/env "PATH=$root/programs" "$host" "$runtime_root" "$work" "${native_base[@]:1}")
  terminal_start
fi
plan '{"plan":["PREHANDOFF"]}'
prompt 'one turn before handing off' PREHANDOFF
send 'DRAFT survives handoff\033s'
expect NATIVE_SHELL_READY
send 'printf "SHELL_CWD_%s\n" "$PWD"; printf "ENV_%s\n" "$CAI_HANDOFF_VALUE"; printf "CONFIG_%s\n" "$CAI_HANDOFF_CONFIG"\r'
expect "SHELL_CWD_$work"
expect 'ENV_literal=environment value'
expect 'CONFIG_derived value'
resize 71
send '/bin/stty size\r'; expect '30 71'
plan '{"plan":["HANDOFFANSWER"]}'
"$controller" "$runtime_root" "$work" work >"$root/control.out" 2>"$root/control.err"
[[ $(requests) == 2 ]]
absent "$term/text" HANDOFFANSWER
send 'exit 7\r'
expect 'Shell exited 7.'
expect HANDOFFANSWER
expect_prompt 'DRAFT survives handoff'
[[ $(grep -o PREHANDOFF "$term/feed" | wc -l) == 1 ]]
wait_for has "$term/message" 'Observing workspace.'
send '\025/takeover\r'
not_observing() { [[ -s "$term/message" ]] && ! grep -F 'Observing workspace.' "$term/message" >/dev/null; }
wait_for not_observing
send '/status\r'; expect 'Session ID'
plan '{"plan":["POSTHANDOFF"]}'
prompt 'continue after the handoff' POSTHANDOFF
absent "$root/peer/requests.jsonl" SHELL_CWD_
absent "$root/peer/requests.jsonl" ENV_literal
session_log=$(find "$root/cache/cai" -maxdepth 1 -name '*.log' -print -quit)
completed_before=$(grep -c '"event":"run_completed"' "$session_log")
plan '{"plan":[{"pause":true,"pause_text":"ACTIVEPREFIX\n\n","text":"ACTIVEHANDOFFDONE"}]}'
send 'continue generating during handoff\r'; expect ACTIVEPREFIX
send 'ACTIVE DRAFT\033s'; expect NATIVE_SHELL_READY
release
generation_finished() { [[ $(grep -c '"event":"run_completed"' "$session_log") -gt $completed_before ]]; }
wait_for generation_finished
absent "$term/text" ACTIVEHANDOFFDONE
send 'exit\r'; expect 'Shell exited 0.'; expect ACTIVEHANDOFFDONE
expect_prompt 'ACTIVE DRAFT'
[[ $(grep -o ACTIVEPREFIX "$term/feed" | wc -l) == 1 ]]
absent "$root/peer/requests.jsonl" 'ACTIVE DRAFT'
send '\025/status\r'; expect 'Session ID'
send '/shell\r'; expect NATIVE_SHELL_READY
send 'printf INTERRUPT_READY; /bin/sleep 60\r'; expect INTERRUPT_READY
send '\003'; expect HANDOFF_PROMPT
send 'printf AFTER_INTERRUPT; exit\r'
expect AFTER_INTERRUPT
expect 'Shell exited 0.'
send '!sh\r'; expect NATIVE_SHELL_READY
send 'kill -KILL $$\r'; expect 'Shell terminated by signal 9.'
send '/shell\r'; expect NATIVE_SHELL_READY
send '\r~.'; expect 'Shell escaped.'
send '/log -p\r'; expect "NATIVE_PAGER_READY|$work|"
send 'quit\r'; expect PAGER_INPUT_quit; expect 'Pager exited 0.'
send '/log less\r'; expect NATIVE_PAGER_READY
send 'quit\r'; expect 'Pager exited 0.'
send '/shell\r'; expect NATIVE_SHELL_READY
"$controller" "$runtime_root" "$work" new >"$root/new.out" 2>"$root/new.err"
send 'exit\r'; expect 'Shell exited 0.'
wait_for has "$term/message" 'Observing workspace.'
send '/takeover\r'; wait_for not_observing
send '/status\r'; expect 'Session ID'
plan '{"plan":["NEWSESSIONHANDOFF"]}'
prompt 'continue in the new conversation' NEWSESSIONHANDOFF
jq -se '.[-1].input | tostring | contains("one turn before handing off") | not' "$root/peer/requests.jsonl" >/dev/null
rm "$SHELL"
send '/shell\r'; expect 'execute local terminal program'
send '/status\r'; expect 'Session ID'
rm "$root/programs/less"
pager_offset=$(wc -c <"$term/raw")
send '/log less\r'
pager_opened() { tail -c +$((pager_offset+1)) "$term/raw" | grep -F '[?1049h' >/dev/null; }
wait_for pager_opened
plan '{"plan":["FALLBACKANSWER"]}'
"$controller" "$runtime_root" "$work" work >"$root/pager-control.out" 2>"$root/pager-control.err"
absent "$term/feed" FALLBACKANSWER
send q; expect 'Pager closed.'
expect FALLBACKANSWER
wait_for has "$term/message" 'Observing workspace.'
send '/takeover\r'; wait_for not_observing
send '/status\r'; expect 'Session ID'
[[ $(requests) == 6 ]]

# The fallback viewer redraws after a second terminal resize, then restores
# the same editor/attachment without injecting pager keys as agent input.
pager_offset=$(wc -c <"$term/raw")
send '/log -p\r'; wait_for pager_opened
pager_frame_offset=$pager_offset
pager_drawn() { tail -c +$((pager_frame_offset+1)) "$term/raw" | grep -E '[0-9]+%' >/dev/null; }
wait_for pager_drawn
pager_frame_offset=$(wc -c <"$term/raw")
resize 83
wait_for pager_drawn
send q; expect 'Pager closed.'
send '/status\r'; expect 'Session ID'
[[ $(requests) == 6 ]]

# Owner loss must cancel the finite viewer and restore its terminal, rather
# than leaving a dead pager waiting for a key with no working attachment.
pager_offset=$(wc -c <"$term/raw")
send '/log -p\r'; wait_for pager_opened
"$controller" "$runtime_root" "$work" stop >"$root/pager-stop.out" 2>"$root/pager-stop.err"
wait_for test -s "$term/status"
peer_rc=0; wait "$terminal_pid" || peer_rc=$?; terminal_pid=
[[ $peer_rc == 1 ]]
[[ $(cat "$term/status") == 1 ]]
tail -c +$((pager_offset+1)) "$term/raw" | grep -F '[?1049l' >/dev/null
exec 7>&-; exec {terminal_event_fd}>&-; terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
no_publication() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
wait_for no_publication

# A foreground headless owner logs to its explicit sink, so no session log is
# available. Rejecting the pager action must leave transcript delivery alive.
"${native_base[@]}" --headless -N </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
terminal_start
send '/log -p\r'; expect 'No log file is configured'
send '/status\r'; expect 'Session ID'
terminal_close /shutdown
wait "$owner_pid"; owner_pid=
wait_for no_publication
