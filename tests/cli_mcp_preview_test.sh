#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
base+=(--mcp)
width=80
cat >"$root/preview.ini" <<INI
[client]
api_protocol = $protocol
[mcp.alpha]
endpoint = "${endpoint%/v1}/mcp/alpha"
eager = true
INI
base+=(--config "$root/preview.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
preview_plan() {
  plan "$(jq -n --argjson chat "$chat" --arg kind "$1" --arg reply "$2" '{chat:$chat,mcp:{alpha:{call_result:({content:[{type:"text",text:(if $kind == "lines" then "HEADVISIBLE\n"+("preview line\n"*2000)+"TAILLINES" elif $kind == "large" then "{\"HEADVISIBLE\":\""+("x"*180000)+"TAILLARGE\"}" elif $kind == "chart" then "```mdf-bar-chart\nkey,value\n"+("Metric,1\n"*40)+"```" else "HEADVISIBLE "+("x"*2000)+"TAILFULL" end)}]} + if $kind == "full" or $kind == "chart" then {} else {structuredContent:{responses:[{id:"message",headers:("header "*10000)}]}} end)}},plan:[{tool:"mcp_5_alpha_echo",arguments:{text:"preview"}},$reply]}')"
}
preview_plan lines 'LINES DONE'
terminal_start -N
discovered() { logs; jq -se 'any(.[];.event=="mcp_updated")' "$root/logs.jsonl" >/dev/null; }
wait_for discovered
prompt 'show the multiline tool result' 'LINES DONE'
contains "$term/feed" HEADVISIBLE
contains "$term/feed" 'Output omitted'
absent "$term/feed" TAILLINES
[[ $(wc -c <"$term/feed") -lt 3000 ]]
preview_plan large 'LARGE DONE'
prompt 'show the single-line tool result' 'LARGE DONE'
tail -c +$((send_feed_offset+1)) "$term/feed" >"$root/large-preview"
contains "$root/large-preview" HEADVISIBLE
contains "$root/large-preview" 'Output omitted'
absent "$root/large-preview" TAILLARGE
[[ $(wc -c <"$root/large-preview") -lt 3000 ]]
preview_plan chart 'CHART DONE'
prompt 'show a chart preview' 'CHART DONE'
send '/output full\r'; expect 'applies from the next tool call'
preview_plan full 'FULL DONE'
prompt 'show the full tool result' 'FULL DONE'
contains "$term/feed" TAILFULL
# Presentation clipping must not alter model input or durable results.
contains "$root/peer/requests.jsonl" TAILLINES
contains "$root/peer/requests.jsonl" TAILLARGE
id=$(current_session_id)
terminal_close
run --export "$id" --export-out "$root/complete.jsonl"; ok
contains "$root/complete.jsonl" TAILLINES
contains "$root/complete.jsonl" TAILLARGE
before=$(requests)
terminal_start --resume "$id"
expect 'Conversation resumed.'
contains "$term/text" 'Output omitted'
absent "$term/text" TAILLINES
absent "$term/text" TAILLARGE
absent "$term/text" TAILFULL
[[ $(wc -c <"$term/text") -lt 6000 && $(requests) == "$before" ]]
terminal_close
