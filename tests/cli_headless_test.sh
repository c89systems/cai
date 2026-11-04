#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
client=$(realpath "$1")
peer_start
runtime_root=$(mktemp -d "$support/../build/hd.XXXXXX")
server_pid=
headless_cleanup() {
  local result=$?
  if [[ -n "$server_pid" ]]; then
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  if [[ $result != 0 ]]; then
    tail -50 "$root/server.log" >&2 || true
  else
    rm -rf -- "$runtime_root"
  fi
  cleanup "$result"
}
trap headless_cleanup EXIT
export LOG_LEVEL=info
start_server() {
  "${base[@]}" --runtime-dir "$runtime_root" "$@" </dev/null >"$root/server.out" 2>"$root/server.log" & server_pid=$!
  wait_for has "$root/server.log" 'Workspace server ready'
  [[ ! -s "$root/server.out" ]]
  jq -se 'any(.[]; .event == "workspace_ready" and .persistent == true)' "$root/server.log" >/dev/null
}
stop_server() {
  wait "$server_pid"
  server_pid=
  [[ ! -s "$root/server.out" ]]
  jq -se 'any(.[]; .event == "workspace_stopped" and .exit_code == 0)' "$root/server.log" >/dev/null
}
inspect() { "$client" "$runtime_root" "$work" inspect; }

# Real foreground executable, closed stdin and no terminal. Readiness is
# structured logging; no banner, editor, tool receipt or Markdown reaches stdout.
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"kill -s TERM $$; exit 0","tty":false}},"HEADLESS_ANSWER"]}'
start_server -x -N --terminal --yolo
id=$(inspect)
[[ -n "$id" && $(requests) == 0 ]]
"$client" "$runtime_root" "$work" abrupt >"$root/client"
[[ $(inspect) == "$id" ]]
if [[ -r /proc/$server_pid/stat ]]; then
  idle_before=$(awk '{print $14+$15}' "/proc/$server_pid/stat")
  ticks=$(getconf CLK_TCK)
  sleep .25
  idle_after=$(awk '{print $14+$15}' "/proc/$server_pid/stat")
  printf 'headless idle CPU: %s clock ticks over 250 ms (CLK_TCK=%s)\n' "$((idle_after-idle_before))" "$ticks"
  [[ $((idle_after-idle_before)) -le $((ticks/20+1)) ]]
fi

# A second owner fails at endpoint ownership and leaves the live owner's
# session and configuration intact. No provider call is needed for discovery.
if "${base[@]}" --runtime-dir "$runtime_root" --headless -N </dev/null >"$root/duplicate.out" 2>"$root/duplicate.log"; then
  fail 'duplicate workspace server was accepted'
fi
[[ ! -s "$root/duplicate.out" && $(inspect) == "$id" && $(requests) == 0 ]]
jq -se 'any(.[]; .lvl == "error")' "$root/duplicate.log" >/dev/null

# A public client submits one model input and receives its authenticated live
# transcript. Its process aborts if it attempts to open an SDK storage client.
[[ $("$client" "$runtime_root" "$work" work) == "$id" ]]
[[ $(requests) == 2 && $(inspect) == "$id" ]]
jq -se 'any(.[] | .input[]? | select(.type == "function_call_output") | .output | fromjson; .signal == 15)' "$root/peer/requests.jsonl" >/dev/null
"$client" "$runtime_root" "$work" shutdown
stop_server

# The same journal resumes after every supported process stop signal. Last
# client departure never stops -x; only explicit shutdown/signals do.
for signal in TERM INT HUP QUIT; do
  start_server --headless
  [[ $(inspect) == "$id" && $(requests) == 2 ]]
  kill -s "$signal" "$server_pid"
  stop_server
  jq -se 'any(.[]; .event == "workspace_stopped" and .signal > 0)' "$root/server.log" >/dev/null
done

# Invalid startup and incompatible utility/batch modes do not leave an owner
# or perform the utility. Help/version remain ordinary output-only invocations.
printf '[ui]\nreplay_turns=-2\n' >"$root/invalid.ini"
run --headless --runtime-dir "$runtime_root" --config "$root/invalid.ini"
[[ "$rc" == 2 ]]
absent "$root/stderr" 'Workspace server ready'
for args in '--headless --list' '-x -n -i task'; do
  read -r -a invalid <<<"$args"
  run "${invalid[@]}"; [[ "$rc" == 2 ]]
  absent "$root/stderr" 'Workspace server ready'
done
run -x --help; ok
contains "$root/stdout" '--headless'
run --headless --version; ok
[[ ! -s "$root/stderr" ]]

# A command blocked inside a foreground tool must receive cancellation. Its
# INT/TERM trap proves delivery before teardown; SIGKILL would not create this
# marker. The server must not sample the model again after cancellation.
mkfifo "$work/command-gate"
plan '{"plan":[{"tool":"exec_command","arguments":{"cmd":"trap \"printf handled > command-stopped; exit 0\" INT TERM; printf started > command-started; read line < command-gate"}},"MUST_NOT_BE_REQUESTED"]}'
start_server --headless --terminal --yolo --foreground -i 'Start cancellable command'
wait_for test -s "$work/command-started"
kill -TERM "$server_pid"
stop_server
[[ $(cat "$work/command-stopped") == handled && $(requests) == 3 ]]

# Cancellation also reaches an owner with in-flight inference; it does not
# wait for a provider fixture to finish its response.
plan '{"plan":[{"pause":true,"pause_text":"Pending response ","text":"Must not finish"}]}'
start_server --headless -i 'Start cancellable work'
wait_for test -f "$root/peer/requests.jsonl"
wait_for bash -c '[[ $(wc -l <"$1") == 4 ]]' _ "$root/peer/requests.jsonl"
kill -TERM "$server_pid"
stop_server
