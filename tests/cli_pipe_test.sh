#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
native_host=$1
runtime_root=$(mktemp -d "$support/../build/pipe.XXXXXX")
pipe_pid= owner_pid=
pipe_cleanup() {
  local result=$?
  if [[ -n "$pipe_pid" ]]; then kill "$pipe_pid" 2>/dev/null || :; wait "$pipe_pid" 2>/dev/null || :; fi
  if [[ -n "$owner_pid" ]]; then kill "$owner_pid" 2>/dev/null || :; wait "$owner_pid" 2>/dev/null || :; fi
  [[ "$result" != 0 ]] || rm -rf -- "$runtime_root"
  cleanup "$result"
}
trap pipe_cleanup EXIT
base+=(--runtime-dir "$runtime_root" --replay-turns 0)
no_owner() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
owner_published() { [[ -n $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }

plan '{"plan":["FIRST PIPE REPLY","SECOND PIPE REPLY"]}'
printf '\nfirst pipe prompt\r\nsecond pipe prompt' | "${base[@]}" -N >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'FIRST PIPE REPLY'
contains "$root/stdout" 'SECOND PIPE REPLY'
[[ $(requests) == 2 ]]
[[ -z $(sed -n '/^  [^[:space:]]/p' "$root/stdout") ]]
wait_for no_owner
first_session=$(current_session_id)

# EOF is not needed to infer, render a word, or run a command. The output
# must become visible while input remains open and the provider is paused.
mkfifo "$root/input"
exec 8<>"$root/input"
plan '{"plan":[{"pause":true,"pause_text":"LIVE BEFORE EOF ","text":"LIVE TURN FINISHED"},"AFTER PIPE COMMAND"]}'
"${base[@]}" <"$root/input" >"$root/live.out" 2>"$root/live.err" & pipe_pid=$!
printf 'live pipe prompt\n' >&8
wait_for has "$root/live.out" 'LIVE BEFORE EOF'
[[ $(requests) == 3 ]]
printf '/status\nnext pipe prompt\n/quit\nMUST NOT RUN AFTER QUIT\n' >&8
# Read-only commands must apply while the previous request is still paused.
wait_for has "$root/live.out" 'Session ID'
release
wait_for has "$root/live.out" 'AFTER PIPE COMMAND'
wait "$pipe_pid"; pipe_pid=
exec 8>&-
[[ $(requests) == 4 ]]
absent "$root/peer/requests.jsonl" 'MUST NOT RUN AFTER QUIT'
wait_for no_owner

# A queued goal does not block subsequent control admission. Pausing it must
# be possible from this same pipe, even before its first response completes.
before_goal=$(requests)
exec 8<>"$root/input"
plan '{"plan":[{"pause":true,"pause_text":"PIPE GOAL IN FLIGHT ","text":"PIPE GOAL RESPONSE"},"PIPE GOAL PAUSED"]}'
"${base[@]}" <"$root/input" >"$root/goal.out" 2>"$root/goal.err" & pipe_pid=$!
printf '/goal new pipe-controlled goal\n' >&8
wait_for has "$root/goal.out" 'PIPE GOAL IN FLIGHT'
printf '/status\n/goal pause\n/quit\n' >&8
wait_for has "$root/goal.out" 'Session ID'
release
wait "$pipe_pid"; pipe_pid=
exec 8>&-
[[ $(requests) -le $((before_goal+2)) ]]
contains "$root/goal.out" 'PIPE GOAL RESPONSE'
wait_for no_owner

# Context changes are actual admission barriers. Following input belongs to
# the new session, never an in-flight request against the old binding.
plan '{"plan":["NEW PIPE CONTEXT"]}'
printf '/new\nnew pipe session\n/quit\n' | "${base[@]}" >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'NEW PIPE CONTEXT'
wait_for no_owner
plan '{"plan":["RESUMED PIPE CONTEXT"]}'
printf '/resume %s\nresumed pipe session\n/quit\n' "$first_session" | "${base[@]}" >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'RESUMED PIPE CONTEXT'
absent "$root/stdout" 'FIRST PIPE REPLY'
absent "$root/stdout" 'PIPE GOAL RESPONSE'
wait_for no_owner
before=$(requests)
printf '/new\n/quit\n' | "${base[@]}" >"$root/stdout" 2>"$root/stderr"
[[ $(requests) == "$before" ]]
wait_for no_owner

# Missing terminal capabilities are inline command errors; they do not kill
# the attachment or consume the next prompt as shell input.
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"printf PIPE_TOOL_OUTPUT"}},"PIPE TOOLS DONE"]}'
printf '/shell\n/tool-does-not-exist\npipe tool prompt\n/quit\n' | "${base[@]}" --terminal >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'interactive shell requires terminal input and output'
contains "$root/stdout" 'PIPE_TOOL_OUTPUT'
contains "$root/stdout" 'Ran printf PIPE_TOOL_OUTPUT'
contains "$root/stdout" 'PIPE TOOLS DONE'
wait_for no_owner

