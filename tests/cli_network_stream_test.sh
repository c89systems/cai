#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
source "$support/cli_network_peers.sh"
peer_start
network_initialize
mkdir "$root/receiver"
# Use a test-specific group so the kernel membership is an observable readiness
# barrier. Publishers do not join; only the receiving tool adds this membership.
group="239.194.$((($$ / 250) % 250 + 1)).$(( $$ % 250 + 1 ))"
group_hex=$(printf '%02X%02XC2EF' "${group##*.}" "$((($$ / 250) % 250 + 1))")
[[ -r /proc/net/igmp ]] || fail 'Linux multicast membership observations unavailable'
if grep -F "$group_hex" /proc/net/igmp >/dev/null; then fail 'Test multicast group already joined'; fi
network_owner_start publisher "$work" -N --event-multicast "$group:49189" --ip-multicast-if 127.0.0.1
network_owner_start receiver "$root/receiver" -N
pane_start publisher -s network-publisher
pane_start receiver -C "$root/receiver" -s network-receiver
plan "$(jq -nc --arg address "$group:49189" '{plan:[{pause:true,pause_text:"PUBLICATION HELD\n\n",text:"HEADLESS_STREAM_MARKER"},{tool:"event_stream_listen",arguments:{address:$address,interface:"127.0.0.1",duration_ms:5000,max_bytes:65536}},"OBSERVED STREAM"]}')"
pane_use publisher
send 'publish after the frontend has disconnected\r'
expect 'PUBLICATION HELD'
pane_use receiver
send 'observe the publisher journal stream\r'
joined() { grep -F "$group_hex" /proc/net/igmp >/dev/null; }
wait_for joined
pane_close publisher
kill -0 "${network_owners[publisher]}"
release
pane_use receiver
expect 'OBSERVED STREAM'
expect 'Observed multicast events'
[[ $(requests) == 3 ]]
jq -se '[.[] | .. | objects | select(.type? == "function_call_output") | .output | fromjson?] | any(.external_data == true and .events > 0 and (.content | contains("HEADLESS_STREAM_MARKER")))' "$root/peer/requests.jsonl" >/dev/null
pane_close receiver
