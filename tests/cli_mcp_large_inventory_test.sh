#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
base+=(--mcp)
width=240
origin=${endpoint%/v1}
cat >"$root/inventory.ini" <<INI
[client]
api_protocol = $protocol
[mcp.catalog]
endpoint = "$origin/mcp/catalog"
INI
base+=(--config "$root/inventory.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,mcp:{catalog:{tools:[range(0;320)|{name:("tool_"+tostring),description:("Catalog entry "+tostring+" "+("metadata "*80)+"完整 C1:\u009b DESCRIPTIONEND"+tostring),inputSchema:{type:"object"}}]}},plan:["AFTER LARGE INVENTORY"]}')"
terminal_start -N
discovery_count=0
discovered() { logs; jq -se --argjson before "$discovery_count" '[.[] | select(.event == "mcp_updated")] | length > $before' "$root/logs.jsonl" >/dev/null; }
wait_for discovered
send '/tools\r'; expect 'DESCRIPTIONEND319'; expect 'MCP servers'
[[ $(grep -c 'DESCRIPTIONEND' "$term/feed") == 320 ]]
[[ $(grep -c 'Description' "$term/feed") == 1 ]]
send '/status\r'; expect 'Session ID'
mkdir "$work/blocked.md"
send '/export blocked.md\r'; expect 'ERROR: input:'
send '/log\r'; expect '.log'
prompt 'continue after the inventory' 'AFTER LARGE INVENTORY'
terminal_close
logs
discovery_count=$(jq -s '[.[] | select(.event == "mcp_updated")] | length' "$root/logs.jsonl")
# Even row streaming retains a bounded pending row. Oversized metadata must
# produce an inline error, leaving the terminal and following commands usable.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,mcp:{catalog:{tools:[{name:"oversized",description:("x"*70000),inputSchema:{type:"object"}}]}},plan:["AFTER RENDER FAILURE"]}')"
terminal_start -N
wait_for discovered
send '/tools\r'; expect 'Cannot render command output'
send '/status\r'; expect 'Session ID'
prompt 'continue after the rendering error' 'AFTER RENDER FAILURE'
terminal_close
