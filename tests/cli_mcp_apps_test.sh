#!/usr/bin/env bash
# Real MCP transport + ChatGPT credentials; inference remains on the fixture.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
width=240
base+=(--mcp)
origin=${endpoint%/v1}
mkdir -p "$HOME/.codex"
token='e30.eyJleHAiOjQxMDI0NDQ4MDB9.fixture'
jq -n --arg token "$token" '{auth_mode:"chatgpt",tokens:{id_token:$token,access_token:$token,refresh_token:"fixture",account_id:"apps-account"}}' >"$HOME/.codex/auth.json"
cat >"$root/apps.ini" <<INI
[client]
api_protocol = $protocol
[mcp.chatgpt]
endpoint = "$origin/mcp/chatgpt"
[mcp.extra]
lazy = false
endpoint = "$origin/mcp/extra"
INI
base+=(--config "$root/apps.ini")
cp "$root/apps.ini" "$root/original.ini"
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
apps_plan() {
  local mcp_spec='{}'
  [[ $# == 1 ]] || mcp_spec=$2
  local json
  json=$(jq -n --argjson chat "$chat" "{chat:\$chat,plan:$1,mcp:({chatgpt:{notification_status:204}} * $mcp_spec)}") || return
  plan "$json"
}
names() { jq -r '.tools[]? | .name // .function.name // .type' "$1"; }
query() { send "$1\r"; expect 'Use /mcp help'; }
update() { send "$1\r"; expect 'MCP connections updated.'; }
apps_plan '["APP TURN COMPLETE"]' '{chatgpt:{gate:"mcp-ready"}}'
terminal_start -N
initial=$(current_session_id)
# Discovery can be held indefinitely without blocking the editor or queries.
wait_for test -s "$root/peer/mcp_requests.jsonl"
query '/mcp'
contains "$term/feed" chatgpt
contains "$term/feed" extra
send 'MCP discovery keeps the draft editable'; expect_prompt 'MCP discovery keeps the draft editable'
send '\025'
touch "$root/peer/mcp-ready"
query '/mcp chatgpt'
contains "$term/feed" "$origin/mcp/chatgpt"
before=$(requests)
update '/mcp disable chatgpt'
[[ $(requests) == "$before" ]]
query '/mcp chatgpt'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/connection"
contains "$root/connection" disabled
send '/tools\r'; expect 'MCP servers'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/tools"
absent "$root/tools" mcp_7_chatgpt_echo
contains "$root/tools" mcp_5_extra_echo
apps_plan '[{tool:"mcp_5_extra_echo",arguments:{text:"independent connection"}},"EXTRA WORKS"]'
prompt 'use the other connection' 'EXTRA WORKS'
tail -2 "$root/peer/requests.jsonl" | head -1 >"$root/request"
names "$root/request" >"$root/names"
absent "$root/names" mcp_7_chatgpt_echo
contains "$root/names" mcp_5_extra_echo
update '/mcp enable chatgpt'
apps_plan '[]' '{chatgpt:{tools:[{name:"fresh",description:"Refreshed app tool",inputSchema:{type:"object",properties:{text:{type:"string"}},required:["text"],additionalProperties:false}}]}}'
update '/mcp refresh chatgpt'
send '/tools\r'; expect mcp_7_chatgpt_fresh
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/tools"
absent "$root/tools" mcp_7_chatgpt_echo
apps_plan '[{tool:"tool_search",arguments:{query:"chatgpt fresh",limit:1}},{tool:"mcp_7_chatgpt_fresh",arguments:{text:"ChatGPT app through another inference provider"}},"APP WORKS"]' '{chatgpt:{tools:[{name:"fresh",inputSchema:{type:"object",properties:{text:{type:"string"}},required:["text"],additionalProperties:false}}]}}'
prompt 'use the app' 'APP WORKS'
jq -se --arg token "Bearer $token" 'any(.[]; .server == "chatgpt" and .request.method == "tools/call") and all(.[]; .server != "chatgpt" or (.authorization == $token and .account_id == "apps-account" and .product_sku == "codex" and .originator == "cai"))' "$root/peer/mcp_requests.jsonl" >/dev/null
jq -se 'all(.[]; .Authorization == "Bearer fixture" and .["ChatGPT-Account-Id"] == null)' "$root/peer/headers.jsonl" >/dev/null
absent "$term/feed" "$token"
absent "$term/stderr" "$token"
# Alt-Enter remains editable while streaming; the change applies after the
# current response/tool batch and before the very next model request.
apps_plan '[{pause:true,pause_text:"McpBoundaryWait\n\n",tool:"get_goal",arguments:{}},"McpBoundaryDone"]'
send 'wait while the connection is changed\r'; expect McpBoundaryWait
send '/mcp disable chatgpt\033\r'; expect 'S 1. /mcp disable chatgpt'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/pending"
absent "$root/pending" '$ /mcp disable chatgpt'
release
expect 'MCP connections updated.'
expect 'McpBoundaryDone'
expect 'Worked for'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
names "$root/request" >"$root/names"
absent "$root/names" mcp_7_chatgpt_fresh
contains "$root/names" mcp_5_extra_echo
# Provider names are not restricted to model tool-name syntax. Distinct remote
# punctuation must never collapse into the same alias; calls retain exact names.
apps_plan '[]' '{chatgpt:{tools:[{name:"mail.search",inputSchema:{type:"object"}},{name:"mail/search",inputSchema:{type:"object"}},{name:("long" * 25),inputSchema:{type:"object"}}]}}'
update '/mcp enable chatgpt'
update '/mcp refresh chatgpt'
apps_plan '[{tool:"tool_search",arguments:{query:"chatgpt",limit:8}},"ALIAS CATALOG"]'
prompt 'inspect alias catalog' 'ALIAS CATALOG'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
alias=$(names "$root/request" | awk '/^mcp_h/ {print; exit}')
[[ -n "$alias" ]]
names "$root/request" | jq -Rse 'split("\n") | map(select(startswith("mcp_h"))) | length == 3 and (unique | length == 3)' >/dev/null
apps_plan "[{tool:\"$alias\",arguments:{}},\"EXACT REMOTE NAME\"]"
prompt 'call the first app alias' 'EXACT REMOTE NAME'
jq -se 'any(.[]; .server == "chatgpt" and .request.method == "tools/call" and .request.params.name == "mail.search")' "$root/peer/mcp_requests.jsonl" >/dev/null
update '/mcp disable chatgpt'
send '/new\r'; expect 'New session'
query '/mcp chatgpt'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/connection"
contains "$root/connection" disabled
send '/resume 2\r'; expect 'Conversation resumed'
send '/status\r'; expect "$initial"
query '/mcp chatgpt'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/connection"
contains "$root/connection" disabled
send '/mcp enable nonexistent\r'; expect 'No configured MCP connection'
# A failed refresh removes that connection's tools without stopping inference.
apps_plan '[]' '{extra:{malformed:true}}'
update '/mcp refresh extra'
apps_plan '["INFERENCE STILL WORKS"]'
prompt 'continue without app access' 'INFERENCE STILL WORKS'
terminal_close
cmp "$root/apps.ini" "$root/original.ini"
# Shutdown cancels idle discovery too; no inference or user intervention is
# needed to release a connection whose server is still holding its response.
before=$(wc -l <"$root/peer/mcp_requests.jsonl")
apps_plan '[]' '{extra:{gate:"mcp-stop"}}'
terminal_start -N
discovery_held() {
  [[ $(wc -l <"$root/peer/mcp_requests.jsonl") -gt "$before" ]] &&
    jq -se 'last.server == "extra"' "$root/peer/mcp_requests.jsonl" >/dev/null
}
wait_for discovery_held
terminal_close
cmp "$root/apps.ini" "$root/original.ini"
before=$(wc -l <"$root/peer/mcp_requests.jsonl")
apps_plan '["MCP OFF"]'
run --no-mcp -Nni 'all connections disabled'; ok
[[ $(stdout_text) == 'MCP OFF' ]]
[[ $(wc -l <"$root/peer/mcp_requests.jsonl") == "$before" ]]
# Absent/logged-out ChatGPT credentials do not break another inference provider.
rm "$HOME/.codex/auth.json"
apps_plan '["NO APP AUTH"]'
XDG_STATE_HOME="$root/no-auth-state" run -Nni 'no app authentication'; ok
[[ $(stdout_text) == 'NO APP AUTH' ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
names "$root/request" >"$root/names"
absent "$root/names" mcp_7_chatgpt_echo
cmp "$root/apps.ini" "$root/original.ini"
