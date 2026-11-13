#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-socket}
peer_start
runtime_root=$(mktemp -d "$support/../build/dq.XXXXXX")
base+=(--runtime-dir "$runtime_root")
gate_open=0 pipe_pid=
quit_cleanup() {
  local result=$?
  if ((gate_open)); then printf y >&8; fi
  if [[ -n $pipe_pid ]]; then
    kill "$pipe_pid" 2>/dev/null || :
    wait "$pipe_pid" 2>/dev/null || :
  fi
  "${base[@]}" -X >"$root/cleanup.out" 2>"$root/cleanup.err" || :
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap quit_cleanup EXIT

publication() { find "$runtime_root" -name 'ws-*.info' -print -quit; }
no_server() { [[ -z $(publication) ]]; }
finish_terminal() {
  wait_for test -s "$term/status"
  wait "$terminal_pid" || fail 'Terminal CLI failed'
  terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -s "$term/stderr" ]]
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
}
close_keys() { send "$1"; finish_terminal; }
export_current() {
  run --list-all; ok
  id=$(awk 'NR==1 {print $1}' "$root/stdout")
  [[ -n $id ]]
  run --export "$id" --export-out "$root/result.md"; ok
}
if [[ $mode == durable ]]; then
  # The final text is visible, but its completion append remains blocked.
  # Quitting must not cancel that append or release the workspace lease early.
  export CAI_TEST_APPEND_GATE_FINAL=1 CAI_TEST_APPEND_GATE="$root/final-gate"
  mkdir -p "$CAI_TEST_APPEND_GATE"
  mkfifo "$CAI_TEST_APPEND_GATE/release"
  exec 8<>"$CAI_TEST_APPEND_GATE/release"
  gate_open=1
  plan '{"plan":["DURABLE_QUIT_FINAL"]}'
  terminal_start -N
  send 'Finish this durably\r'
  wait_for test -e "$CAI_TEST_APPEND_GATE/entered"
  expect 'DURABLE_QUIT_FINAL'
  send '/quit\r'
  expect 'Goodbye.'
  [[ -n $(publication) ]]
  # A synchronous embedding retains its owned backend until settlement even
  # though its frontend has detached. It must neither return nor cancel here.
  [[ ! -e $term/status ]]
  printf y >&8
  finish_terminal
  wait_for no_server
  unset CAI_TEST_APPEND_GATE CAI_TEST_APPEND_GATE_FINAL
  exec 8>&-
  gate_open=0
  export_current
  contains "$root/result.md" 'DURABLE_QUIT_FINAL'
  exit 0
fi

for action in quit altq eof reconnect queued goal 'quit now' 'quit force' 'quit -f' 'quit --force' terminate; do
  before=$(requests)
  plan '{"plan":[{"pause":true,"pause_text":"QUIT_IN_FLIGHT\n\n","text":"QUIT_FINAL_COMMITTED"}]}'
  terminal_start -N
  if [[ $action == goal ]]; then
    send '/goal new Complete this goal\r'
  else
    send 'Complete the current turn\r'
  fi
  expect 'QUIT_IN_FLIGHT'
  if [[ $action == queued ]]; then
    send 'MUST_STAY_QUEUED\r'
    expect 'MUST_STAY_QUEUED'
  fi
  info=$(publication)
  [[ -n $info ]]
  cp "$info" "$root/instance.info"
  case $action in
    altq) close_keys '\033q' ;;
    eof) close_keys '\004' ;;
    reconnect|queued|goal) close_keys '/quit\r' ;;
    *) close_keys "/$action\r" ;;
  esac
  case $action in
    quit|altq|eof|reconnect|queued|goal)
      [[ -f $info ]] || fail 'Ordinary detach cancelled an active turn'
      if [[ $action == reconnect ]]; then
        terminal_start
        cmp "$info" "$root/instance.info"
        expect 'QUIT_IN_FLIGHT'
      fi
      release
      if [[ $action == reconnect ]]; then
        expect 'QUIT_FINAL_COMMITTED'
        expect 'Worked for'
        [[ -f $info ]]
        terminal_close
      fi
      wait_for no_server
      export_current
      contains "$root/result.md" 'QUIT_FINAL_COMMITTED'
      ;;
    *)
      wait_for no_server
      release
      export_current
      absent "$root/result.md" 'QUIT_FINAL_COMMITTED'
      ;;
  esac
  [[ $(requests) == $((before+1)) ]]
  rm -f "$root/result.md"
done

# Force means an immediate detach, not an unconditional workspace kill.
plan '{"plan":[{"pause":true,"pause_text":"PERSISTENT_QUIT\n\n","text":"PERSISTENT_FINAL"}]}'
terminal_start -N --persist
send 'Continue without a client\r'
expect 'PERSISTENT_QUIT'
info=$(publication)
close_keys '/terminate\r'
[[ -f $info ]]
release
terminal_start
expect 'PERSISTENT_FINAL'
expect 'Worked for'
terminal_close
[[ -f $info ]]
run -X; ok
wait_for no_server

# Redirected input has no terminal key path. A force command must still
# detach without waiting for an already accepted turn to finish.
plan '{"plan":[{"pause":true,"pause_text":"PIPE_QUIT_IN_FLIGHT\n\n","text":"PIPE_MUST_NOT_FINISH"}]}'
mkfifo "$root/pipe-input"
exec 9<>"$root/pipe-input"
( "${base[@]}" -N <"$root/pipe-input" >"$root/pipe.out" 2>"$root/pipe.err"; printf '%s\n' "$?" >"$root/pipe.status" ) & pipe_pid=$!
printf 'Start a pipe turn\n' >&9
wait_for has "$root/pipe.out" PIPE_QUIT_IN_FLIGHT
printf '/terminate\n' >&9
wait_for test -s "$root/pipe.status"
wait "$pipe_pid"
pipe_pid=
[[ $(cat "$root/pipe.status") == 0 ]]
wait_for no_server
release
absent "$root/pipe.out" 'PIPE_MUST_NOT_FINISH'
exec 9>&-
