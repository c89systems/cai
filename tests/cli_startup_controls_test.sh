#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_host=$(realpath "$1")
mode=${2:-native}
runtime_root=$(mktemp -d "$support/../build/controls.XXXXXX")
base+=(--runtime-dir "$runtime_root")
owner_pid=
controls_cleanup() {
  local result=$?
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap controls_cleanup EXIT
export LOG_LEVEL=info
plan '{"plan":["FIRST SETTINGS APPLIED"]}'
"${base[@]}" -x -N --todo >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
if [[ $mode == embed ]]; then
  base=("$terminal_host" "$runtime_root" "$work" "${base[@]:1}")
fi
# The native and library frontend use exactly the same queue and controls.
# Instruction submission must wait for every generated control's execution.
terminal_start --service-tier flex --reasoning-summary concise -r low \
  --yield-time-ms 17 --yolo --no-reasoning-tool --shell-context \
  --normal-reasoning-effort low --high-reasoning-effort xhigh \
  --autoqueue --board 'startup board' --todo-layout list \
  --prompt-history-limit 3 --conversation-retention-days 0 \
  -i 'FIRST INVOCATION'
expect 'FIRST SETTINGS APPLIED'
jq -se 'length == 1 and .[0].service_tier == "flex" and
  .[0].reasoning.effort == "low" and .[0].reasoning.summary == "concise"' \
  "$root/peer/requests.jsonl" >/dev/null
send '\033[A\033[A'; expect_prompt 'FIRST INVOCATION'
send '\025'
send '/yield_time_ms\r'; expect '17 ms'
send '/yolo\r'; expect 'on (unrestricted)'
send '/history\r'; expect '3 entries'
send '/retention\r'; expect 'forever'
send '/shell-context\r'; expect 'will be shared'
send '/reasoning-tool\r'; expect 'low'; expect 'xhigh'
send '/todo\r'; expect 'startup board'
send '/todo layout\r'; expect 'list'
# A new public summary command is discoverable and validates rather than
# killing the frontend. Queries never cause inference or reset selection.
send '/reasoning-summary invalid\r'; expect 'use /reasoning-summary'
send '/reasoning-summary\r'; expect 'Reasoning summary: concise.'
terminal_close
kill -0 "$owner_pid"

# Explicit default/false/zero values must undo the first attachment's changes.
plan '{"plan":["SECOND SETTINGS APPLIED"]}'
terminal_start --service-tier default --reasoning-summary detailed \
  --foreground --no-yolo --no-shell-context --prompt-history-limit 0 \
  --no-autoqueue --board 'second board' --todo-layout board \
  --conversation-retention-days 30 -i 'SECOND INVOCATION'
expect 'SECOND SETTINGS APPLIED'
jq -se 'length == 2 and .[1].service_tier == "default" and
  .[1].reasoning.summary == "detailed"' "$root/peer/requests.jsonl" >/dev/null
send '/yield_time_ms\r'; expect 'Foreground commands'
send '/yolo\r'; expect 'off (restricted)'
send '/history\r'; expect '0 entries'
send '/retention\r'; expect '30 days'
send '/shell-context\r'; expect 'stay out of model'
send '/todo\r'; expect 'second board'
send '/todo layout\r'; expect 'board'
terminal_close

# No explicit control flags: the running owner's settings survive reattachment.
# Catch-up must display prior controls without reapplying them or resubmitting
# either startup instruction.
export CAI_BOARD=environment-board CAI_UI_TODO_LAYOUT=list
terminal_start
send '/todo\r'; expect 'second board'
send '/todo layout\r'; expect 'list'
[[ $(requests) == 2 ]]
terminal_close
unset CAI_BOARD CAI_UI_TODO_LAYOUT
terminal_start
send '/todo\r'; expect 'second board'
send '/todo layout\r'; expect 'list'
send '/yield_time_ms\r'; expect 'Foreground commands'
send '/reasoning-summary\r'; expect 'Reasoning summary: detailed.'
[[ $(requests) == 2 ]]
send '/background\r'; expect '10000 ms'
send '/reasoning-summary none\r'; expect 'Reasoning summary changed to none.'
# A summary change during an in-flight response waits for its request boundary.
# Its accepted command cannot change the request that is already on the wire.
printf 'SUMMARY BOUNDARY TOOL\n' >"$work/summary.txt"
plan '{"plan":[{"pause":true,"tool":"read_file","arguments":{"path":"summary.txt"}},"SUMMARY BOUNDARY DONE"]}'
send 'change summaries during this turn\r'; expect 'STREAM in flight'
send '/reasoning-summary concise\033\r'; expect '/reasoning-summary concise'
release
expect 'SUMMARY BOUNDARY DONE'
jq -se 'length == 4 and (.[2].reasoning | has("summary") | not) and
  .[3].reasoning.summary == "concise"' "$root/peer/requests.jsonl" >/dev/null
terminal_close

# Application rejection stops the remaining startup sequence: never run a
# prompt under settings different from the caller's explicit request.
terminal_start --service-tier 'unknown tier' -i 'MUST NOT RUN'
expect 'Usage: /tier'
send '/status\r'; expect 'Session ID'
[[ $(requests) == 4 ]]
terminal_close
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=
