#!/usr/bin/env bash
# Real executable coverage: offline resolution, file ownership, parent/child
# isolation and request bodies use the same app/runtime paths as production.
cli_fixture_parent=$(dirname "$(dirname "$(realpath "$1")")")
source "$(dirname "$0")/cli_test_common.sh"
peer_start
mkdir -p "$root/cfg" "$root/offline"
printf 'CONFIG AGENT\n' >"$root/cfg/agent.md"
printf 'CONFIG REVIEWER\n' >"$root/cfg/reviewer.md"
printf 'ROOT ONLY {{agent_identity}} {{agent_tools}}\n' >"$work/root.md"
printf '[agent]\ndeveloper_instructions_file=agent.md\n[review]\ndeveloper_instructions_file=reviewer.md\n' >"$root/cfg/config.ini"

# ChatGPT resolution is offline, including absent auth and an unapproved cwd.
"$cli" --no-config --home "$root/offline" -C "$work" -p chatgpt -m gpt-6.1-sol -r high -d "$root/dump/deep"
[[ -s "$root/dump/deep/cai_developer_instructions.md" && -s "$root/dump/deep/cai_reviewer_instructions.md" ]]
absent "$root/dump/deep/cai_developer_instructions.md" '{{agent_'
absent "$root/dump/deep/cai_developer_instructions.md" '# CAI agent-mode'
contains "$root/dump/deep/cai_reviewer_instructions.md" 'You are cai, a code reviewer'
[[ ! -d "$root/offline/state/pouch" && $(requests) == 0 ]]
CAI_AGENT_MODEL=gpt-6.1-sol CAI_AGENT_REASONING_EFFORT=low "$cli" --no-config --home "$root/offline" -C "$work" -d "$root/env-dump"
cmp "$root/dump/deep/cai_developer_instructions.md" "$root/env-dump/cai_developer_instructions.md"
mkdir -m 0755 "$root/existing-directory"
run -d "$root/existing-directory"; ok
[[ $(stat -c %a "$root/existing-directory") == 755 ]]
[[ -s "$root/existing-directory/cai_developer_instructions.md" ]]
run -d "$work/root.md"; bad

# Identity stays readable inside the prompt; only safe filenames are produced.
run --config "$root/cfg/config.ini" --identity 'My Agent (Rev)' -d "$root/named"; ok
[[ -f "$root/named/my_agent_rev_developer_instructions.md" ]]
cmp "$root/cfg/agent.md" "$root/named/my_agent_rev_developer_instructions.md"
run --config "$root/cfg/config.ini" -z root.md -I ADDITION -d "$root/overridden"; ok
# Files preserve their trailing newline, followed by the extension separator.
{ cat "$work/root.md"; printf '\n\nADDITION'; } >"$root/expected"
cmp "$root/expected" "$root/overridden/cai_developer_instructions.md"
{ cat "$root/cfg/reviewer.md"; printf '\n\nADDITION'; } >"$root/expected"
cmp "$root/expected" "$root/overridden/cai_reviewer_instructions.md"
run --config "$root/cfg/config.ini" --review -z root.md -d "$root/review-root"; ok
cmp "$work/root.md" "$root/review-root/cai_reviewer_instructions.md"
cmp "$root/cfg/agent.md" "$root/review-root/cai_developer_instructions.md"
CAI_DEVELOPER_INSTRUCTIONS_FILE="$work/root.md" CAI_REVIEWER_INSTRUCTIONS_FILE="$work/root.md" run --config "$root/cfg/config.ini" -d "$root/env-files"; ok
cmp "$work/root.md" "$root/env-files/cai_developer_instructions.md"
cmp "$work/root.md" "$root/env-files/cai_reviewer_instructions.md"

# Existing outputs and symlinks are never truncated, including partial pairs.
run --config "$root/cfg/config.ini" -d "$root/env-files"; bad
cmp "$work/root.md" "$root/env-files/cai_developer_instructions.md"
mkdir "$root/collision"
ln -s "$work/root.md" "$root/collision/cai_reviewer_instructions.md"
run -d "$root/collision"; bad
[[ ! -e "$root/collision/cai_developer_instructions.md" ]]
[[ -L "$root/collision/cai_reviewer_instructions.md" ]]
[[ $(cat "$work/root.md") == 'ROOT ONLY {{agent_identity}} {{agent_tools}}' ]]
for identity in '!!!' '../escape' 'A/B' 'Name$HOME' 'A`id`' $'A\nB' $'A\033B' ' ._-() ' 'Two  Spaces' ' Leading' 'Trailing '; do
  run --identity "$identity" -d "$root/invalid-identity"; bad
