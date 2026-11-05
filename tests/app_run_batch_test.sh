#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/rub.XXXXXX")
batch_cleanup() {
  local result=$?
  [[ $result != 0 ]] || rm -rf -- "$runtime_root"
  cleanup "$result"
}
trap batch_cleanup EXIT
export CAI_RUN_ENV=host CAI_RUN_REPORT="$root/report.json"
printf 'config_env_has_precedence = true\n[env]\nCAI_RUN_ENV = configured\n' >"$root/run.ini"
base+=(--config "$root/run.ini" --runtime-dir "$runtime_root")

# The shared native composition must keep host callbacks on the calling
# default-stack owner, preserve cwd/environment/locale/signal/input flags and
# open exactly one SDK client. No terminal or executable bootstrap is needed.
plan '{"plan":[{"tool":"host_fact","arguments":{}},"NATIVE BATCH REPLY"]}'
run -Nni 'use the native host tool in batch'; ok
[[ $(cat "$root/stdout") == 'NATIVE BATCH REPLY' && $(requests) == 2 ]]
jq -e '.calls == 1 and .ready == 1 and .observations > 1' "$CAI_RUN_REPORT" >/dev/null
[[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]
plan '{"plan":["RESTORED NATIVE BATCH REPLY"]}'
run -ni 'continue the native batch conversation'; ok
[[ $(cat "$root/stdout") == 'RESTORED NATIVE BATCH REPLY' && $(requests) == 3 ]]
contains "$root/peer/requests.jsonl" 'NATIVE BATCH REPLY'

# Temporary batch output stays machine-readable. No UI cleanup question may
# consume noninteractive input; --force removes the complete private tree.
plan '{"plan":["TEMP NATIVE BATCH REPLY"]}'
run --temp --force -ni 'temporary native batch'; ok
[[ $(cat "$root/stdout") == 'TEMP NATIVE BATCH REPLY' && $(requests) == 4 ]]
[[ -z $(find "$root/cache/cai/temp" -name private -o -name workspace) ]]
plan '{"plan":["KEPT TEMP NATIVE BATCH REPLY"]}'
run --temp -ni 'keep temporary native batch' </dev/null; ok
[[ $(cat "$root/stdout") == 'KEPT TEMP NATIVE BATCH REPLY' && $(requests) == 5 ]]
kept=$(jq -sr '[.[] | select(.event == "temp_workspace_kept") | .workspace] | last' "$root/stderr")
[[ -d "$kept" && ! -d "${kept%/workspace}/private" ]]
absent "$root/stdout" '[y/N]'

# Failure before READY wakes the adapter and tears down its owned endpoint.
export CAI_RUN_FAIL_START=1
run -Nni 'MUST NOT REACH INFERENCE'; bad
[[ ! -s "$root/stdout" && $(requests) == 5 ]]
[[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]
