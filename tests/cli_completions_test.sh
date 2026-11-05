#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
attach_options=()
mkdir -p "$root/skills/completion-skill" "$work/folder space" "$work/.git"
printf '%s\n' '---' 'name: completion-skill' 'description: Completion fixture.' '---' 'Skill instructions.' >"$root/skills/completion-skill/SKILL.md"
printf 'file\n' >"$work/report-unique.txt"
printf 'child\n' >"$work/folder space/deep-file.txt"
printf 'hidden\n' >"$work/.hidden-file"
ln -s 'folder space' "$work/linked-folder"
if [[ ${1:-local} != local ]]; then
  completion_runtime=$(mktemp -d "$support/../build/cmp.XXXXXX")
  base+=(--runtime-dir "$completion_runtime")
  export LOG_LEVEL=info
  for ((skill=1; skill<256; ++skill)); do
    printf -v skill_name 'filler-%03d' "$skill"
    mkdir "$root/skills/$skill_name"
    printf '%s\n' '---' "name: $skill_name" 'description: Filler.' '---' 'Only loaded on demand.' >"$root/skills/$skill_name/SKILL.md"
  done
  completion_owner=
  completion_cleanup() {
    local result=$?
    if [[ -n "$completion_owner" ]]; then
      kill -TERM "$completion_owner" 2>/dev/null || true
      wait "$completion_owner" 2>/dev/null || true
    fi
    if [[ $result == 0 ]]; then rm -rf -- "$completion_runtime"; fi
    cleanup "$result"
  }
  trap completion_cleanup EXIT
  "${base[@]}" -x -N --workspace-name completion-owner --skills --skills-dir "$root/skills" >"$root/owner.out" 2>"$root/owner.log" & completion_owner=$!
  wait_for has "$root/owner.log" 'Workspace server ready'
  if [[ $1 == embed ]]; then
    base=("$(realpath "$2")" "$completion_runtime" "$work" "${base[@]:1}")
  else
    # Initial named discovery resolves outside the client's provisional cwd.
    # File completions must use the verified owner's directory throughout.
    attach_options=(-C "$root" -s completion-owner)
  fi
fi
terminal_start -N --skills --skills-dir "$root/skills" "${attach_options[@]}"
if [[ ${1:-local} != local ]]; then
  # Only the owner has the loaded catalog. A frontend scan now sees a decoy.
  mv "$root/skills" "$root/server-only-skills"
  mkdir -p "$root/skills/client-only-decoy"
  printf '%s\n' '---' 'name: client-only-decoy' 'description: Must not be discovered by the frontend.' '---' >"$root/skills/client-only-decoy/SKILL.md"
fi
# Complete real slash commands, retaining the slash required by dispatch.
send '/stat\t\r'; expect_table 1 'Session ID'
send '/ski\t\r'; expect 'completion-skill'
[[ $(requests) == 0 ]]
last_input() {
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  jq -er '[.input[]? | select(.role=="user") | .content[]? | .text?] | last' "$root/request"
}
plan '{"plan":["FILE COMPLETED"]}'
prompt 'inspect @report-\t' 'FILE COMPLETED'
[[ $(last_input) == 'inspect report-unique.txt' ]]
# Spaces remain part of a matching path. Tab resolves only the selected
# directory's children, and the native editor removes the leading @.
plan '{"plan":["DIRECTORY COMPLETED"]}'
prompt 'inspect @fold\t\t' 'DIRECTORY COMPLETED'
[[ $(last_input) == 'inspect folder space/deep-file.txt' ]]
plan '{"plan":["SKILL COMPLETED"]}'
prompt 'Use $comple\t' 'SKILL COMPLETED'
[[ $(last_input) == 'Use completion-skill' ]]
if [[ ${1:-local} != local ]]; then
  plan '{"plan":["OWNER CATALOG CHECKED"]}'
  prompt 'Use $client-\t' 'OWNER CATALOG CHECKED'
  [[ $(last_input) == 'Use client-' ]]
fi
plan '{"plan":["HIDDEN COMPLETED"]}'
prompt 'inspect @.hid\t' 'HIDDEN COMPLETED'
[[ $(last_input) == 'inspect .hidden-file' ]]
# Directory symlinks are names, not recursive traversal requests.
plan '{"plan":["SYMLINK COMPLETED"]}'
prompt 'inspect @linked-\t' 'SYMLINK COMPLETED'
[[ $(last_input) == 'inspect linked-folder' ]]
# Enter submits actual text, never an unaccepted completion ghost.
plan '{"plan":["NO GHOST SUBMITTED"]}'
prompt 'inspect @report-' 'NO GHOST SUBMITTED'
[[ $(last_input) == 'inspect report-' ]]
plan '{"plan":["LITERAL TEXT KEPT"]}'
prompt 'email example@host.test costs$10' 'LITERAL TEXT KEPT'
[[ $(last_input) == 'email example@host.test costs$10' ]]
# A new activation refreshes the directory; it does not reuse a stale scan.
printf 'fresh\n' >"$work/fresh-after-start.txt"
plan '{"plan":["FRESH COMPLETED"]}'
prompt 'inspect @fresh-\t' 'FRESH COMPLETED'
[[ $(last_input) == 'inspect fresh-after-start.txt' ]]
# Catalog completion remains available during a live provider response.
plan '{"plan":[{"pause":true,"text":"STREAM COMPLETED"}]}'
send 'hold a response\r'; expect 'STREAM in flight'
send 'Use $comple\t'; expect_prompt 'Use $completion-skill'
send '\025'
release; expect 'STREAM COMPLETED'; expect 'Worked for'
terminal_close
