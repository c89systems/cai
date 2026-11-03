#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
base+=(--mcp)
width=240
origin=${endpoint%/v1}
cat >"$root/lazy.ini" <<INI
[client]
api_protocol = $protocol
[mcp.lazy]
endpoint = "$origin/mcp/lazy"
description = "Workspace utilities"
[mcp.eager]
endpoint = "$origin/mcp/eager"
lazy = false
INI
base+=(--config "$root/lazy.ini")
cp "$root/lazy.ini" "$root/original.ini"
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
catalog=$(jq -n '[{name:"searchInvoices",description:"Find invoice messages",inputSchema:{type:"object",properties:{invoiceNumber:{type:"string"}}},_meta:{connector_display_name:"Gmail",connectorDescription:"Read and send mail"}}, {name:"sendMail",description:"Send a message",inputSchema:{type:"object"},_meta:{connector_name:"Gmail",connector_description:"Read and send mail"}}, {name:"rawText",description:"RawGate accepts text",inputSchema:{type:"object",properties:{payload:{type:"string"}},required:["payload"],additionalProperties:false},_meta:{"systems.c89.cai/raw-input":{field:"payload",framing:"text"},connector_name:"Notes"}}] + [range(0;100) | {name:("readFile_" + tostring),description:"Read a document",inputSchema:{type:"object"},_meta:{connector_name:"Drive",connector_description:"Find and read files"}}]')
lazy_plan() {
  local steps json
  steps=$(jq -n "$1") || return
  json=$(jq -n --argjson chat "$chat" --argjson tools "$catalog" --argjson steps "$steps" '{chat:$chat,mcp:{lazy:{tools:$tools}},plan:$steps}') || return
  plan "$json"
}
names() { jq -r '.tools[]? | .name // .function.name // .type' "$1"; }
request_at() { sed -n "$1"p "$root/peer/requests.jsonl" >"$root/request"; names "$root/request" >"$root/names"; }
lazy_plan '[{tool:"tool_search",arguments:{query:"Gmail invoice",limit:1}},{tool:"mcp_4_lazy_searchInvoices",arguments:{invoiceNumber:"123"}},"LAZY TURN DONE"]'
terminal_start -N
discovered() { logs; jq -se 'any(.[]; .event == "mcp_updated")' "$root/logs.jsonl" >/dev/null; }
wait_for discovered
catalog_reads=$(jq -s '[.[] | select(.server == "lazy" and .request.method == "tools/list")] | length' "$root/peer/mcp_requests.jsonl")
prompt 'find the invoice through Gmail' 'LAZY TURN DONE'
request_at 1
contains "$root/names" tool_search
contains "$root/names" mcp_5_eager_echo
absent "$root/names" mcp_4_lazy_
jq -e 'any(.tools[]; (.name // .function.name) == "tool_search" and ((.description // .function.description) | contains("Gmail: Read and send mail") and contains("Drive: Find and read files")))' "$root/request" >/dev/null
request_at 2
jq -e '[.. | strings | fromjson? | objects | .tools? | arrays | .[] | .source?] | index("Gmail") != null' "$root/request" >/dev/null
contains "$root/names" mcp_4_lazy_searchInvoices
absent "$root/names" mcp_4_lazy_sendMail
absent "$root/names" mcp_4_lazy_readFile
contains "$term/feed" 'Discovered MCP tools'
contains "$term/feed" 'Loaded 1 MCP tool(s)'
absent "$term/feed" '"input_schema"'
[[ $(jq -s '[.[] | select(.server == "lazy" and .request.method == "tools/list")] | length' "$root/peer/mcp_requests.jsonl") == "$catalog_reads" ]]
jq -se 'any(.[]; .server == "lazy" and .request.method == "tools/call" and .request.params.name == "searchInvoices")' "$root/peer/mcp_requests.jsonl" >/dev/null
send '/tools\r'; expect 'MCP servers'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/inventory"
contains "$root/inventory" 'json (deferred)'
send '/mcp lazy\r'; expect 'Use /mcp help'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/connection"
contains "$root/connection" '(lazy)'
# No match is a successful discovery result, without arbitrary fallback tools.
start=$(requests)
lazy_plan '[{tool:"tool_search",arguments:{query:"xyzzynomatch"}},"NO MATCH DONE"]'
prompt 'try an unavailable service' 'NO MATCH DONE'
request_at $((start+2))
absent "$root/names" mcp_4_lazy_readFile
contains "$term/feed" 'No matching MCP tools'
# Default search size is eight; previously selected tools remain available.
start=$(requests)
lazy_plan '[{tool:"tool_search",arguments:{query:"Drive files"}},"DEFAULT LIMIT DONE"]'
prompt 'discover file tools' 'DEFAULT LIMIT DONE'
request_at $((start+2))
[[ $(grep -c mcp_4_lazy_readFile "$root/names") == 8 ]]
contains "$root/names" mcp_4_lazy_searchInvoices
lazy_plan '[]'
send '/mcp refresh lazy\r'; expect 'MCP connections updated.'
start=$(requests)
lazy_plan '["REFRESH PRESERVES SELECTION"]'
prompt 'check the refreshed catalog' 'REFRESH PRESERVES SELECTION'
request_at $((start+1))
contains "$root/names" mcp_4_lazy_searchInvoices
[[ $(grep -c mcp_4_lazy_readFile "$root/names") == 8 ]]
# Loading preserves each protocol's input contract, including custom raw tools.
start=$(requests)
raw_argument=$(jq -cn --argjson chat "$chat" 'if $chat then {payload:"raw discovery text"} else "raw discovery text" end')
lazy_plan "[{tool:\"tool_search\",arguments:{query:\"RawGate\",limit:1}},{tool:\"mcp_4_lazy_rawText\",arguments:$raw_argument},\"RAW DISCOVERY DONE\"]"
prompt 'discover and use the text tool' 'RAW DISCOVERY DONE'
request_at $((start+2))
jq -e --argjson chat "$chat" 'any(.tools[]; if $chat then .type == "function" and .function.name == "mcp_4_lazy_rawText" else .type == "custom" and .name == "mcp_4_lazy_rawText" end)' "$root/request" >/dev/null
jq -se 'any(.[]; .server == "lazy" and .request.method == "tools/call" and .request.params.name == "rawText" and .request.params.arguments.payload == "raw discovery text")' "$root/peer/mcp_requests.jsonl" >/dev/null
send '/mcp disable lazy\r'; expect 'MCP connections updated.'
start=$(requests)
lazy_plan '["DISABLED DONE"]'
prompt 'continue with eager tools' 'DISABLED DONE'
request_at $((start+1))
absent "$root/names" tool_search
absent "$root/names" mcp_4_lazy_
contains "$root/names" mcp_5_eager_echo
send '/mcp enable lazy\r'; expect 'MCP connections updated.'
start=$(requests)
lazy_plan '[{tool:"tool_search",arguments:{query:"mail",limit:0}},"INVALID SEARCH RECOVERED"]'
prompt 'exercise invalid search arguments' 'INVALID SEARCH RECOVERED'
request_at $((start+2))
absent "$root/names" mcp_4_lazy_
contains "$term/feed" 'limit 1..32'
terminal_close
cmp "$root/lazy.ini" "$root/original.ini"
