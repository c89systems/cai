#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ $protocol != chat_completions ]] || chat=true
persistent=0
chart_cleanup() {
  local result=$?
  if ((persistent)); then
    "${base[@]}" -X >"$root/cleanup.out" 2>"$root/cleanup.err" || :
  fi
  cleanup "$result"
}
trap chart_cleanup EXIT
marker='## Markdown charts (on request)'
guide() {
  contains "$1" "$marker"
  [[ $(grep -Fc "$marker" "$1") == 1 ]]
  contains "$1" 'mdf-horizontal-bar-chart'
  contains "$1" 'mdf-vertical-bar-chart'
  contains "$1" 'mdf-tile-chart'
  contains "$1" 'including a standing request'
  contains "$1" 'do not turn ordinary numeric results into charts'
}
request_instructions() {
  jq -r '.instructions // ([.messages[] | select(.role == "developer" or .role == "system") | .content] | join("\n"))'
}
printf 'CUSTOM BASE\n' >"$work/base.md"
printf '[agent]\ndisable_chart_instructions=true\n' >"$root/disabled.ini"

# Offline dumps, literal overrides and reviewer defaults share the live policy.
run -z base.md -I USER_ADDITION -d "$root/default"; ok
for role in developer reviewer; do
  guide "$root/default/cai_${role}_instructions.md"
  contains "$root/default/cai_${role}_instructions.md" USER_ADDITION
done
awk -v marker="$marker" '$0 == marker { guide=1 } guide' "$root/default/cai_developer_instructions.md" >"$root/guide"
[[ $(wc -c <"$root/guide") -lt 1600 ]]
run --no-chart-instructions -z base.md -I USER_ADDITION -d "$root/disabled"; ok
{ cat "$work/base.md"; printf '\n\nUSER_ADDITION'; } >"$root/expected"
cmp "$root/expected" "$root/disabled/cai_developer_instructions.md"
absent "$root/disabled/cai_reviewer_instructions.md" "$marker"
run --state-dir "$root/disabled-state" --config "$root/disabled.ini" -d "$root/ini"; ok
absent "$root/ini/cai_developer_instructions.md" "$marker"
CAI_DISABLE_CHART_INSTRUCTIONS=false run --state-dir "$root/disabled-state" --config "$root/disabled.ini" -d "$root/env"; ok
absent "$root/env/cai_developer_instructions.md" "$marker"
CAI_DISABLE_CHART_INSTRUCTIONS=false run --no-chart-instructions -d "$root/cli"; ok
absent "$root/cli/cai_developer_instructions.md" "$marker"
CAI_DISABLE_CHART_INSTRUCTIONS=true run --chart-instructions -d "$root/explicit-on"; ok
guide "$root/explicit-on/cai_developer_instructions.md"
CAI_DISABLE_CHART_INSTRUCTIONS=invalid run -d "$root/invalid"; ok
guide "$root/invalid/cai_developer_instructions.md"
run --dump-config "$root/defaults.ini"; ok
contains "$root/defaults.ini" 'disable_chart_instructions = false'
[[ $(requests) == 0 ]]

# Live parent and isolated reviewer receive the guide once. Explicit additions
# follow it, and automatic capabilities follow those additions.
report=$(jq -c . "$support/fixtures/review.json")
plan "$(jq -nc --arg report "$report" --argjson chat "$chat" '{chat:$chat,plan:[{tool:"run_review",arguments:{target:"custom",instructions:"Inspect fixture"}},$report,"CHART POLICY READY"]}')"
run -Nni 'review fixture' -z base.md -I USER_ADDITION; ok
[[ $(requests) == 3 ]]
for index in 1 2 3; do
  sed -n "${index}p" "$root/peer/requests.jsonl" | request_instructions >"$root/request-$index"
  guide "$root/request-$index"
done
first=$(grep -n -m1 "$marker" "$root/request-1" | cut -d: -f1)
addition=$(grep -n -m1 USER_ADDITION "$root/request-1" | cut -d: -f1)
capability=$(grep -n -m1 '# CAI' "$root/request-1" | cut -d: -f1)
[[ $first -lt $addition && $addition -lt $capability ]]

# Reopening and changing model rebuilds guidance without duplicating it.
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["REOPENED CHART POLICY"]}')"
run -ni 'continue' -m gpt-6-astra; ok
tail -1 "$root/peer/requests.jsonl" | request_instructions >"$root/reopened"
guide "$root/reopened"
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["GUIDE DISABLED"]}')"
run --no-chart-instructions -Nni 'ordinary numbers'; ok
tail -1 "$root/peer/requests.jsonl" | request_instructions >"$root/without-guide"
absent "$root/without-guide" "$marker"

# Export reconstructs the same optional addition without provider traffic.
id=$(current_session_id)
before=$(requests)
run --export "$id" --export-out "$root/export.md"; ok
guide "$root/export.md"
run --no-chart-instructions --export "$id" --export-out "$root/export-off.md"; ok
absent "$root/export-off.md" "$marker"
[[ $(requests) == "$before" ]]

# Feed all three examples through the actual interactive libmdf renderer.
charts=$(awk '/^```mdf-/ { chart=1 } chart { print } chart && /^```$/ { chart=0; print "" }' "$root/guide")
plan "$(jq -nc --argjson chat "$chat" --arg charts "$charts" '{chat:$chat,plan:[$charts]}')"
terminal_start -N
send 'Show the requested charts\r'
expect 'Worked for'
contains "$term/feed" Build
contains "$term/feed" Used
absent "$term/feed" '```mdf-'
for row in Build,40 Test,25 Used,60 Free,40; do
  absent "$term/feed" "$row"
done
grep -qE '[█▉▊▋▌▍▎▏]' "$term/feed"
terminal_close

# An attaching client cannot silently change an existing owner's prompt policy.
plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:["OWNER CHART POLICY"]}')"
persistent=1
run --persist -Nni 'keep this owner'; ok
before=$(requests)
run --no-chart-instructions -ni 'must not be submitted'; bad
contains "$root/stderr" 'agent.disable_chart_instructions'
[[ $(requests) == "$before" ]]
run -X; ok
persistent=0
