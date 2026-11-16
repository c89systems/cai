#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
source_dir="$root/New Agent!"
run --build "$source_dir"; ok
contains "$source_dir/config.ini" 'name = "new_agent"'
contains "$source_dir/SYSTEM_PROMPT.md" '{{runtime_default}}'
[[ -f "$source_dir/EXAMPLE_SYSTEM_PROMPT.md" && ! -e "$source_dir/agent.json" ]]
contains "$source_dir/EXAMPLE_SYSTEM_PROMPT.md" 'When to ask the user for permission'
absent "$source_dir/EXAMPLE_SYSTEM_PROMPT.md" 'an agent based on GPT'
absent "$source_dir/EXAMPLE_SYSTEM_PROMPT.md" '{{agent_identity}}'
[[ $(requests) == 0 ]]
mkdir -p "$root/By Default"
run -C "$root/By Default" --build; ok
contains "$root/By Default/config.ini" 'name = "by_default"'
run -C "$root/By Default" --make; ok
run -C "$root/By Default" --dump-agent by_default; ok
[[ -f "$root/By Default/by_default/SYSTEM_PROMPT.md" ]]

# User edits override both the build-time inferred name and directory name.
sed -i 's/name = "new_agent"/name = "Engineer Name!"/' "$source_dir/config.ini"
cat >>"$source_dir/config.ini" <<INI
[client]
provider = custom
base_url = "$endpoint"
api_key = "agent-package-private-key"
INI
printf 'ENGINEER_PACKAGE_INSTRUCTIONS\n' >"$source_dir/SYSTEM_PROMPT.md"
printf ' \n\t\n' >"$source_dir/AGENTS.md"
mkdir -p "$source_dir/files/nested" "$source_dir/tools/echo"
dd if=/dev/zero of="$source_dir/files/nested/binary.bin" bs=1048576 count=2 status=none
deep="$source_dir/files"
for ((level=0; level<100; ++level)); do deep="$deep/d"; done
mkdir -p "$deep"
printf 'DEEP_AGENT_RESOURCE\n' >"$deep/end.txt"
printf '%s\n' '{"name":"package_echo","description":"Bundled agent tool","parameters":{"type":"object"},"executor":"process","argv":["/bin/cat"]}' >"$source_dir/tools/echo/tool.json"
token='e30.eyJleHAiOjQxMDI0NDQ4MDB9.fixture'
jq -n --arg token "$token" '{auth_mode:"chatgpt",tokens:{id_token:$token,access_token:$token,refresh_token:"package-refresh-secret",account_id:"apps-account"}}' >"$root/import-auth.json"
run --make "$source_dir" --from-auth-json "$root/import-auth.json"; ok
contains "$root/stdout" 'Published agent engineer_name.'
[[ $(requests) == 0 && $("$cli" --no-config -C "$work" --list) == 'No sessions.' ]]
run --make "$source_dir"; bad
contains "$root/stderr" 'agent already exists'

# Ordinary package exports never write declared secret config values.
run --dump-agent engineer_name; ok
dump="$work/engineer_name"
[[ ! -e "$dump/agent.json" && ! -e "$dump/AGENTS.md" && ! -e "$dump/AUTH.json" ]]
absent "$dump/config.ini" agent-package-private-key
cmp "$source_dir/files/nested/binary.bin" "$dump/files/nested/binary.bin"
contains "$dump/tools/echo/tool.json" package_echo
contains "$dump/config.ini" 'name = "engineer_name"'
rm -rf "$dump"
run --include-credentials --dump-agent engineer_name; ok
contains "$dump/config.ini" agent-package-private-key
json "$dump/AUTH.json" '.tokens.refresh_token == "package-refresh-secret"'
# Replacing a definition without an import preserves the current native auth.
run --yes --make "$source_dir"; ok
rm -rf "$dump"
run --include-credentials --dump-agent engineer_name; ok
json "$dump/AUTH.json" '.tokens.refresh_token == "package-refresh-secret"'
run --dump-agent engineer_name; bad
contains "$root/stderr" 'must be empty'

# Named runtime activation uses bundled instructions/tools and the same session.
plan '{"plan":[{"tool":"package_echo","arguments":{"value":"BUNDLED_TOOL_RESULT"}},"ENGINEER_REPLY"]}'
run --use 'Engineer Name!' -Nni 'first named agent turn'; ok
id=$(current_session_id)
contains "$root/stdout" ENGINEER_REPLY
contains "$root/peer/requests.jsonl" ENGINEER_PACKAGE_INSTRUCTIONS
contains "$root/peer/requests.jsonl" BUNDLED_TOOL_RESULT
contains "$root/peer/headers.jsonl" 'Bearer agent-package-private-key'
[[ -n "$id" ]]
if rg --text -l 'agent-package-private-key' "$root/cache/cai/agents" >"$root/cached-keys"; then
  fail 'Agent API key was copied into the runtime cache'