done
run -z missing.md -d "$root/missing"; bad
printf 'NUL\000INPUT' >"$work/nul.md"
run -z nul.md -d "$root/nul"; bad
mkfifo "$work/fifo.md"
run -z fifo.md -d "$root/fifo"; bad
head -c 1048577 /dev/zero >"$work/large.md"
run -z large.md -d "$root/large"; bad
[[ $(requests) == 0 ]]

# Parent-only CLI replacement survives model resolution but cannot enter an
# isolated reviewer, which uses the preconfigured reviewer default instead.
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -nc --arg report "$report" '{plan:[{tool:"run_review",arguments:{target:"custom",instructions:"Inspect the fixture"}},$report,"PARENT FINISHED"]}')"
before=$(requests)
run --config "$root/cfg/config.ini" -Nni 'review this' -z root.md -I ADDITION; ok
[[ $(requests) == "$((before+3))" ]]
sed -n "$((before+1))p" "$root/peer/requests.jsonl" | jq -r .instructions >"$root/parent"
sed -n "$((before+2))p" "$root/peer/requests.jsonl" | jq -r .instructions >"$root/child"
contains "$root/parent" 'ROOT ONLY {{agent_identity}} {{agent_tools}}'
contains "$root/parent" ADDITION
absent "$root/parent" 'CONFIG AGENT'
contains "$root/child" 'CONFIG REVIEWER'
absent "$root/child" 'ROOT ONLY'
first=$(grep -n -m1 ADDITION "$root/parent" | cut -d: -f1)
second=$(grep -n -m1 '# CAI' "$root/parent" | cut -d: -f1)
[[ "$first" -lt "$second" ]]

# Direct --review makes the reviewer the initial parent, so -z wins there.
plan "$(jq -nc --arg report "$report" '{plan:[$report]}')"
run --config "$root/cfg/config.ini" --review -i 'Inspect fixture' -z root.md; ok
tail -1 "$root/peer/requests.jsonl" | jq -r .instructions >"$root/direct-review"
contains "$root/direct-review" 'ROOT ONLY'
absent "$root/direct-review" 'CONFIG REVIEWER'

# A persistent owner must forget the one-review override after use. The role
# is part of admission, so an ordinary -z invocation cannot silently reuse it.
plan "$(jq -nc --arg report "$report" '{plan:[$report]}')"
run --config "$root/cfg/config.ini" --persist --review -i 'Inspect fixture' -z root.md; ok
before=$(requests)
run --config "$root/cfg/config.ini" -ni 'Do not submit' -z root.md; bad
[[ $(requests) == "$before" ]]
run --config "$root/cfg/config.ini" --review -i 'Do not submit' -z root.md; bad
contains "$root/stderr" 'requires a new workspace owner'
[[ $(requests) == "$before" ]]
terminal_start --config "$root/cfg/config.ini"
plan "$(jq -nc --arg report "$report" '{plan:[$report]}')"
send '/review custom Inspect again\r'; expect 'Review finished'
tail -1 "$root/peer/requests.jsonl" | jq -r .instructions >"$root/second-review"
contains "$root/second-review" 'CONFIG REVIEWER'
absent "$root/second-review" 'ROOT ONLY'
send '/shutdown\r'
wait_for test -s "$term/status"
wait "$terminal_pid"; terminal_pid=
[[ $(cat "$term/status") == 0 ]]
owner_gone() { [[ -z $(find "$root/cache/cai" -name 'ws-*.info' -print -quit) ]]; }
wait_for owner_gone

# Relative CLI files retain their original workspace when --temp changes it.
plan '{"plan":["TEMP PARENT"]}'
TMPDIR="$root" run --temp --force -ni 'temporary hello' -z root.md; ok
tail -1 "$root/peer/requests.jsonl" | jq -r .instructions >"$root/temp-parent"
contains "$root/temp-parent" 'ROOT ONLY'

# Switching conversations restores configured defaults, rather than carrying
# an invocation-only override into the new parent. No synthetic startup input.
terminal_start --config "$root/cfg/config.ini" -N -z root.md
send '/m gpt-6-astra\r'; expect 'Model changed to gpt-6-astra.'
plan '{"plan":["OVERRIDE AFTER MODEL CHANGE"]}'
prompt 'hello' 'OVERRIDE AFTER MODEL CHANGE'
tail -1 "$root/peer/requests.jsonl" | jq -r .instructions >"$root/changed-model"
contains "$root/changed-model" 'ROOT ONLY {{agent_identity}} {{agent_tools}}'
send '/new\r'; expect 'New session'
plan '{"plan":["NEW DEFAULT PARENT"]}'
prompt 'hello' 'NEW DEFAULT PARENT'
terminal_close
tail -1 "$root/peer/requests.jsonl" | jq -r .instructions >"$root/new-parent"
contains "$root/new-parent" 'CONFIG AGENT'
absent "$root/new-parent" 'ROOT ONLY'
