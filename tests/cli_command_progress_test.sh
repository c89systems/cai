#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_host=$(realpath "$1")
mode=${2:-native}
owner=$(realpath "$3")
runtime_root=$(mktemp -d "$support/../build/progress.XXXXXX")
owner_pid=
progress_cleanup() {
  local result=$?
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap progress_cleanup EXIT
export LOG_LEVEL=info
printf '[client]\ntimeout_ms = 20000\n' >"$root/progress.ini"
base+=(--runtime-dir "$runtime_root" --config "$root/progress.ini")
"$owner" "${base[@]:1}" -x -N >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
if [[ $mode == embed ]]; then
  base=("$terminal_host" "$runtime_root" "$work" "${base[@]:1}")
fi
terminal_start
catalog_started() {
  [[ -f "$root/peer/gets" ]] &&
    [[ $(wc -l <"$root/peer/gets") -gt $previous_gets ]]
}
queue_settled() {
  [[ -f "$term/message" ]] &&
    ! grep -E 'Q [0-9]+\..*/model|P [1-9][0-9]* pending' "$term/message" >/dev/null
}
for attempt in 1 2; do
  previous_gets=0
  if [[ -f "$root/peer/gets" ]]; then previous_gets=$(wc -l <"$root/peer/gets"); fi
  rm -f "$root/peer/catalog-release" "$root/peer/catalog_finished"
  plan '{"catalog_gate":"catalog-release","catalog":{"models":[{"slug":"command-progress-model"}]},"plan":[]}'
  send '/model\r'
  wait_for catalog_started
  # The owner is blocked in a real provider operation. Echo must precede its
  # result and completion; no frontend echo or fabricated receipt is allowed.
  expect '$ /model'
  wait_for queue_settled
  [[ ! -e "$root/peer/catalog_finished" ]]
  send 'DRAFT while catalog is held'
  expect_prompt 'DRAFT while catalog is held'
  [[ ! -e "$root/peer/catalog_finished" ]]
  touch "$root/peer/catalog-release"
  expect command-progress-model
  wait_for test -s "$root/peer/catalog_finished"
  send '\025/status\r'; expect 'Session ID'
done
[[ $(without_dtg "$term/feed" | grep -c '^\$ /model$') == 2 ]]
[[ $(requests) == 0 ]]
terminal_close
kill -0 "$owner_pid"
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=