# Explicit selection discovers the existing owner instead of opening a
# second writer. Quitting this client preserves the persistent owner.
plan '{"plan":[]}'
"${base[@]}" -x --workspace-name pipe-server </dev/null >"$root/owner.out" 2>"$root/owner.err" & owner_pid=$!
wait_for owner_published
plan '{"plan":["EXISTING PIPE OWNER"]}'
printf 'existing pipe owner\n/quit\n' | "${base[@]}" -s pipe-server >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'EXISTING PIPE OWNER'
[[ -n $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]
# Native run's explicit attachment must likewise open no runtime or SDK store,
# including on a default-stack worker and when its host stops before readiness.
plan '{"plan":["NATIVE EXISTING PIPE OWNER"]}'
printf 'native existing pipe owner\n/quit\n' | "$native_host" "${base[@]:1}" -s pipe-server >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'NATIVE EXISTING PIPE OWNER'
saved_cli=${base[0]}
base[0]=$native_host
plan '{"plan":["NATIVE TERMINAL OWNER"]}'
terminal_start -s pipe-server
prompt 'native terminal attachment' 'NATIVE TERMINAL OWNER'
terminal_close
base[0]=$saved_cli
CAI_TEST_ATTACH_STOP=1 "$native_host" "${base[@]:1}" -s pipe-server <"$root/input" >"$root/stdout" 2>"$root/stderr" & pipe_pid=$!
# Keep this source open without submitting anything; host STOP must wake the
# attachment even during discovery, rather than waiting for input or a turn.
exec 8<>"$root/input"
wait "$pipe_pid"; pipe_pid=
exec 8>&-
[[ -n $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]
mkfifo "$root/native-stop"
exec 8<>"$root/input"
exec 9<>"$root/native-stop"
CAI_TEST_ATTACH_STOP_FILE="$root/native-stop" "$native_host" "${base[@]:1}" -s pipe-server <"$root/input" >"$root/stdout" 2>"$root/stderr" & pipe_pid=$!
printf '/status\n' >&8
wait_for has "$root/stdout" 'Session ID'
printf x >&9
wait "$pipe_pid"; pipe_pid=
exec 8>&-
exec 9>&-
for stop_surface in editor pager shell; do
  exec 9<>"$root/native-stop"
  export CAI_TEST_ATTACH_STOP_FILE="$root/native-stop"
  saved_cli=${base[0]}
  base[0]=$native_host
  saved_shell=$SHELL
  if [[ "$stop_surface" == shell ]]; then
    printf '#!/bin/sh\nprintf NATIVE_STOP_SHELL_READY\nexec /bin/sh -i\n' >"$work/native-stop-shell"
    chmod 700 "$work/native-stop-shell"
    export SHELL="$work/native-stop-shell"
  fi
  terminal_start -s pipe-server
  if [[ "$stop_surface" == pager ]]; then
    pager_offset=$(wc -c <"$term/raw")
    send '/help -p\r'; expect 'Your coding agent'
    tail -c +$((pager_offset+1)) "$term/raw" >"$root/pager-opened"
    contains "$root/pager-opened" '[?1049h'
  elif [[ "$stop_surface" == shell ]]; then
    send '/shell\r'; expect NATIVE_STOP_SHELL_READY
  else
    send '/status\r'; expect 'Session ID'
  fi
  printf x >&9
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -s "$term/stderr" ]]
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD CAI_TEST_ATTACH_STOP_FILE
  base[0]=$saved_cli
  export SHELL=$saved_shell
  exec 9>&-
done
printf '/status\n/quit\n' | "${base[@]}" -s pipe-server >"$root/stdout" 2>"$root/stderr"
contains "$root/stdout" 'Session ID'
# Detaching an active native frontend must not cancel remote inference. The
# process returns while the provider remains paused, then the owner completes
# after release without any attached terminal.
plan '{"plan":[{"pause":true,"pause_text":"NATIVE BACKGROUND IN FLIGHT ","text":"NATIVE BACKGROUND AFTER DETACH"}]}'
exec 8<>"$root/input"
exec 9<>"$root/native-stop"
CAI_TEST_ATTACH_STOP_FILE="$root/native-stop" "$native_host" "${base[@]:1}" -s pipe-server <"$root/input" >"$root/stdout" 2>"$root/stderr" & pipe_pid=$!
printf 'continue while detached\n' >&8
wait_for has "$root/stdout" 'NATIVE BACKGROUND IN FLIGHT'
printf x >&9
wait "$pipe_pid"; pipe_pid=
exec 8>&-
exec 9>&-
release
wait_for has "$root/owner.err" 'NATIVE BACKGROUND AFTER DETACH'
if "$native_host" "${base[@]:1}" -s nonexistent-server </dev/null >"$root/stdout" 2>"$root/stderr"; then fail 'Missing native owner accepted'; fi
contains "$root/stderr" 'no running server matches --server'
if "$native_host" "${base[@]:1}" -x </dev/null >"$root/stdout" 2>"$root/stderr"; then fail 'Native run accepted headless process mode'; fi
contains "$root/stderr" 'must use cai_app_start_server'
kill -TERM "$owner_pid"; wait "$owner_pid"; owner_pid=
wait_for no_owner

# Reject oversized or binary input explicitly, with no model submission.
before=$(requests)
head -c 66049 /dev/zero | tr '\0' x >"$root/oversized"
if "${base[@]}" <"$root/oversized" >"$root/stdout" 2>"$root/stderr"; then fail 'Oversized line accepted'; fi
contains "$root/stderr" 'line exceeds input limit'
printf 'prefix\0suffix\n' >"$root/binary"
if "${base[@]}" <"$root/binary" >"$root/stdout" 2>"$root/stderr"; then fail 'Binary line accepted'; fi
contains "$root/stderr" 'input contains NUL'
[[ $(requests) == "$before" ]]
wait_for no_owner

# Closed/empty input exits cleanly, with no extra prompt or newline.
"${base[@]}" </dev/null >"$root/stdout" 2>"$root/stderr"
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
wait_for no_owner
