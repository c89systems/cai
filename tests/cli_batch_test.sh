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

# Goals finish from durable turn/goal boundaries, including a goal created by
# a tool during an ordinary accepted prompt. Continuations need no new prompt
# receipt and must never cause a second runtime to open the same collection.
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
