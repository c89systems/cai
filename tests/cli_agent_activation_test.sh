#!/usr/bin/env bash
# Agent activation crosses the owner protocol; export retains historical
# instructions independently of the registry's current contents.
cli_fixture_parent=$(dirname "$(dirname "$(realpath "$1")")")
source "$(dirname "$0")/cli_test_common.sh"
peer_start
cat >"$root/seed.ini" <<INI
[client]
provider = custom
base_url = "$endpoint"
api_key = fixture
[agent]
model = default-model
INI
base=("$cli" -c "$root/seed.ini" --no-terminal --no-image-generation --no-mcp --insecure -C "$work" --runtime-dir "$root/run")
owner_pid=
finish() {
  local result=$?
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  cleanup "$result"
}
trap finish EXIT
for name in first second; do
  run --build "$root/$name"; ok
  printf '%s_AGENT_ORIGINAL\n' "$name" >"$root/$name/SYSTEM_PROMPT.md"
  sed -i "s/model = \"default-model\"/model = \"$name-model\"/" "$root/$name/config.ini"
  cat >>"$root/$name/config.ini" <<INI
[client]
api_key = $name-agent-activation-private-key
INI
  if [[ "$name" == second ]]; then
    mkdir -p "$root/$name/tools/echo"
    printf '%s\n' '{"name":"second_agent_tool","description":"Second agent capability","parameters":{"type":"object"},"executor":"process","argv":["/bin/cat"]}' >"$root/$name/tools/echo/tool.json"
    printf '\n[review]\nmodel = second-review-model\n' >>"$root/$name/config.ini"
  fi
  run --make "$root/$name"; ok
done
last_request() { tail -1 "$root/peer/requests.jsonl" >"$root/request"; }
plan '{"plan":["ORIGINAL_SESSION_REPLY"]}'
run --use first -Nni 'ORIGINAL_SESSION_PROMPT'; ok
id=$(current_session_id)
last_request
json "$root/request" '.model == "first-model"'
contains "$root/request" first_AGENT_ORIGINAL
owner_stopped() { ! compgen -G "$root/run/ws-*.sock" >/dev/null; }
wait_for owner_stopped

export LOG_LEVEL=info
"${base[@]}" --use first -x -R "$id" </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
plan '{"plan":[{"output_items":[{"type":"compaction","id":"cmp_activation","encrypted_content":"ACTIVATION_OPAQUE_CONTEXT"}]}]}'
run -ni '/compact'; ok
plan '{"plan":["ATTACHED_SECOND_REPLY"]}'
run -ni $'/use\t second  '; ok
run --use second -ni 'continue the same session'; ok
[[ $(current_session_id) == "$id" ]]
last_request
json "$root/request" '.model == "second-model"'
contains "$root/request" second_AGENT_ORIGINAL
contains "$root/request" ACTIVATION_OPAQUE_CONTEXT
contains "$root/request" second_agent_tool
absent "$root/request" first_AGENT_ORIGINAL

# Publishing does not mutate a running definition. Explicit activation reloads
# the revision and journals the changed instructions before its next request.
printf 'SECOND_AGENT_REVISED\n' >"$root/second/SYSTEM_PROMPT.md"
run --yes --make "$root/second"; ok
plan '{"plan":["OLD_REVISION_STILL_ACTIVE"]}'
run -ni 'keep the resolved definition'; ok
last_request
contains "$root/request" second_AGENT_ORIGINAL
absent "$root/request" SECOND_AGENT_REVISED
plan '{"plan":["REVISED_DEFINITION_ACTIVE"]}'
run --use second -ni 'activate the revised definition'; ok
last_request
contains "$root/request" SECOND_AGENT_REVISED
absent "$root/request" second_AGENT_ORIGINAL
before=$(requests)
run --use missing -ni 'must not run'; bad
[[ $(requests) == "$before" ]]