fi
[[ -z $(find "$root/cache/cai/agents" -name config.ini -o -name AUTH.json) ]]
if rg --text -l 'package-refresh-secret' "$root/cache/cai/agents"; then
  fail 'Native credentials were copied into the runtime cache'
fi
# The imported auth also serves ChatGPT apps with a different inference provider.
cat >>"$source_dir/config.ini" <<INI
[mcp.chatgpt]
endpoint = "${endpoint%/v1}/mcp/chatgpt"
INI
run --yes --make "$source_dir"; ok
plan '{"plan":["NATIVE_AGENT_APPS_AUTH"]}'
run --use engineer_name --mcp -ni 'use imported native auth'; ok
contains "$root/stdout" NATIVE_AGENT_APPS_AUTH
json "$root/peer/mcp_requests.jsonl" 'select(.authorization == "Bearer e30.eyJleHAiOjQxMDI0NDQ4MDB9.fixture")'


# A template can be renamed in its generated config and published independently.
run --build "$root/other" --use engineer_name; ok
contains "$root/other/config.ini" 'name = "other"'
printf 'OTHER_PACKAGE_INSTRUCTIONS\n' >"$root/other/SYSTEM_PROMPT.md"
run --make "$root/other"; ok
plan '{"plan":["OTHER_REPLY"]}'
run --use other -ni 'continue with another agent'; ok
[[ $(current_session_id) == "$id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" ENGINEER_REPLY
contains "$root/request" OTHER_PACKAGE_INSTRUCTIONS
absent "$root/request" ENGINEER_PACKAGE_INSTRUCTIONS

# Without --use, restore the last selected agent for this exact saved session.
plan '{"plan":["RESTORED_OTHER_AGENT"]}'
run -ni 'restore the selected agent'; ok
[[ $(current_session_id) == "$id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" OTHER_PACKAGE_INSTRUCTIONS
contains "$root/request" OTHER_REPLY
plan '{"plan":["NEW_DEFAULT_AGENT"]}'
run -Nni 'a new session uses default'; ok
new_id=$(current_session_id)
[[ "$new_id" != "$id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" OTHER_PACKAGE_INSTRUCTIONS
# A numbered/exact resume restores the saved selection independently of latest.
plan '{"plan":["RESUMED_OTHER_AGENT"]}'
run -R "$id" -ni 'resume the selected agent'; ok
[[ $(current_session_id) == "$id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" OTHER_PACKAGE_INSTRUCTIONS

# Missing names and invalid source data must not start inference or publish.
before=$(requests)
run --use absent -Nni 'must not run'; bad
[[ $(requests) == "$before" ]]
mkdir -p "$root/missing"
printf '[agent]\nname=missing\n' >"$root/missing/config.ini"
run --make "$root/missing"; bad
run --use missing -Nni 'must not run'; bad
[[ $(requests) == "$before" ]]

# Empty names use the basename. Symlinks and malformed instructions fail safely.
run --build "$root/fallback"; ok
sed -i '/^name = /d' "$root/fallback/config.ini"
run --make "$root/fallback"; ok
ln -s "$source_dir/files/nested/binary.bin" "$root/fallback/files-escape"
mkdir -p "$root/fallback/files"
ln -s "$source_dir/files/nested/binary.bin" "$root/fallback/files/escape"
run --yes --make "$root/fallback"; bad
plan '{"plan":["ROLLBACK_AGENT_USABLE"]}'
run --use fallback -Nni 'use the previous valid publication'; ok
contains "$root/stdout" ROLLBACK_AGENT_USABLE

run --dump-agents "$root/all-agents"; ok
[[ -f "$root/all-agents/engineer_name/config.ini" && -f "$root/all-agents/other/config.ini" ]]
absent "$root/all-agents/engineer_name/config.ini" agent-package-private-key

# Attaching with --use activates through the owner, preserving the live
# session and terminal transcript cursor instead of silently ignoring it.
terminal_start --use engineer_name -R "$id"
send '/status\r'; expect 'engineer_name'
before=$(requests)
run --use other -ni 'observer must not change the active agent'; bad
[[ $(requests) == "$before" ]]
send '/use other\r'; expect 'other is active'
plan '{"plan":["LIVE_OTHER_AGENT"]}'
prompt 'continue in the existing owner' LIVE_OTHER_AGENT
[[ $(requests) == $((before+1)) ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" OTHER_PACKAGE_INSTRUCTIONS
absent "$root/request" ENGINEER_PACKAGE_INSTRUCTIONS
send '/status\r'; expect_table 1 Agent; expect 'other'
send '/use engineer_name\r'; expect 'engineer_name is active'
plan '{"plan":["LIVE_ENGINEER_AGAIN"]}'
prompt 'continue after changing back' LIVE_ENGINEER_AGAIN
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" ENGINEER_PACKAGE_INSTRUCTIONS
contains "$root/request" LIVE_OTHER_AGENT
terminal_close

# Publication validates manifests and rejects special files without blocking.
printf '%s\n' '{invalid tool json' >"$source_dir/tools/echo/tool.json"
run --yes --make "$source_dir"; bad
mkfifo "$root/fallback/files/pipe"
rm "$root/fallback/files/escape"
run --yes --make "$root/fallback"; bad
rm "$root/fallback/files/pipe"
printf '\0' >>"$root/fallback/SYSTEM_PROMPT.md"
run --yes --make "$root/fallback"; bad
contains "$root/stderr" 'NUL-free'
run --build "$root/modes" --headless; bad
run --include-credentials --list; bad
run --from-auth-json "$root/import-auth.json" --list; bad

# Explicitly configured seed resources become portable bundled files.
printf 'SEEDED_LITERAL_INSTRUCTIONS\n' >"$root/seed-prompt.md"
printf 'SEEDED_GLOBAL_INSTRUCTIONS\n' >"$root/seed-agents.md"
mkdir -p "$root/seed-tools/echo" "$root/seed-skills"
printf '%s\n' '{"name":"seed_echo","description":"Seed tool","parameters":{"type":"object"},"executor":"process","argv":["/bin/cat"]}' >"$root/seed-tools/echo/tool.json"
cat >"$root/seed.ini" <<INI
[store.seed]
endpoint = "pouch://$root/seed-pouch"
key_file = "$root/seed-pouch.key"
[agent]
developer_instructions_file = "$root/seed-prompt.md"
[paths]
agents_md = "$root/seed-agents.md"
skills_dir = "$root/seed-skills"
[plugins]
directory = "$root/seed-tools"
INI
# Failed initialization publishes neither a partial definition nor resources.
cp "$root/seed-tools/echo/tool.json" "$root/seed-tool-valid.json"
printf '{invalid json\n' >"$root/seed-tools/echo/tool.json"
run -c "$root/seed.ini" --agents-store seed --build "$root/invalid-seed"; bad
[[ ! -e "$root/invalid-seed" ]]
cp "$root/seed-tool-valid.json" "$root/seed-tools/echo/tool.json"
printf '\0' >>"$root/seed-prompt.md"
run -c "$root/seed.ini" --agents-store seed --build "$root/invalid-seed"; bad
contains "$root/stderr" NUL-free
printf 'SEEDED_LITERAL_INSTRUCTIONS\n' >"$root/seed-prompt.md"
run -c "$root/seed.ini" --agents-store seed --build "$root/portable"; ok
contains "$root/portable/SYSTEM_PROMPT.md" SEEDED_LITERAL_INSTRUCTIONS
contains "$root/portable/AGENTS.md" SEEDED_GLOBAL_INSTRUCTIONS
contains "$root/portable/tools/echo/tool.json" seed_echo
# Restore the default marker after copying an empty configured skills root.
contains "$root/portable/config.ini" 'directory = "tools"'
run -c "$root/seed.ini" --agents-store seed --make "$root/portable"; ok
rm "$root/seed-prompt.md" "$root/seed-agents.md"
plan '{"plan":["PORTABLE_AGENT_WORKED"]}'
run -c "$root/seed.ini" --agents-store seed --use portable -Nni 'portable'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" SEEDED_LITERAL_INSTRUCTIONS
contains "$root/request" SEEDED_GLOBAL_INSTRUCTIONS
contains "$root/request" seed_echo
if rg --text -l 'agent-package-private-key|package-refresh-secret' "$root/state/cai/pouch"; then
  fail 'Agent credentials are present as plaintext in the encrypted store'
fi

# The initialized default captures its resources once as well. Source edits
# and removal cannot change a later export or runtime behind its revision.
run -c "$root/seed.ini" --agents-store seed --build "$root/default-copy"; ok
contains "$root/default-copy/SYSTEM_PROMPT.md" SEEDED_LITERAL_INSTRUCTIONS
contains "$root/default-copy/AGENTS.md" SEEDED_GLOBAL_INSTRUCTIONS
contains "$root/default-copy/tools/echo/tool.json" seed_echo
printf 'USER_OWNED_FILE\n' >"$root/default-copy/keep.txt"
run -c "$root/seed.ini" --agents-store seed --build "$root/default-copy"; bad
contains "$root/default-copy/keep.txt" USER_OWNED_FILE
