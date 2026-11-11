#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
network_fixture=$1
peer_start
port=$("$network_fixture" port)
base+=(--network-tools)
terminal_start -N --prompt-listener "$port" --event-multicast --ip-multicast-if 127.0.0.1
expect 'Workspace event multicast: 239.193.89.'
expect 'socat -u'
run --list; ok
contains "$root/stdout" "127.0.0.1:$port"
contains "$root/stdout" 'multicast_publisher'
plan '{"plan":[{"tool":"prompt_listener_info","arguments":{}},"LISTENER INFO DONE"]}'
prompt 'find my reply endpoint' 'LISTENER INFO DONE'
expect 'Read prompt listener'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
jq -e '[.. | strings | fromjson? | select(type == "object" and .status == "available")] | any(.reply_tool.name == "prompt_send" and .attachment_id > 0 and .server_instance != "")' "$root/request" >/dev/null
plan '{"plan":[{"tool":"network_list","arguments":{}},"NETWORK LIST DONE"]}'
prompt 'list network endpoints' 'NETWORK LIST DONE'
expect 'Listed network endpoints'
plan "$(jq -nc --arg address "127.0.0.1:$port" '{plan:[{tool:"prompt_send",arguments:{address:$address,prompt:"TCP TOOL PROMPT",include_reply:true}},"SENDER DONE","TCP REMOTE ACK"]}')"
prompt 'send a prompt to this frontend' 'SENDER DONE'
expect 'Transmitted'
expect 'TCP REMOTE ACK'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'TCP TOOL PROMPT'
contains "$root/request" 'Reply requested: use prompt_send'
plan '{"plan":[{"tool":"event_stream_listen","arguments":{"address":"239.193.89.223:49189","interface":"127.0.0.1","duration_ms":10}},"OBSERVATION DONE"]}'
prompt 'observe an event stream briefly' 'OBSERVATION DONE'
expect 'Observed multicast events'
terminal_close
# Public registrations and claimed automatic groups are withdrawn on close.
withdrawn() {
  run --list
  [[ $rc == 0 ]] && ! grep -F 'multicast_publisher' "$root/stdout" >/dev/null &&
    ! grep -F "127.0.0.1:$port" "$root/stdout" >/dev/null
}
# A detached owner can finish cleanup just after its last frontend exits.
wait_for withdrawn