# Controls and session selection use the same owner. A steerable submission
# of /use is visibly reclassified and never applied mid-response.
run --use first --build "$root/noauth"; ok
sed -i 's/provider = "custom"/provider = "chatgpt"/' "$root/noauth/config.ini"
run --make "$root/noauth"; ok
terminal_start
before=$(requests)
send '/use noauth\r'; expect 'selected agent has no native credentials'
send '/use\r'; expect 'second is active'
[[ $(requests) == "$before" ]]
plan '{"plan":[{"pause":true,"text":"BEFORE_QUEUED_ACTIVATION"}]}'
send 'hold the response\r'; expect 'STREAM in flight'
send '/use first\033\r'; expect 'Q 1. /use first'
release; expect BEFORE_QUEUED_ACTIVATION
expect 'first is active'
plan '{"plan":["AFTER_QUEUED_ACTIVATION"]}'
prompt 'after the safe switch' AFTER_QUEUED_ACTIVATION
last_request
contains "$root/request" first_AGENT_ORIGINAL
absent "$root/request" SECOND_AGENT_REVISED
send '/new\r'; expect 'New session'; expect 'ID:'
new_id=$(current_session_id)
[[ "$id" != "$new_id" ]]
send "/resume $id\r"; expect 'Session resumed'
send '/use\r'; expect 'first is active'
send '/trim 1\r'; expect 'Trimmed 1 newest history entry'
send '/export retained\r'; expect 'Session export'
send '/goal\r'; expect 'No goal.'
contains "$work/retained.md" first_AGENT_ORIGINAL
contains "$work/retained.md" second_AGENT_ORIGINAL
contains "$work/retained.md" SECOND_AGENT_REVISED
absent "$work/retained.md" agent-activation-private-key
absent "$work/retained.jsonl" agent-activation-private-key
terminal_close

# The builder mode is also a backend-owned activation on an existing owner.
plan '{"plan":["LIVE_BUILDER_REPLY"]}'
run --builder -ni 'help build a new agent'; ok
last_request
contains "$root/request" 'helping the user build a complete stored agent'
contains "$root/request" ACTIVATION_OPAQUE_CONTEXT
[[ $(current_session_id) == "$id" ]]
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=

# Offline export reads recorded instructions, even when their original
# registry definitions have since changed. JSONL roundtrips preserve them.
printf 'FIRST_AGENT_REPLACEMENT\n' >"$root/first/SYSTEM_PROMPT.md"
run --yes --make "$root/first"; ok
cat >>"$root/seed.ini" <<INI
[store.unopened]
endpoint = "pouch://$root/unopened"
key_file = "$root/unopened.key"
INI
run --agents-store unopened --export "$id" --export-out "$root/offline"; ok
[[ ! -e "$root/unopened" && ! -e "$root/unopened.key" ]]
contains "$root/offline.md" first_AGENT_ORIGINAL
contains "$root/offline.md" second_AGENT_ORIGINAL
contains "$root/offline.md" SECOND_AGENT_REVISED
contains "$root/offline.md" "$work"
absent "$root/offline.md" FIRST_AGENT_REPLACEMENT
run --import "$root/offline.jsonl"; ok
imported=$(current_session_id)
run --export "$imported" --export-out "$root/roundtrip.md"; ok
contains "$root/roundtrip.md" first_AGENT_ORIGINAL
contains "$root/roundtrip.md" second_AGENT_ORIGINAL
contains "$root/roundtrip.md" SECOND_AGENT_REVISED
absent "$root/roundtrip.md" agent-activation-private-key

# Import must publish selection metadata before the first resume.
plan '{"plan":["IMPORTED_NAMED_AGENT_REPLY"]}'
run --resume "$imported" -ni 'continue the imported session'; ok
last_request
json "$root/request" '.model == "first-model"'
contains "$root/request" FIRST_AGENT_REPLACEMENT
[[ $(current_session_id) == "$imported" ]]
wait_for owner_stopped
# Directory recovery must also retain the normalized seed's binding.
run --import "$root/offline.jsonl"; ok
imported=$(current_session_id)
run --delete "$new_id"; ok
plan '{"plan":["IMPORTED_RECOVERED_AGENT_REPLY"]}'
run --resume "$imported" -ni 'continue after rebuilding the directory index'; ok
last_request
json "$root/request" '.model == "first-model"'
contains "$root/request" FIRST_AGENT_REPLACEMENT
[[ $(current_session_id) == "$imported" ]]
wait_for owner_stopped
plan '{"plan":["IMPORTED_LATEST_AGENT_REPLY"]}'
run -ni 'continue the latest imported session'; ok
last_request
json "$root/request" '.model == "first-model"'
contains "$root/request" FIRST_AGENT_REPLACEMENT
[[ $(current_session_id) == "$imported" ]]
