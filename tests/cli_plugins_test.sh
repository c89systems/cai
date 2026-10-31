#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/plugins.ini"
base+=(--config "$root/plugins.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
packages="$work/.cai/tools"
mkdir -p "$packages/echo" "$packages/lua"
printf '%s\n' '{"name":"plugin_echo","description":"Version one","parameters":{"type":"object"},"executor":"process","argv":["/bin/cat"]}' >"$packages/echo/tool.json"
printf '%s\n' '{"name":"plugin_lua","description":"Lua example","parameters":{"type":"object"},"executor":"lua","entrypoint":"main.lua"}' >"$packages/lua/tool.json"
printf '%s\n' 'return function(input,ctx) local x=input:decode(); return {result=x.value+1,restricted=os==nil} end' >"$packages/lua/main.lua"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{tool:"plugin_echo",arguments:{payload:"PROCESS_PLUGIN"}},{tool:"plugin_lua",arguments:{value:41}},"PLUGINSFINISHED"]}')"
run -Nni 'use plugins'; ok
contains "$root/stdout" PLUGINSFINISHED
contains "$root/peer/requests.jsonl" PROCESS_PLUGIN
contains "$root/peer/requests.jsonl" '\"result\":42'
contains "$root/peer/requests.jsonl" '\"restricted\":true'
jq -se 'all(.[]; any(.tools[]; (.name // .function.name) == "plugin_echo") and any(.tools[]; (.name // .function.name) == "reload_tools"))' "$root/peer/requests.jsonl" >/dev/null
# A package modifies its own definition while invoked. Current calls pin old
# handlers, and the next request advertises the explicitly reloaded definition.
cat >"$packages/echo/change.sh" <<'SCRIPT'
#!/bin/sh
cat >/dev/null
cp .cai/tools/echo/next.json .cai/tools/echo/tool.json
printf OLD_HANDLER
SCRIPT
printf '%s\n' '{"name":"plugin_echo","description":"Version two","parameters":{"type":"object"},"executor":"process","argv":["/bin/sh","-c","cat >/dev/null; printf NEW_HANDLER"]}' >"$packages/echo/next.json"
printf '%s\n' '{"name":"plugin_echo","description":"Version one","parameters":{"type":"object"},"executor":"process","argv":["/bin/sh"],"entrypoint":"change.sh"}' >"$packages/echo/tool.json"
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{tool:"plugin_echo",arguments:{}},{tool:"reload_tools",arguments:{}},{tool:"plugin_echo",arguments:{}},{tool:"unload_tool",arguments:{name:"plugin_echo"}},"RELOADED"]}')"
run -Nni 'reload the tool'; ok
contains "$root/peer/requests.jsonl" OLD_HANDLER
contains "$root/peer/requests.jsonl" NEW_HANDLER
jq -se '.[0] | any(.tools[]; (.description // .function.description) == "Version one")' "$root/peer/requests.jsonl" >/dev/null
jq -se '.[2] | any(.tools[]; (.description // .function.description) == "Version two")' "$root/peer/requests.jsonl" >/dev/null
jq -se '.[-1] | all(.tools[]; (.name // .function.name) != "plugin_echo")' "$root/peer/requests.jsonl" >/dev/null
# Explicit off has no management tools, even with installed packages.
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["DISABLED"]}')"
run --no-plugins -Nni test; ok
jq -se '.[0] | all(.tools[]; (.name // .function.name) != "reload_tools")' "$root/peer/requests.jsonl" >/dev/null
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["ENVIRONMENT"]}')"
CAI_PLUGINS=false run -Nni test; ok
jq -se '.[0] | all(.tools[]; (.name // .function.name) != "reload_tools")' "$root/peer/requests.jsonl" >/dev/null
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["RELATIVEPATH"]}')"
run --tools-dir .cai/tools --workspace-tools-dir "$root/missing" -Nni test; ok
jq -se '.[0] | any(.tools[]; (.name // .function.name) == "plugin_echo")' "$root/peer/requests.jsonl" >/dev/null
# Interactive control uses the common Softline command path and Markdown feed.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[]}')"
terminal_start -N
send '/tools\r'; expect plugin_lua
send '/tools unload plugin_lua\r'; expect 'Tool catalog updated'
send '/tools reload\r'; expect 'Tool catalog updated'
send '/reasoning-tool on\r'; expect 'Changes apply to this app'
send '/tools reload\r'; expect 'Tool catalog updated'
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{tool:"plugin_lua",arguments:{value:41}},"TOGGLED"]}')"
send 'test the plugin after toggling native tools\r'; expect_assistant TOGGLED; expect 'Worked for'
jq -se '.[-1] | any(.tools[]; (.name // .function.name) == "set_reasoning")' "$root/peer/requests.jsonl" >/dev/null
send '/reasoning-tool off\r'; expect 'Changes apply to this app'
send '/tools reload\r'; expect 'Tool catalog updated'
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:[{tool:"plugin_lua",arguments:{value:1}},"DISABLEDTOOL"]}')"
send 'test the plugin again\r'; expect_assistant DISABLEDTOOL; expect 'Worked for'
jq -se '.[-1] | all(.tools[]; (.name // .function.name) != "set_reasoning")' "$root/peer/requests.jsonl" >/dev/null
terminal_close
