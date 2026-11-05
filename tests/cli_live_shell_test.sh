#!/usr/bin/env bash
# Exercise live pipe output through the public socket frontend, not just tails.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
controller=$(realpath "$1")
runtime_root=$(mktemp -d "$support/../build/lsh.XXXXXX")
no_publication() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
live_cleanup() {
  local result=$?
  if ! no_publication; then
    "$controller" "$runtime_root" "$work" stop >"$root/stop.out" 2>"$root/stop.err" || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap live_cleanup EXIT
base+=(--runtime-dir "$runtime_root")
terminal_start -N --shell-context-max-bytes 4096
send '!printf LIVE_; printf EARLY; while [ ! -f output-gate ]; do sleep .01; done; printf LIVE_; printf LATE\r'
expect LIVE_EARLY
absent "$term/feed" LIVE_LATE
send '/status\r'; expect_table 1 'Session ID'
[[ $(requests) == 0 ]]
touch "$work/output-gate"
expect 'Shell command exited 0'
[[ $(grep -o LIVE_EARLY "$term/feed" | wc -l) == 1 ]]
[[ $(grep -o LIVE_LATE "$term/feed" | wc -l) == 1 ]]
send '!printf "C1_LIVE: \\302"; sleep .05; printf "\\233 END_C1\\n"\r'
expect 'Shell command exited 0'
contains "$term/feed" 'C1_LIVE: \xc2\x9b END_C1'
send '!printf LIVE_; printf CANCEL; sleep 60\r'; expect LIVE_CANCEL
send '\003'; expect 'Shell command exited 137'
send '!printf CANCEL_BURST_; printf READY; awk "BEGIN {for (i=0;i<20000;i++) printf \"b\"}"; sleep 60\r'
expect CANCEL_BURST_READY
send '\003'; expect 'Shell command exited 137'
send '!kill -TERM $$\r'; expect 'Shell command exited 143'
send '/status\r'; expect_table 1 'Session ID'
cat >"$work/large-output.sh" <<'LARGE_OUTPUT'
printf LIVE_DISCARDED_PREFIX
awk 'BEGIN {for (i=0;i<70000;i++) printf "x"}'
printf LIVE_RETAINED_SUFFIX
LARGE_OUTPUT
send '!sh large-output.sh\r'
expect 'Shell command exited 0'
contains "$term/feed" LIVE_RETAINED_SUFFIX
plan '{"plan":["LIVECONTEXTOK"]}'
prompt 'use the retained shell tail' LIVECONTEXTOK
contains "$root/peer/requests.jsonl" LIVE_RETAINED_SUFFIX
absent "$root/peer/requests.jsonl" LIVE_DISCARDED_PREFIX
terminal_close
wait_for no_publication
run --list-all; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
run --export "$id"; ok
journal="$root/data/cai/exports/$id/$id.jsonl"
# Only the retained tail survives; raw progress has no journal records.
absent "$journal" LIVE_DISCARDED_PREFIX
! jq -se 'any(.[]; .type == "shell_output" or .type == "user_shell_output_hex")' "$journal" >/dev/null
logs
! jq -se 'any(.[]; .event == "user_shell_output_tail" and (.msg | contains("LIVE_DISCARDED_PREFIX")))' "$root/logs.jsonl" >/dev/null
terminal_start --replay-turns -1
expect LIVE_RETAINED_SUFFIX
send '/status\r'; expect_table 1 'Session ID'
[[ $(requests) == 1 ]]
terminal_close
wait_for no_publication
