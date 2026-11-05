#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
client=$(realpath "$1")
peer_start
runtime_root=$(mktemp -d "$support/../build/bat.XXXXXX")
base+=(--runtime-dir "$runtime_root")
owner_pid= holder_pid= batch_pid=
batch_cleanup() {
  local result=$?
  for child in "$batch_pid" "$holder_pid" "$owner_pid"; do
    if [[ -n "$child" ]]; then
      kill -TERM "$child" 2>/dev/null || true
      wait "$child" 2>/dev/null || true
    fi
  done
  [[ $result != 0 ]] || rm -rf -- "$runtime_root"
  cleanup "$result"
}
trap batch_cleanup EXIT
export LOG_LEVEL=info
plan '{"plan":["FIRST OWNER REPLY","FINAL OWNER REPLY"]}'
"${base[@]}" -x -N --workspace-name batch-fixture </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
run -s batch-fixture -ni first -i second --service-tier flex -r low; ok
[[ $(cat "$root/stdout") == 'FINAL OWNER REPLY' && $(requests) == 2 ]]
contains "$root/stderr" 'FIRST OWNER REPLY'
jq -se 'all(.[]; .service_tier == "flex" and .reasoning.effort == "low")' "$root/peer/requests.jsonl" >/dev/null
original=$(current_session_id)
[[ -n "$original" ]]
kill -0 "$owner_pid"

# Context-changing controls must complete in the new conversation before the
# dependent prompt is admitted. Neither old output nor a second runtime wins.
plan '{"plan":["NEW CONVERSATION REPLY"]}'
run -s batch-fixture -Nni 'new conversation'; ok
[[ $(cat "$root/stdout") == 'NEW CONVERSATION REPLY' && $(requests) == 3 ]]
plan '{"plan":["RESUMED CONVERSATION REPLY"]}'
run -s batch-fixture -R "$original" -ni 'resumed conversation'; ok
[[ $(cat "$root/stdout") == 'RESUMED CONVERSATION REPLY' && $(requests) == 4 ]]
contains "$root/peer/requests.jsonl" 'FIRST OWNER REPLY'

# An accepted command receipt is not its application outcome.
run -s batch-fixture --service-tier 'unknown tier' -ni 'MUST NOT RUN'; bad
[[ ! -s "$root/stdout" && $(requests) == 4 ]]
contains "$root/stderr" 'startup command failed'
absent "$root/peer/requests.jsonl" 'MUST NOT RUN'

# A new batch attachment never steals a live controller's authority.
"$client" "$runtime_root" "$work" hold >"$root/holder.out" 2>"$root/holder.err" & holder_pid=$!
wait_for test -s "$root/holder.out"
run -s batch-fixture -ni 'OBSERVER MUST NOT RUN'; bad
[[ ! -s "$root/stdout" && $(requests) == 4 ]]
contains "$root/stderr" 'controller authority'
kill -TERM "$holder_pid"; wait "$holder_pid" || true; holder_pid=

# Queries and state changes use the same admitted command path as the TUI.
# Their documents are selected by command identity, without an inference call.
before=$(requests)
run -s batch-fixture -ni '/reasoning high' -i '/status'; ok
contains "$root/stdout" 'Reasoning effort'
contains "$root/stdout" high
[[ $(requests) == "$before" ]]
run -s batch-fixture -ni '/not-a-command' -i 'MUST NOT FOLLOW COMMAND ERROR'; bad
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
run -s batch-fixture -ni '/help -p' -i 'MUST NOT FOLLOW FRONTEND ACTION'; bad
contains "$root/stderr" 'interactive terminal attachment'
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]

