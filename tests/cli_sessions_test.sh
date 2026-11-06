#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/cli_table_assertions.sh"

cli=$1
build_dir=$2
fixture=$(mktemp -d "$build_dir/cli-sessions.XXXXXX")
session_cleanup() {
  local result=$?
  if [[ "$result" == 0 ]]; then
    rm -rf "$fixture"
  else
    printf 'Session fixture retained: %s\n' "$fixture" >&2
    for output in out err error; do
      [[ ! -f "$fixture/$output" ]] || cat "$fixture/$output" >&2
    done
  fi
}
trap session_cleanup EXIT
mkdir -p "$fixture/home" "$fixture/work-a" "$fixture/work-b" "$fixture/state"
export HOME="$fixture/home" XDG_CONFIG_HOME="$fixture/config" XDG_STATE_HOME="$fixture/state" XDG_CACHE_HOME="$fixture/cache" XDG_DATA_HOME="$fixture/data" OPENAI_API_KEY=fixture CAI_INSECURE=true CAI_MCP=false

first=$(printf '/status\n/resume\n/quit\n' | "$cli" -p openai -N -C "$fixture/work-a")
[[ "$first" == *"Status"* ]]
table_contains /dev/stdin 1 Model <<<"$first"
table_contains /dev/stdin 1 'Reasoning effort' <<<"$first"
[[ "$first" == *"Conversations"* ]]
first_id=$(printf '%s\n' "$first" | sed -n 's/^[[:space:]]*1  \([^ ]*\).*/\1/p')
[[ -n "$first_id" ]]
[[ -f "$fixture/state/cai/pouch.key" && -d "$fixture/state/cai/pouch" ]]
[[ ! -d "$fixture/state/cai/sessions" && ! -f "$fixture/state/cai/auth.json" ]]

"$cli" --export "$first_id" -C "$fixture/work-a" >/dev/null
journal="$fixture/data/cai/exports/$first_id/$first_id.jsonl"
# Startup/command diagnostics already occupy event IDs in the export. Append
# fresh ordered IDs rather than conflicting with existing collection entries.
sequence=$(jq -s '[.[] | select(.record_type == "event") | .sequence] | max // 0' "$journal")
jq -cn --argjson sequence "$sequence" \
  '{record_type:"event",sequence:($sequence+1),type:"assistant_text_delta",data:"Saved assistant answer"},
   {record_type:"event",sequence:($sequence+2),type:"assistant_text_end",data:null}' >>"$journal"
imported_id=$("$cli" --import "$journal" -C "$fixture/work-a")
[[ -n "$imported_id" && "$imported_id" != "$first_id" ]]
resumed=$(printf '/resume\n/quit\n' | "$cli" -p openai -C "$fixture/work-a")
[[ "$resumed" == *"1  $imported_id"* && "$resumed" == *"Saved assistant answer"* ]]

second=$(printf '/resume\n/quit\n' | "$cli" -p openai --new -C "$fixture/work-a")
second_id=$(printf '%s\n' "$second" | sed -n 's/^[[:space:]]*1  \([^ ]*\).*/\1/p')
[[ -n "$second_id" && "$second_id" != "$imported_id" ]]
[[ "$second" == *"2  $imported_id"* ]]
selected=$(printf '/resume 2\n/quit\n' | "$cli" -p openai -C "$fixture/work-a")
[[ "$selected" == *"Saved assistant answer"* ]]
exact=$(printf '/quit\n' | "$cli" -p openai --resume "$imported_id" -C "$fixture/work-a")
[[ "$exact" == *"Saved assistant answer"* ]]
isolated=$(printf '/resume\n/quit\n' | "$cli" -p openai --new -C "$fixture/work-b")
isolated_id=$(printf '%s\n' "$isolated" | sed -n 's/^[[:space:]]*1  \([^ ]*\).*/\1/p')
[[ -n "$isolated_id" ]]
[[ "$isolated" != *"$first_id"* && "$isolated" != *"$second_id"* && "$isolated" != *"$imported_id"* ]]
for flag in -l --list; do
  local_rows=$("$cli" "$flag" -C "$fixture/work-b")
  [[ "$local_rows" != *"$first_id"* && "$local_rows" != *"$second_id"* && "$local_rows" != *"$imported_id"* ]]
  [[ "$local_rows" == *"$isolated_id"* ]]
