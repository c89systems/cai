#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/stop.XXXXXX")
base+=(--runtime-dir "$runtime_root")
first_pid= second_pid=
finish() {
  local result=$? pid
  for pid in "$first_pid" "$second_pid"; do
    if [[ -n "$pid" ]]; then kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  done
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap finish EXIT
mkdir -p "$root/other" "$root/unrelated"
ln -s "$work" "$root/alias"
export LOG_LEVEL=info
"${base[@]}" -x -N >"$root/first.out" 2>"$root/first.log" & first_pid=$!
wait_for has "$root/first.log" 'Workspace server ready'
"${base[@]}" -C "$root/other" -x -N >"$root/second.out" 2>"$root/second.log" & second_pid=$!
wait_for has "$root/second.log" 'Workspace server ready'
run --list --all; ok; cp "$root/stdout" "$root/list-all"
run -L; ok; cmp "$root/stdout" "$root/list-all"
[[ $(requests) == 0 ]]
run -C "$root/unrelated" -X; bad
kill -0 "$first_pid"; kill -0 "$second_pid"
# Canonical-directory derivation works through symlinks and does not need a
# matching conversation store or provider credentials in the control client.
workspace_id=$(printf '%s' "$work" | sha256sum | cut -c 1-32)
[[ -S "$runtime_root/ws-$workspace_id.sock" ]]
cp "$runtime_root/ws-$workspace_id.info" "$root/stale.info"
rm "$runtime_root/ws-$workspace_id.info"
run -C "$root/alias" --state-dir "$root/control-only" -X; ok
contains "$root/stdout" 'Workspace server stopped.'
[[ ! -e "$runtime_root/ws-$workspace_id.sock" ]]
wait "$first_pid"; first_pid=
kill -0 "$second_pid"
[[ ! -d "$root/control-only/pouch" && $(requests) == 0 ]]
cp "$root/stale.info" "$runtime_root/ws-$workspace_id.info"
chmod 600 "$runtime_root/ws-$workspace_id.info"
# Prepare a crash-left socket inode, with no listening process. Bash owns all
# execution/assertions; Python only constructs this small filesystem fixture.
"${CAI_TEST_PYTHON:-python3}" -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.close()' "$runtime_root/ws-$workspace_id.sock"
chmod 600 "$runtime_root/ws-$workspace_id.sock"
run --terminate-headless --all; ok
contains "$root/stdout" 'Workspace servers stopped: 1.'
[[ -S "$runtime_root/ws-$workspace_id.sock" && ! -e "$runtime_root/ws-$workspace_id.info" ]]
wait "$second_pid"; second_pid=
run -X --all; ok
contains "$root/stdout" 'Workspace servers stopped: 0.'
run --runtime-dir "$root/not-created" -X --all; ok
contains "$root/stdout" 'Workspace servers stopped: 0.'
# Enumeration must continue while owners remove their registry records.
: >"$root/first.log"; : >"$root/second.log"
"${base[@]}" -x >"$root/first.out" 2>"$root/first.log" & first_pid=$!
wait_for has "$root/first.log" 'Workspace server ready'
"${base[@]}" -C "$root/other" -x >"$root/second.out" 2>"$root/second.log" & second_pid=$!
wait_for has "$root/second.log" 'Workspace server ready'
run -X --all; ok
contains "$root/stdout" 'Workspace servers stopped: 2.'
wait "$first_pid"; first_pid=
wait "$second_pid"; second_pid=
# Corrupt hints are reported without preventing other owners from stopping.
printf 'invalid' >"$runtime_root/ws-00000000000000000000000000000000.info"
chmod 600 "$runtime_root/ws-00000000000000000000000000000000.info"
: >"$root/first.log"
"${base[@]}" -x >"$root/first.out" 2>"$root/first.log" & first_pid=$!
wait_for has "$root/first.log" 'Workspace server ready'
run -X --all; bad
contains "$root/stdout" 'Workspace servers stopped: 1.'
contains "$root/stderr" 'some workspace servers could not be stopped'
wait "$first_pid"; first_pid=
rm "$runtime_root/ws-00000000000000000000000000000000.info"
run -X; bad
[[ $(requests) == 0 ]]
# Automatic owners are real -x subprocesses; the lifetime/watch regressions
# separately cover last-client exit. -X must also work with a live controller.
terminal_start -N
run -X; ok
wait_for test -s "$term/status"
wait "$terminal_pid" || :
terminal_pid=
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
run -X --all; ok
contains "$root/stdout" 'Workspace servers stopped: 0.'
run --all; bad
run -X -x; bad
run -X -i forbidden; bad
# Socket shutdown cancels active foreground commands and settles storage; it
# must not merely drop the requesting connection or start another model call.
mkfifo "$work/command-gate"
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"trap \"printf handled > command-stopped; exit 0\" INT TERM; printf started > command-started; read line < command-gate"}},"MUST_NOT_BE_REQUESTED"]}'
: >"$root/first.log"
"${base[@]}" -x --terminal --yolo --foreground -i 'Start cancellable command' >"$root/first.out" 2>"$root/first.log" & first_pid=$!
wait_for test -s "$work/command-started"
run -X; ok
wait "$first_pid"; first_pid=
[[ $(cat "$work/command-stopped") == handled && $(requests) == 1 ]]
