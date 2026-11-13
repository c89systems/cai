#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
watch=$(realpath "$1")
peer_start
runtime_root=$(mktemp -d "$support/../build/sp.XXXXXX")
persistent_owner=0
spawn_cleanup() {
  local result=$?
  if ((persistent_owner)); then
    "${base[@]}" --runtime-dir "$runtime_root" -X >"$root/cleanup.out" 2>"$root/cleanup.err" || :
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap spawn_cleanup EXIT
export CAI_TEST_SERVER_RUNTIME="$runtime_root" CAI_TEST_CONTROLLER_WORK="$work"
export CAI_TEST_SPAWN_REQUESTS="$root/peer/requests.jsonl"
for mode in fixtures temporary persistent half-close signal cancel disconnect invalid active; do
  export CAI_TEST_SPAWN_MODE=$mode
  args=("${base[@]:1}" --runtime-dir "$runtime_root")
  [[ $mode != persistent && $mode != half-close ]] || args+=(--persist)
  if [[ $mode == invalid ]]; then
    printf '[ui]\nreplay_turns=-2\n' >"$root/invalid.ini"
    args+=(--config "$root/invalid.ini")
  fi
  if [[ $mode == active ]]; then
    plan '{"plan":[{"pause":true,"pause_text":"Pending work ","text":"Must not finish"}]}'
    args+=(-n -i 'Start work for cancellation')
  fi
  "$watch" "${args[@]}" >"$root/watch.out" 2>"$root/watch.err" || {
    cat "$root/watch.err" >&2
    fail "quiet workspace case failed: $mode"
  }
  [[ ! -s "$root/watch.out" && ! -s "$root/watch.err" ]]
  if [[ $mode == active ]]; then
    [[ $(requests) == 1 ]]
    contains "$root/peer/requests.jsonl" 'Admitted after readiness'
    absent "$root/peer/requests.jsonl" 'Start work for cancellation'
  else [[ $(requests) == 0 ]]; fi
done

# The launching frontend has actually exited before the second invocation.
# A persistent forked app must retain the same public server instance.
before=$(requests)
plan '{"plan":["PERSISTENT FIRST REPLY","REATTACHED AFTER CLIENT EXIT"]}'
persistent_owner=1
run --runtime-dir "$runtime_root" --persist -Nni 'Start a persistent workspace'; ok
contains "$root/stdout" 'PERSISTENT FIRST REPLY'
publication=$(find "$runtime_root" -name 'ws-*.info' -print -quit)
[[ -n "$publication" ]]
cp "$publication" "$root/persistent.info"
run --runtime-dir "$runtime_root" --persist -ni 'Continue after the launching client exited'; ok
contains "$root/stdout" 'REATTACHED AFTER CLIENT EXIT'
cmp "$publication" "$root/persistent.info"
[[ $(requests) == $((before+2)) ]]
run --runtime-dir "$runtime_root" -X; ok
persistent_owner=0
wait_for test ! -e "$publication"

# The removed private exec entry must be rejected as an ordinary unknown option.
for fd in 0 -1 +3 3x 9999999999999999999999999; do
  rc=0
  "$cli" -x --_workspace-owner "$fd" -- >"$root/stdout" 2>"$root/stderr" || rc=$?
  [[ $rc == 2 && ! -s "$root/stdout" ]]
  contains "$root/stderr" 'unknown option'
done
rc=0
"$cli" -x --_workspace-owner 3 missing-separator >"$root/stdout" 2>"$root/stderr" || rc=$?
[[ $rc == 2 ]]
# No placement can select a private owner entry.
run --_workspace-owner 0 --
[[ $rc == 2 ]]
contains "$root/stderr" 'unknown option'
