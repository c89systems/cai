#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
config="$root/bootstrap.ini"
write_bootstrap() {
  cat >"$config" <<EOF
[store.other]
endpoint = "pouch://$root/other-agents"
key_file = "$root/other-agents.key"
[agent]
model = "$1"
reasoning_effort = "$2"
developer_instructions = "$3"
EOF
}
write_bootstrap gpt-6-luna low STORED_AGENT_INITIAL
base=("$cli" -p custom --endpoint "$endpoint" --no-terminal --no-image-generation --no-mcp --insecure -C "$work" -c "$config")
plan '{"plan":["FIRST STORED AGENT"]}'
run -Nni 'first'; ok
absent "$root/stderr" storage.pouch
first_id=$(current_session_id)
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "gpt-6-luna" and .reasoning.effort == "low"'
json "$root/request" '.instructions | contains("STORED_AGENT_INITIAL")'

# Disk behavior and agent environment overrides stop participating after seed.
write_bootstrap gpt-6.1-sol high DISK_AGENT_CHANGED
plan '{"plan":["CONTINUED STORED AGENT"]}'
CAI_MODEL=gpt-6.1-sol CAI_AGENT_REASONING_EFFORT=high CAI_THEME=not-a-theme run -ni 'continue'; ok
[[ $(current_session_id) == "$first_id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "gpt-6-luna" and .reasoning.effort == "low"'
json "$root/request" '.instructions | contains("STORED_AGENT_INITIAL") and (contains("DISK_AGENT_CHANGED") | not)'

# A fresh session uses storage too, independently of saved model settings.
plan '{"plan":["NEW SESSION SAME AGENT"]}'
run -Nni 'fresh'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "gpt-6-luna"'
second_id=$(current_session_id)
[[ $second_id != "$first_id" ]]

# Explicit invocation flags still override the stored agent.
plan '{"plan":["EXPLICIT MODEL"]}'
run -m gpt-6.1-sol -r high -ni 'explicit'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "gpt-6.1-sol" and .reasoning.effort == "high"'

# Another repository seeds from the current INI without moving session history.
plan '{"plan":["OTHER REPOSITORY"]}'
run --agents-store other -ni 'another agent store'; ok
[[ $(current_session_id) == "$second_id" ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.model == "gpt-6.1-sol"'
json "$root/request" '.instructions | contains("DISK_AGENT_CHANGED")'
[[ -s "$root/other-agents.key" ]]

# Bootstrap environment and CLI selectors remain operational.
write_bootstrap gpt-6-luna low NEVER_RESEED
plan '{"plan":["BOOTSTRAP ENV"]}'
CAI_AGENTS_STORE=other run -ni 'bootstrap environment'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
json "$root/request" '.instructions | contains("DISK_AGENT_CHANGED") and (contains("NEVER_RESEED") | not)'
before=$(requests)
run --agents-store absent -ni 'invalid repository'; bad
[[ $(requests) == "$before" ]]
contains "$root/stderr" 'requires a pouch:// or HTTP(S) endpoint'
