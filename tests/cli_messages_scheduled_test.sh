#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start

# The standalone process and its storage client close before the receiver
# starts. The native visibility deadline must survive reopening the root.
run -q delayed-cli -M 'SCHEDULED FROM CLI' --message-delay-seconds 2 \
  --message-ack-timeout-ms 1
ok
jq -e '.status == "scheduled" and .recipient == "delayed-cli" and
  (.message_id | length > 0) and (.not_visible_until_unix | type == "number")' \
  "$root/stdout" >/dev/null
due=$(jq -r .not_visible_until_unix "$root/stdout")
[[ $(requests) == 0 ]]
terminal_start -N --messaging --message-queue delayed-cli
expect 'approval'
[[ $(date +%s) -ge $due ]]
send '\033a'; expect 'Alt-Y approve'; contains "$term/text" 'SCHEDULED FROM CLI'
[[ $(requests) == 0 ]]
terminal_close

# The tool overrides a configured delay, succeeds while the recipient is
# offline despite a 1 ms ACK timeout, and exits before delivery. No receipt
# listener from that sender is needed when the recipient later opens.
plan '{"plan":[{"tool":"agent_message","arguments":{"operation":"send","recipient":"delayed-tool","text":"SCHEDULED FROM TOOL","delay_seconds":2}},"SCHEDULE ACCEPTED"]}'
run -Nni 'schedule the message' --messaging --message-queue scheduler \
  --message-delay-seconds 60 --message-ack-timeout-ms 1
ok
contains "$root/stdout" 'SCHEDULE ACCEPTED'
[[ $(requests) == 2 ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
jq -e '[.input[]? | select(.type == "function_call_output") | .output | fromjson] |
  last | .status == "scheduled" and .recipient == "delayed-tool" and
  (.not_visible_until_unix | type == "number")' "$root/request" >/dev/null
due=$(jq -r '[.input[]? | select(.type == "function_call_output") | .output | fromjson] | last | .not_visible_until_unix' "$root/request")
terminal_start -N --messaging --message-queue delayed-tool
expect 'approval'
[[ $(date +%s) -ge $due ]]
send '\033a'; expect 'Alt-Y approve'; contains "$term/text" 'SCHEDULED FROM TOOL'
[[ $(requests) == 2 ]]
terminal_close