done
all=$("$cli" -L -C "$fixture/work-b")
[[ "$all" == *"$first_id"* && "$all" == *"$second_id"* && "$all" == *"$imported_id"* ]]
[[ "$("$cli" --list-all -C "$fixture/work-b")" == "$all" ]]
[[ "$("$cli" -L --state-dir "$fixture/another-state" -C "$fixture/work-b")" == 'No conversations.' ]]
numbered=$("$cli" --resume -C "$fixture/work-a")
[[ "$("$cli" -l -C "$fixture/work-a")" == "$numbered" ]]
[[ "$numbered" == *"1  $second_id"* && "$numbered" == *"2  $imported_id"* ]]
[[ "$numbered" != *"Conversations"* ]]
slash_export=$(printf '/export\n/quit\n' | "$cli" -p openai -C "$fixture/work-b")
[[ "$slash_export" == *".jsonl"* && "$slash_export" == *".md"* && "$slash_export" != *"error"* ]]
[[ "$slash_export" == *"Conversation export"* ]]
if printf '/quit\n' | "$cli" -p openai --resume "$first_id" -C "$fixture/work-b" >"$fixture/out" 2>"$fixture/err"; then
  echo 'cross-directory resume was accepted' >&2
  exit 1
fi
grep -q 'another directory' "$fixture/err"

mkdir "$fixture/work-\`metadata\`-**literal**"
created=$(printf '/new\n/resume\n/quit\n' | "$cli" -p openai -C "$fixture/work-\`metadata\`-**literal**")
created_id=$(printf '%s\n' "$created" | sed -n 's/^[[:space:]]*1  \([^ ]*\).*/\1/p')
[[ -n "$created_id" ]]
if [[ "$created" != *"New session"* || "$created" != *"ID: $created_id"* ]]; then
  printf 'Missing confirmation for new session %s\n' "$created_id" >&2
  exit 1
fi
[[ "$created" == *"Workspace:"* && "$created" == *'work-`metadata`-**literal**'* ]]
[[ "$created" != *'**ID:'* && "$created" != *'Message queue:'* ]]
# The plain socket producer prints command results, without interactive input
# echoes. The actual new-session ID/workspace confirmation is checked above.
[[ "$created" != *'$ /new'* && "$created" != *'--- session ---'* ]]
[[ "$selected" != *'--- session ---'* ]]

# Resolve CLI numbers from the current directory's numbered listing.
for option in --resume -R; do
  rows=$("$cli" --resume -C "$fixture/work-a")
  row=$(awk -v id="$imported_id" '$2 == id {print $1}' <<<"$rows")
  [[ -n "$row" ]]
  resumed=$(printf '/quit\n' | "$cli" -p openai "$option" "$row" -C "$fixture/work-a")
  [[ "$resumed" == *'Saved assistant answer'* ]]
done
mkdir "$fixture/work-empty"
for choice in 0 999999999999999999999999999999999999 missing-session-id; do
  if printf '/quit\n' | "$cli" -p openai --resume "$choice" -C "$fixture/work-a" >"$fixture/error" 2>&1; then
    echo 'invalid resume selection was accepted' >&2; exit 1
  fi
  if [[ "$choice" == missing-session-id ]]; then
    grep -q 'was not found' "$fixture/error"
  else
    grep -q 'No conversation with that number' "$fixture/error"
  fi
  ! grep -q 'incomplete JSON document' "$fixture/error"
done
if printf '/quit\n' | "$cli" -p openai --resume 1 -C "$fixture/work-empty" >"$fixture/error" 2>&1; then
  echo 'numeric resume escaped its directory' >&2; exit 1
fi
grep -q 'No conversation with that number' "$fixture/error"
