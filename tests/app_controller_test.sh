#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export LOG_LEVEL=warn
for mode in run queue steer goal cancel stop_active startup_stop provider_error observer_error invalid invalid_config invalid_directory; do
  export CAI_TEST_CONTROLLER_MODE=$mode
  args=(-N)
  plan '{"plan":["First reply","Next reply","Final reply"]}'
  if [[ $mode == steer ]]; then
    printf 'fixture input\n' >"$work/tracked.txt"
    plan '{"plan":[{"tool":"read_file","arguments":{"path":"tracked.txt"}},"Steered reply"]}'
  elif [[ $mode == invalid ]]; then
    args=(--help)
  elif [[ $mode == goal ]]; then
    plan '{"plan":["__complete_goal__","Goal completed"]}'
  elif [[ $mode == provider_error ]]; then
    plan '{"plan":[{"error":"provider exploded"},"Recovered reply"]}'
  elif [[ $mode == invalid_config ]]; then
    printf 'invalid INI\n' >"$root/bad.ini"
    args=(--config "$root/bad.ini")
  elif [[ $mode == invalid_directory ]]; then
    args=(-C "$work/missing")
  fi
  run "${args[@]}"; ok
  [[ ! -s "$root/stdout" ]]
  if [[ $mode == steer ]]; then contains "$root/peer/requests.jsonl" 'CONTROLLER STEERING'; fi
done
export CAI_TEST_CONTROLLER_MODE=replay_stop
before=$(requests)
run; ok
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
mkdir "$work/child"
export CAI_TEST_CONTROLLER_MODE=contexts CAI_TEST_CONTROLLER_WORK="$work"
run -N; ok
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
