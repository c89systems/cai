#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
network_fixture=$1
feeder=$2
source "$support/cli_network_peers.sh"
peer_start
network_initialize
mkdir "$root/other"
port_a=$("$network_fixture" port)
network_owner_start a "$work" -N --event-multicast --ip-multicast-if 127.0.0.1
network_owner_start b "$root/other" -N
pane_start a -s network-a --prompt-listener "$port_a"
expect "TCP prompt listener: 127.0.0.1:$port_a"
port_b=$("$network_fixture" port)
pane_start b -C "$root/other" -s network-b --prompt-listener "$port_b"
expect "TCP prompt listener: 127.0.0.1:$port_b"

# Two different frontends/backends exchange prompts and an explicit reply.
# Identical final responses avoid imposing ordering on the two inference calls.
plan "$(jq -nc --arg address "127.0.0.1:$port_b" '{plan:[{tool:"prompt_send",arguments:{address:$address,prompt:"CROSS_FRONTEND",include_reply:true}},"CROSS FLOW DONE","CROSS FLOW DONE"]}')"
pane_use a
prompt 'send to the other frontend' 'CROSS FLOW DONE'
expect 'Transmitted'
wait_for has "${pane_dirs[b]}/feed" 'CROSS FLOW DONE'
[[ $(requests) == 3 ]]
jq -se '[.[] | .. | objects | .text? | strings] | any(startswith("CROSS_FRONTEND\n\nReply requested:"))' "$root/peer/requests.jsonl" >/dev/null
plan "$(jq -nc --arg address "127.0.0.1:$port_a" '{plan:[{tool:"prompt_send",arguments:{address:$address,prompt:"REPLY_FROM_B"}},"REPLY FLOW DONE","REPLY FLOW DONE"]}')"
pane_use b
prompt 'reply to the first frontend' 'REPLY FLOW DONE'
wait_for has "${pane_dirs[a]}/feed" 'REPLY FLOW DONE'
[[ $(requests) == 6 ]]
jq -se 'any(.[] | .. | objects; .text? == "REPLY_FROM_B")' "$root/peer/requests.jsonl" >/dev/null

# Observers reject TCP input; takeover cancels a receipt already in progress.
port_o=$("$network_fixture" port)
pane_start observer -s network-a --prompt-listener "$port_o"
printf 'OBSERVER MUST REJECT' | "$feeder" --feed-tcp "$port_o" >"$root/observer-send.log" 2>&1 || :
expect 'cannot admit input'
[[ $(requests) == 6 ]]
pane_use a
exec 9<>"/dev/tcp/127.0.0.1/$port_a"
printf 'INTERRUPTED_TAKEOVER' >&9
expect_prompt 'INTERRUPTED_TAKEOVER'
pane_use observer
send '/takeover\r'
wait_for has "${pane_dirs[a]}/feed" 'TCP prompt discarded'
exec 9>&-
pane_use a
empty_prompt() { [[ $(cat "$term/prompt") == '>' ]]; }
wait_for empty_prompt
printf 'REVOKED MUST REJECT' | "$feeder" --feed-tcp "$port_a" >"$root/revoked-send.log" 2>&1 || :
expect 'cannot admit input'
[[ $(requests) == 6 ]]
pane_use observer
plan '{"plan":["NEW CONTROLLER ANSWER"]}'
printf 'AFTER_TAKEOVER' | "$feeder" --feed-tcp "$port_o" >"$root/takeover-send.log" 2>&1
expect 'NEW CONTROLLER ANSWER'
expect 'Worked for'
[[ $(requests) == 7 ]]

# A session switch invalidates a separate receipt without admitting its prefix.
send 'LOCAL DRAFT'
expect_prompt 'LOCAL DRAFT'
exec 9<>"/dev/tcp/127.0.0.1/$port_o"
printf 'INTERRUPTED_SESSION' >&9
receiving() { run --list; [[ $rc == 0 ]] && grep -F "127.0.0.1:$port_o" "$root/stdout" | grep -F unavailable >/dev/null; }
wait_for receiving
send '\025/new\r'
expect 'TCP prompt discarded'
expect 'Session resumed'
exec 9>&-
[[ $(requests) == 7 ]]
plan '{"plan":["NEW SESSION ANSWER"]}'
printf 'AFTER_SESSION_SWITCH' | "$feeder" --feed-tcp "$port_o" >"$root/session-send.log" 2>&1
expect 'NEW SESSION ANSWER'
expect 'Worked for'
[[ $(requests) == 8 ]]
tail -1 "$root/peer/requests.jsonl" >"$root/new-request"
absent "$root/new-request" INTERRUPTED_SESSION
absent "$root/new-request" CROSS_FRONTEND

# The listener remains with A when this terminal parks A and observes B.
send '/workspace network-b\r'
expect 'Observing workspace'
plan '{"plan":["PARKED A ANSWER"]}'
printf 'PARKED_PROMPT_A' | "$feeder" --feed-tcp "$port_o" >"$root/parked-send.log" 2>&1
wait_for has "$root/a.log" 'PARKED A ANSWER'
absent "$term/feed" 'PARKED A ANSWER'
send '/workspace network-a\r'
expect 'PARKED A ANSWER'
[[ $(requests) == 9 ]]
pane_close a
pane_close observer
pane_close b

# SIGKILL leaves leases/registry hints behind. Restart must recover the saved
# session and allocate another group rather than stealing the crashed claim.
old_group=$(jq -r 'select(.msg=="Workspace event multicast enabled") | .destination' "$root/a.log" | tail -1)
[[ -n $old_group ]]
kill -KILL "${network_owners[a]}"
wait "${network_owners[a]}" 2>/dev/null || :
unset 'network_owners[a]'
mv "$root/a.log" "$root/a.crashed.log"
network_owner_start a "$work" --event-multicast --ip-multicast-if 127.0.0.1
new_group=$(jq -r 'select(.msg=="Workspace event multicast enabled") | .destination' "$root/a.log" | tail -1)
[[ -n $new_group && $new_group != "$old_group" ]]
pane_start recovered -s network-a --prompt-listener "$port_a"
expect 'PARKED A ANSWER'
expect "Workspace event multicast: $new_group"
plan '{"plan":["RECOVERED NETWORK ANSWER"]}'
printf 'AFTER_CRASH' | "$feeder" --feed-tcp "$port_a" >"$root/recovered-send.log" 2>&1
expect 'RECOVERED NETWORK ANSWER'
pane_close recovered
[[ $(requests) == 10 ]]
