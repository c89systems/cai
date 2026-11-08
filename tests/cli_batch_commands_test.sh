#!/usr/bin/env bash
# Accepted command receipts are not completion of their review/goal work.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
scope=${2:-review}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
git init --quiet --initial-branch=fixture "$work"
printf 'before\n' >"$work/fixture.txt"
git -C "$work" add fixture.txt
git -C "$work" -c user.name=Fixture -c user.email=fixture@example.test -c commit.gpgSign=false commit -qm 'test(batch): seed fixture'
printf 'after\n' >"$work/fixture.txt"
separators=(' ' '  ' $'\t' $' \t \t' $' \n\t')
case "$scope" in
review)
  report=$(jq -c . "$support/fixtures/review.json")
  for separator in "${separators[@]}"; do
    plan "$(jq -nc --argjson chat "$chat" --arg report "$report" '{chat:$chat,plan:[$report]}')"
    before=$(requests)
    run -Nni "/review${separator}custom inspect fixture"; ok
    contains "$root/stdout" '# Review findings'
    contains "$root/stdout" 'Fix parser'
    [[ $(requests) == $((before+1)) ]]
  done
  # Invalid child output must fail rather than returning its launch receipt.
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["not a review report"]}')"
  before=$(requests)
  run -Nni $'/review\tcustom inspect fixture'; bad
  [[ ! -s "$root/stdout" && $(requests) == $((before+1)) ]]
  contains "$root/stderr" schema
  ;;
goal)
  for separator in "${separators[@]}"; do
    plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["INITIAL GOAL TURN","__complete_goal__","FINAL GOAL TURN"]}')"
    before=$(requests)
    run -Nni "/goal${separator}new${separator}finish fixture"; ok
    [[ $(cat "$root/stdout") == 'FINAL GOAL TURN' && $(requests) == $((before+3)) ]]
    contains "$root/stderr" 'INITIAL GOAL TURN'
  done
  # Structured goal start retains its distinct initial-prompt contract.
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["INITIAL START TURN","__complete_goal__","FINAL START TURN"]}')"
  before=$(requests)
  run -Nni $'/goal \t start\t{"objective":"finish fixture","prompt":"EXACT INITIAL PROMPT"}\t '; ok
  [[ $(cat "$root/stdout") == 'FINAL START TURN' && $(requests) == $((before+3)) ]]
  sed -n "$((before+1))p" "$root/peer/requests.jsonl" >"$root/initial-request"
  contains "$root/initial-request" 'EXACT INITIAL PROMPT'
  # Resume launches pursuit too; its acknowledgement cannot finish the batch.
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[{tool:"update_goal",arguments:{status:"paused"}},"GOAL PAUSED"]}')"
  run -Nni '/goal new pause fixture'; bad
  [[ $(cat "$root/stdout") == 'GOAL PAUSED' ]]
  paused=$(current_session_id)
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["__complete_goal__","RESUMED GOAL FINISHED"]}')"
  before=$(requests)
  run --resume "$paused" -ni $'/goal \t resume\t '; ok
  [[ $(cat "$root/stdout") == 'RESUMED GOAL FINISHED' && $(requests) == $((before+2)) ]]
  # Read-only forms still finish without another provider request.
  before=$(requests)
  run -ni $'/goal\tstatus\t '; ok
  contains "$root/stdout" 'Status: complete'
  run -ni $'/goal  help\t '; ok
  contains "$root/stdout" 'Goal commands'
  [[ $(requests) == "$before" ]]
  # Review-and-fix is an explicitly requested goal, not a plain review.
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["__complete_goal__","FIX GOAL FINISHED"]}')"
  before=$(requests)
  run -Nni $'/review-and-fix\tuncommitted\t '; ok
  [[ $(cat "$root/stdout") == 'FIX GOAL FINISHED' && $(requests) == $((before+2)) ]]
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[{error:"GOAL PROVIDER FAILED"}]}')"
  before=$(requests)
  run -Nni $'/goal\tnew\tfinish fixture' -i 'MUST NOT FOLLOW FAILURE'; bad
  contains "$root/stderr" 'GOAL PROVIDER FAILED'
  [[ $(requests) == $((before+1)) ]]
  absent "$root/peer/requests.jsonl" 'MUST NOT FOLLOW FAILURE'
  ;;
*) fail 'Unknown batch command scope' ;;
esac
# Token prefixes and incomplete goals are rejected promptly, never pursued.
before=$(requests)
for command in '/reviewer custom inspect fixture' '/review-and-fix-extra uncommitted' $'/goal\tnewspaper finish fixture' $'/goal\tnew\t ' $'/goal\tstart\t{}'; do
  run -Nni "$command"; bad
  [[ $(requests) == "$before" ]]
done
