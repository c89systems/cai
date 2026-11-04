#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/ws.XXXXXX")
server_cleanup() {
  local status=$?
  if [[ $status == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$status"
}
trap server_cleanup EXIT
# Keep runtime paths short even when the binary's build-directory name is long.
export LOG_LEVEL=warn CAI_TEST_SERVER_RUNTIME="$runtime_root" CAI_TEST_CONTROLLER_WORK="$work"
for mode in server_lifetime server_abrupt server_persist server_grace server_active server_busy server_input server_wire server_fence; do
  export CAI_TEST_CONTROLLER_MODE=$mode
  before=$(requests)
  plan '{"plan":["Background work finished"]}'
  if [[ $mode == server_active ]]; then
    plan '{"plan":[{"pause":true,"text":"Do not complete"}]}'
  fi
  run -N; ok
  [[ ! -s "$root/stdout" ]]
  if [[ $mode == server_persist || $mode == server_input || $mode == server_wire ]]; then
    [[ $(requests) == $((before+1)) ]]
  elif [[ $mode != server_active ]]; then
    [[ $(requests) == "$before" ]]
  fi
  [[ ! -f "$root/peer/requests.jsonl" ]] ||
    ! rg -q 'OBSERVER MUST NEVER RUN' "$root/peer/requests.jsonl"
done
