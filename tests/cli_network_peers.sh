# Helpers for network scenarios with independently attached terminal frontends.
# Source after cli_test_common.sh; all processes belong to this one test.
declare -A network_owners= pane_dirs= pane_pids= pane_inputs= pane_events=
network_initialize() {
  runtime_root=$(mktemp -d "$support/../build/net.XXXXXX")
  base+=(--runtime-dir "$runtime_root" --network-tools)
  owner_base=("${base[@]}")
  export LOG_LEVEL=info
  trap network_cleanup EXIT
}
network_cleanup() {
  local result=$? key pid
  for key in "${!pane_pids[@]}"; do
    pid=${pane_pids[$key]}
    if [[ $pid != "${terminal_pid:-}" ]]; then
      kill -TERM "$pid" 2>/dev/null || :
      wait "$pid" 2>/dev/null || :
    fi
  done
  for key in "${!network_owners[@]}"; do
    pid=${network_owners[$key]}
    kill -TERM "$pid" 2>/dev/null || :
    wait "$pid" 2>/dev/null || :
  done
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
network_owner_start() {
  local name=$1 directory=$2
  shift 2
  : >"$root/$name.log"
  "${owner_base[@]}" -C "$directory" -x --workspace-name "network-$name" "$@" \
    </dev/null >"$root/$name.out" 2>"$root/$name.log" &
  network_owners[$name]=$!
  wait_for has "$root/$name.log" 'Workspace server ready'
}
pane_start() {
  local name=$1 input_fd
  shift
  terminal_start "$@"
  exec {input_fd}>&7
  pane_dirs[$name]=$term
  pane_pids[$name]=$terminal_pid
  pane_inputs[$name]=$input_fd
  pane_events[$name]=$terminal_event_fd
}
pane_use() {
  local name=$1
  term=${pane_dirs[$name]}
  terminal_pid=${pane_pids[$name]}
  terminal_event_fd=${pane_events[$name]}
  terminal_events=1
  exec 7>&${pane_inputs[$name]}
  send_offset=$(wc -c <"$term/text")
  send_feed_offset=$(wc -c <"$term/feed")
}
pane_close() {
  local name=$1 input_fd=${pane_inputs[$1]}
  pane_use "$name"
  terminal_close
  exec {input_fd}>&-
  unset 'pane_pids[$name]' 'pane_inputs[$name]' 'pane_events[$name]'
}
