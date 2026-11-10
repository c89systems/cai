#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol = %s\n' "$protocol" >"$root/context.ini"
base+=(--config "$root/context.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
text='# Shared context

Raw **Markdown**, "quotes", backslash \, and å.'
plan "$(jq -n --argjson chat "$chat" --arg text "$text" '{chat:$chat,plan:[{tool:"context_create",arguments:(if $chat then {input:$text} else $text end)},"CONTEXT_CREATED"]}')"
run -Nni 'Store this context'; ok
contains "$root/stdout" CONTEXT_CREATED
created_session=$(current_session_id)
# Native freeform / adapted Chat Completions both register the complete surface.
jq -se '.[0] as $r | all(["context_create","context_set","context_query","context_get","context_upload","context_download","context_delete","context_replace"][]; . as $n | any($r.tools[]; (.name // .function.name) == $n))' "$root/peer/requests.jsonl" >/dev/null
id=$(jq -sr '[.[] | (.input[]? | select(.type == "custom_tool_call_output") | .output | fromjson? | .id), (.messages[]? | select(.role == "tool") | .content | fromjson? | .id)] | map(select(. != null)) | last' "$root/peer/requests.jsonl")
[[ "$id" != null && ${#id} == 20 ]] || fail 'Missing context XID receipt'
# A different workspace/conversation can retrieve it through the same store.
mkdir -p "$root/other"
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" --arg id "$id" '{chat:$chat,plan:[{tool:"context_get",arguments:{id:$id}},"SHARED_CONTEXT_FOUND"]}')"
run -C "$root/other" -Nni 'Read shared context'; ok
contains "$root/stdout" SHARED_CONTEXT_FOUND
contains "$root/peer/requests.jsonl" 'Raw **Markdown**'
run -D "$created_session"; ok
# Explicit utility export needs no authentication, provider request or frontend.
before=$(requests)
run --context-export "$root/export"; ok
[[ $(requests) == "$before" ]] || fail 'Context export invoked inference'
printf '%s' "$text" >"$root/expected.md"
cmp "$root/expected.md" "$root/export/$id/$id.md"
json "$root/export/$id.context.json" ".metadata.id == \"$id\" and .metadata.kind == \"text\""
# Namespace and feature switches obey environment/INI/flag precedence.
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["CONTEXT_OFF"]}')"
CAI_CONTEXT=false run -Nni 'No context tools'; ok
jq -se '.[0] | all(.tools[]; ((.name // .function.name // "") | startswith("context_")) | not)' "$root/peer/requests.jsonl" >/dev/null
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["CONTEXT_ON"]}')"
CAI_CONTEXT=false run --context -Nni 'Explicit enable'; ok
jq -se '.[0] | any(.tools[]; (.name // .function.name) == "context_create")' "$root/peer/requests.jsonl" >/dev/null
run --context-namespace other.context --context-export "$root/isolated"; ok
json "$root/stdout" '.items == 0'
run --context-namespace 'bad/namespace' --context-export "$root/bad"; [[ "$rc" != 0 ]]
# Named context stores remain independent and aliases do not alter scope.
cat >>"$root/context.ini" <<INI
[storage]
context_store = knowledge
[store.knowledge]
endpoint = pouch://$root/knowledge
key_file = $root/knowledge.key
INI
run --context-export "$root/named"; ok
json "$root/stdout" '.items == 0'
run --context-store default --context-export "$root/inherited"; ok
cmp "$root/expected.md" "$root/inherited/$id/$id.md"
# An inherited context store follows named history, not literal store.default.
: >"$root/peer/requests.jsonl"
plan "$(jq -n --argjson chat "$chat" --arg text "NAMED_HISTORY_CONTEXT" '{chat:$chat,plan:[{tool:"context_create",arguments:(if $chat then {input:$text} else $text end)},"NAMED_STORED"]}')"
run --sessions-store knowledge --context-store '' -Nni 'Store in named history'; ok
run --sessions-store knowledge --context-store '' --context-export "$root/named-history"; ok
json "$root/stdout" '.items == 1'
# Disabled context does not open or require a context-only endpoint.
plan "$(jq -n --argjson chat "$chat" '{chat:$chat,plan:["DISABLED_STORE_UNUSED"]}')"
run --no-context --context-store missing -Nni 'No store needed'; ok
# Inheritance means the actual client: temporary history also has temporary
# context unless an explicit persistent context store is selected.
plan "$(jq -n --argjson chat "$chat" --arg text "TEMPORARY_CONTEXT" '{chat:$chat,plan:[{tool:"context_create",arguments:(if $chat then {input:$text} else $text end)},"TEMP_STORED"]}')"
run --sessions-store knowledge --context-store '' --temp -y -ni 'Temporary context'; ok
run --sessions-store knowledge --context-store '' --context-export "$root/after-temp"; ok
json "$root/stdout" '.items == 1'
