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
export CAI_TEST_SERVER_RELEASE="$root/peer/release"
all_modes='server_lifetime server_abrupt server_persist server_grace server_active server_busy server_input server_wire server_fence server_typed server_config_persist server_override server_commands server_capture server_observer'
for mode in ${1:-$all_modes}; do
  export CAI_TEST_CONTROLLER_MODE=$mode
  before=$(requests)
  plan '{"plan":["Background work finished"]}'
  if [[ $mode == server_active ]]; then
    plan '{"plan":[{"pause":true,"text":"Do not complete"}]}'
  fi
  if [[ $mode == server_commands ]]; then
    printf 'boundary fixture\n' >"$work/boundary.txt"
    report=$(jq -c . "$support/fixtures/review.json")
    plan "$(jq -nc --arg report "$report" '{plan:[{pause:true,tool:"read_file",arguments:{path:"boundary.txt"}},"Background work finished",$report]}')"
  fi
  if [[ $mode == server_config_persist || $mode == server_override ]]; then
    run -N --persist; ok
  else
    run -N; ok
  fi
  [[ ! -s "$root/stdout" ]]
  if [[ $mode == server_commands ]]; then
    [[ $(requests) == $((before+3)) ]]
    tail -3 "$root/peer/requests.jsonl" | jq -es '.[1].reasoning.effort == "low"' >/dev/null
    ! rg -F 'RECALL ONLY' "$root/peer/requests.jsonl"
  elif [[ $mode == server_persist || $mode == server_config_persist || $mode == server_input || $mode == server_wire || $mode == server_typed || $mode == server_capture ]]; then
    [[ $(requests) == $((before+1)) ]]
    if [[ $mode == server_capture ]]; then
      tail -1 "$root/peer/requests.jsonl" >"$root/capture-request"
      contains "$root/capture-request" CAPTURE_TAIL
      absent "$root/capture-request" MUST_NOT_CAPTURE
    fi
  elif [[ $mode != server_active ]]; then
    [[ $(requests) == "$before" ]]
  fi
  [[ ! -f "$root/peer/requests.jsonl" ]] ||
    ! rg -q 'OBSERVER MUST NEVER RUN' "$root/peer/requests.jsonl"
done