# Direct reviews attach to this owner. Launch acknowledgement is insufficient:
# wait for validation and handoff, retaining the exact JSON response or its
# journaled Markdown projection without opening another runtime in the client.
report=$(jq -c . "$support/fixtures/review.json")
printf 'review input\n' >"$work/tracked.txt"
for mode in markdown json file; do
  plan "$(jq -n --arg report "$report" '{plan:[{text:"REVIEW PREAMBLE MUST NOT WIN",tool:"read_file",arguments:{path:"tracked.txt"}},$report]}')"
  flags=()
  case "$mode" in json) flags+=(-T json) ;; file) flags+=(-o findings.md) ;; esac
  run -s batch-fixture --review -i 'Inspect the fixture' "${flags[@]}"; ok
  case "$mode" in
    json) jq -Se . "$root/stdout" >"$root/actual"; jq -Se . "$support/fixtures/review.json" >"$root/expected"; cmp "$root/actual" "$root/expected" ;;
    file) [[ ! -s "$root/stdout" ]]; contains "$work/findings.md" '# Review findings' ;;
    markdown) contains "$root/stdout" '# Review findings'; contains "$root/stdout" 'Priority: P1' ;;
  esac
  absent "$root/stdout" 'REVIEW PREAMBLE MUST NOT WIN'
  [[ $(current_session_id) == "$original" ]]
  kill -0 "$owner_pid"
done
plan "$(jq -n --arg report "$report" '{plan:[$report]}')"
terminal_launch -s batch-fixture --review -i 'Review from a real terminal'
wait_for test -s "$term/status"
wait "$terminal_pid"; terminal_pid=
[[ $(cat "$term/status") == 0 ]]
contains "$term/text" '# Review findings'
absent "$term/text" 'Conversation attached.'
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
printf 'PRESERVE THIS FILE\n' >"$work/findings.md"
plan '{"plan":["not a JSON review report"]}'
run -s batch-fixture --review -i 'Invalid review fixture' -o findings.md; bad
[[ ! -s "$root/stdout" && $(cat "$work/findings.md") == 'PRESERVE THIS FILE' ]]
contains "$root/stderr" 'required JSON report schema'

# Paths are not clipped to a presentation stack buffer; control bytes from
# a report cannot become terminal control sequences in Markdown output.
long_path="/$(printf '%0700d' 0)/parser.c"
safe_report=$(jq -c --arg path "$long_path" '.findings[0].code_location.absolute_file_path=$path | .findings[0].body="before\u001bafter\u009bcontrol"' "$support/fixtures/review.json")
plan "$(jq -n --arg report "$safe_report" '{plan:[$report]}')"
run -s batch-fixture --review -i 'Control and long path fixture'; ok
contains "$root/stdout" "$long_path:4-6"
contains "$root/stdout" 'before?after?control'

# Goals finish from durable turn/goal boundaries, including a goal created by
# a tool during an ordinary accepted prompt. Continuations need no new prompt
# receipt and must never cause a second runtime to open the same collection.
before=$(requests)
goal_text=$'Combined "goal"\nwith \\ escapes and €'
first_text=$'Initial "prompt"\nwith \\ escapes and €'
plan '{"plan":["INITIAL COMBINED REPLY","__complete_goal__","COMBINED GOAL FINISHED","SECOND INSTRUCTION FINISHED"]}'
run -s batch-fixture -ng "$goal_text" -i "$first_text" -i 'second instruction after achievement'; ok
[[ $(cat "$root/stdout") == 'SECOND INSTRUCTION FINISHED' && $(requests) == $((before+4)) ]]
sed -n "$((before+1))p" "$root/peer/requests.jsonl" >"$root/combined-first"
jq -e --arg prompt "$first_text" '([.input[]? | select(.role == "user") | .content[]? | .text?] | last == $prompt) and any(.input[]?; .role == "developer" and any(.content[]?; (.text? // "") | contains("Combined")))' "$root/combined-first" >/dev/null
contains "$root/stderr" 'COMBINED GOAL FINISHED'
before=$(requests)
plan '{"plan":[{"tool":"update_goal","arguments":{"status":"paused"}},"COMBINED NEEDS ANSWER"]}'
run -s batch-fixture -ng 'Pause the combined goal' -i 'initial question' -i 'MUST NOT FOLLOW PAUSE'; bad
[[ $(cat "$root/stdout") == 'COMBINED NEEDS ANSWER' && $(requests) == $((before+2)) ]]
absent "$root/peer/requests.jsonl" 'MUST NOT FOLLOW PAUSE'
before=$(requests)
run -s batch-fixture -ni '/goal start {"objective":"a","objective":"b","prompt":"MUST NOT RUN DUPLICATE"}' -i 'MUST NOT FOLLOW REJECTION'; bad
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
run -s batch-fixture -ni '/goal start {"objective":"a","prompt":"bad\u0000suffix"}'; bad
[[ ! -s "$root/stdout" && $(requests) == "$before" ]]
before=$(requests)
plan '{"plan":["GOAL FIRST TURN","__complete_goal__","GOAL FINAL TURN"]}'
run -s batch-fixture -ng 'finish the attached goal'; ok
[[ $(cat "$root/stdout") == 'GOAL FINAL TURN' && $(requests) == $((before+3)) ]]
contains "$root/stderr" 'GOAL FIRST TURN'
before=$(requests)
plan '{"plan":[{"tool":"create_goal","arguments":{"objective":"finish the model-created batch goal"}},"CREATED GOAL FIRST TURN","__complete_goal__","CREATED GOAL FINAL TURN"]}'
run -s batch-fixture -ni 'create and pursue a goal'; ok
[[ $(cat "$root/stdout") == 'CREATED GOAL FINAL TURN' && $(requests) == $((before+4)) ]]
contains "$root/stderr" 'CREATED GOAL FIRST TURN'
before=$(requests)
plan '{"plan":[{"tool":"update_goal","arguments":{"status":"paused"}},"GOAL NEEDS AN ANSWER"]}'
run -s batch-fixture -ng 'pause for an answer'; bad
[[ $(cat "$root/stdout") == 'GOAL NEEDS AN ANSWER' && $(requests) == $((before+2)) ]]
before=$(requests)

