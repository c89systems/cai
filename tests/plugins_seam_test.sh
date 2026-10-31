#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
probe=$1
protocol=$2
peer_start
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{tool:"replace_me",arguments:{}},{tool:"replace_me",arguments:{}},"SEAMFINISHED"]}')"
"$probe" "$endpoint" "$protocol"
jq -se '.[0] | any(.tools[]; (.description // .function.description) == "OLD_SCHEMA")' "$root/peer/requests.jsonl" >/dev/null
jq -se '.[1] | any(.tools[]; (.description // .function.description) == "NEW_SCHEMA")' "$root/peer/requests.jsonl" >/dev/null
contains "$root/peer/requests.jsonl" OLD_HANDLER
contains "$root/peer/requests.jsonl" NEW_HANDLER
jq -se '.[2].tools[] | select((.name // .function.name) == "reload_tools") | (.description // .function.description) | contains("late_executor")' "$root/peer/requests.jsonl" >/dev/null
