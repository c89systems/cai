#!/usr/bin/env bash
# Commands use the same native editable queue as session prompts.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
command_plan() { plan "$(jq -n --argjson chat "$chat" "{chat:\$chat,plan:$1}")"; }

# Idle commands now pass through the same visible admission path. Edit in the
# native input burst before delivery, including commands that would otherwise
# start a review, change sessions, hand off the terminal or mutate files.
terminal_start -N
before=$(requests)
for command in '/review uncommitted' '/model must-not-select low' \
  '/goal new Must not create' '/new' '/shell' '!touch idle-must-not-exist'; do
  send "$command\r\033e"; expect_prompt "$command"
  absent "$term/feed" "\$ $command"
  send '\025/output compact\r'; expect 'applies from the next tool call'
done
[[ $(requests) == "$before" && ! -e "$work/idle-must-not-exist" ]]
terminal_close

# Escape pauses the old backlog, not newly submitted managed commands. A fresh
# /goal resume must start pursuit without resubmitting or releasing old input.
command_plan '[{pause:true,pause_text:"PolicyGoalBegin\n\n",text:"Cancelled"}]'
terminal_start -N
send '/goal new Native policy resume objective\r'; expect PolicyGoalBegin
send 'STOPPED_ORDINARY_PROMPT\r'; expect 'Q 1. STOPPED_ORDINARY_PROMPT'
send '/goal new STOPPED_GOAL_COMMAND\r'; expect 'Q 2. /goal new STOPPED_GOAL_COMMAND'
send '\033'; expect 'WARN: Turn stopped.'
release
before=$(requests)
send '/status\r'; expect 'Use /tools for the full inventory.'
send '/goal\r'; expect 'Status: paused'
[[ $(requests) == "$before" ]]
command_plan '[{tool:"update_goal",arguments:{status:"complete"}},"PolicyGoalResumed"]'
send '/goal resume\r'; expect PolicyGoalResumed; expect 'goal=achieved'
[[ $(requests) == $((before+2)) ]]
absent "$root/peer/requests.jsonl" STOPPED_ORDINARY_PROMPT
absent "$root/peer/requests.jsonl" STOPPED_GOAL_COMMAND
# Read-only commands and goal pursuit leave old identities editable and stopped.
send '\033e'; expect_prompt '/goal new STOPPED_GOAL_COMMAND'
send '\025/output full\r'; expect 'Q 2. /output full'
absent "$term/feed" '$ /output full'
send '/status\r'; expect 'Use /tools for the full inventory.'
send '\033\r'; expect 'applies from the next tool call'
expect 'Q 1. STOPPED_ORDINARY_PROMPT'
# Context changes leave accepted, stopped entries in their own session.
# Switching back restores the queue rather than moving it into the new one.
original=$(current_session_id)
send '/new\r'; expect 'ID:'
send '/status\r'; expect 'Metric'
absent "$term/message" STOPPED_ORDINARY_PROMPT
send '/resume 2\r'; expect 'Session resumed.'; expect 'Q 1. STOPPED_ORDINARY_PROMPT'
absent "$term/feed" '> STOPPED_ORDINARY_PROMPT'
send "/export $root/pending.jsonl\r"; expect "$root/pending.jsonl"
# The command echo acknowledges submission, not asynchronous export completion.
# The final file appears only after its complete contents have been renamed.
wait_for test -s "$root/pending.jsonl"
terminal_close
[[ $(requests) == $((before+2)) ]]
terminal_start --resume "$original"
expect 'Q 1. STOPPED_ORDINARY_PROMPT'
terminal_close
# A portable export seeds the current queue once. Historic queue deltas must
# neither resurrect consumed commands nor misidentify local delivery receipts
# as foreign child receipts when the session receives a new ID.
run --import "$root/pending.jsonl"; ok
imported=$(stdout_text)
[[ -n "$imported" && "$imported" != "$original" ]]
terminal_start --resume "$imported"
expect 'Q 1. STOPPED_ORDINARY_PROMPT'
send '/status\r'; expect 'Metric'
[[ $(requests) == $((before+2)) ]]
terminal_close
# A headless editor has no idle/watch callbacks. EOF must still drain every
# accepted prompt through the same owner queue and preserve their order.
before_pipe=$(requests)
command_plan '["PipeOneDone", "PipeTwoDone"]'
printf 'PIPE_ONE_INPUT\nPIPE_TWO_INPUT\n' | "${base[@]}" -N >"$root/pipes.out" 2>"$root/pipes.err"
contains "$root/pipes.out" PipeOneDone
contains "$root/pipes.out" PipeTwoDone
[[ $(requests) == $((before_pipe+2)) ]]