# Disconnect after admission means unknown outcome, not success or a retry.
plan '{"plan":[{"pause":true,"pause_text":"BATCH PARTIAL BEFORE LOSS ","text":"UNREACHABLE FINAL"}]}'
"${base[@]}" -s batch-fixture -ni 'disconnect this accepted batch' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for has "$root/batch.err" 'BATCH PARTIAL BEFORE LOSS'
kill -TERM "$owner_pid"; wait "$owner_pid"; owner_pid=
result=0; wait "$batch_pid" || result=$?; batch_pid=
[[ $result != 0 && ! -s "$root/batch.out" && $(requests) == $((before+1)) ]]
contains "$root/batch.err" 'outcome unknown'
[[ ! -s "$root/holder.err" ]]

# A persistent owner may already be pursuing an unowned goal when the batch
# attaches. Replacing it at a request boundary must join that turn safely;
# waiting for a second turn.started can otherwise wait forever.
before=$(requests)
plan '{"plan":[{"pause":true,"pause_text":"BUSY BACKGROUND GOAL ","text":"OLD GOAL RESPONSE"},"__complete_goal__","REPLACED BACKGROUND GOAL DONE"]}'
"${base[@]}" -x -N -g 'Existing background goal' --workspace-name batch-fixture </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'BUSY BACKGROUND GOAL'
"$cli" -p custom --endpoint "$endpoint" --no-terminal --no-image-generation --no-mcp --insecure -C "$work" --runtime-dir "$runtime_root" -s batch-fixture -ng 'Replacement background goal' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for has "$root/batch.err" '/goal new Replacement background goal'
release
wait "$batch_pid"; batch_pid=
[[ $(cat "$root/batch.out") == 'REPLACED BACKGROUND GOAL DONE' && $(requests) == $((before+3)) ]]
kill -TERM "$owner_pid"; wait "$owner_pid"; owner_pid=

# Explicit headless startup uses the same core action without an attachment.
before=$(requests)
plan '{"plan":["HEADLESS INITIAL REPLY","__complete_goal__","HEADLESS COMBINED DONE"]}'
"${base[@]}" -x -N -g 'Headless combined objective' -i 'Headless initial prompt' --workspace-name batch-fixture </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'HEADLESS COMBINED DONE'
[[ $(requests) == $((before+3)) ]]
sed -n "$((before+1))p" "$root/peer/requests.jsonl" >"$root/headless-first"
jq -e '[.input[]? | select(.role == "user") | .content[]? | .text?] | last == "Headless initial prompt"' "$root/headless-first" >/dev/null
kill -TERM "$owner_pid"; wait "$owner_pid"; owner_pid=
