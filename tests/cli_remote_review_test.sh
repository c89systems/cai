#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/rev.XXXXXX")
base+=(--runtime-dir "$runtime_root" --terminal)
owner=
review_cleanup() {
  local result=$?
  if [[ -n "$owner" ]]; then
    kill -TERM "$owner" 2>/dev/null || true
    wait "$owner" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap review_cleanup EXIT
export LOG_LEVEL=info
printf 'REVIEW_INPUT_CONTENT\n' >"$work/review-input.txt"
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -n --arg report "$report" '{plan:[
  {pause:true,pause_text:"",reasoning:["Review verification\n\nREVIEW TOOLS PROGRESS.\n"],tool:"list_files",arguments:{path:"."}},
  {tool:"read_file",arguments:{path:"review-input.txt"}},
  {tool:"exec_command",arguments:{cmd:"printf REVIEW_COMMAND_OUTPUT",tty:false}},
  $report]}')"
"${base[@]}" -x -N >"$root/owner.out" 2>"$root/owner.log" & owner=$!
wait_for has "$root/owner.log" 'Workspace server ready'
if [[ ${1:-native} == embed ]]; then
  base=("$(realpath "$2")" "$runtime_root" "$work" "${base[@]:1}")
fi
terminal_start
send '/review custom Verify tool access\r'
expect 'Review started'
expect 'REVIEW TOOLS PROGRESS'
release
expect 'Ran printf REVIEW_COMMAND_OUTPUT'
expect REVIEW_COMMAND_OUTPUT
expect 'Review finished'
expect 'Fix parser'
[[ $(grep -c '^  Review started' "$term/feed") == 1 ]]
[[ $(grep -c '^  Review finished' "$term/feed") == 1 ]]
[[ $(requests) == 4 ]]
jq -se 'all(.[];
  ([.tools[]? | .name // .function.name] |
    index("list_files") != null and index("read_file") != null and
    index("exec_command") != null and index("write_stdin") != null and
    index("apply_patch") == null and index("run_review") == null)) and
  any(.[]; (.input|tostring|contains("REVIEW_INPUT_CONTENT"))) and
  any(.[]; (.input|tostring|contains("REVIEW_COMMAND_OUTPUT")))' "$root/peer/requests.jsonl" >/dev/null
[[ $(cat "$work/review-input.txt") == REVIEW_INPUT_CONTENT ]]
terminal_close
# Both raw subprocess output and reviewer progress/report remain replayable.
terminal_start
expect 'Ran printf REVIEW_COMMAND_OUTPUT'
expect REVIEW_COMMAND_OUTPUT
expect 'REVIEW TOOLS PROGRESS'
expect 'Review finished'
terminal_close
[[ $(requests) == 4 ]]
