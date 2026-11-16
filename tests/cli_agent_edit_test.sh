#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export TMPDIR="$root/private-tmp"
mkdir -p "$TMPDIR"
export CAI_TEST_EDIT_LOG="$root/edit-path" CAI_TEST_EDIT_MODE=save
export EDITOR="$root/editor.sh --fixture"
unset VISUAL
cat >"$root/editor.sh" <<'EDITOR'
#!/bin/sh
set -eu
test "$1" = --fixture
shift
test "$#" = 3
directory=${1%/*}
printf '%s\n' "$directory" >"$CAI_TEST_EDIT_LOG"
test "$(stat -c %a "$directory")" = 700
test "$(stat -c %a "$1")" = 600
test ! -e "$directory/AUTH.json"
test ! -e "$directory/tools"
grep -q edit-private-key "$1"
printf 'editor backup\n' >"$directory/config.ini.bak"
mkdir -p "$directory/backups/nested"
printf 'editor recovery\n' >"$directory/backups/nested/config.ini"
case "$CAI_TEST_EDIT_MODE" in
  save) printf 'EDITED_AGENT_INSTRUCTIONS\n' >"$2"; printf 'EDITED_GLOBAL_INSTRUCTIONS\n' >"$3" ;;
  whitespace) printf ' \n\t\n' >"$3" ;;
  cancel) exit 1 ;;
  signal) kill -INT $$ ;;
  invalid) printf '[agent]\nunknown_field=true\n' >"$1" ;;
  rename) sed -i 's/name = "engineer"/name = "different"/' "$1" ;;
  symlink) rm "$2"; ln -s "$CAI_TEST_EXTERNAL" "$2" ;;
  conflict) "$CAI_TEST_EDIT_CLI" --no-config -C "$CAI_TEST_EDIT_WORK" --yes --make "$CAI_TEST_EDIT_SOURCE" ;;
  *) exit 90 ;;
esac
EDITOR
chmod +x "$root/editor.sh"
run --build "$root/engineer"; ok
cat >>"$root/engineer/config.ini" <<INI
[client]
api_key = edit-private-key
INI
mkdir -p "$root/engineer/files"
printf 'BUNDLED_UNCHANGED\n' >"$root/engineer/files/resource.txt"
run --make "$root/engineer"; ok
run --edit-agent engineer; ok
contains "$root/stdout" 'Updated agent engineer.'
[[ ! -e $(cat "$CAI_TEST_EDIT_LOG") ]]
[[ -z $(find "$TMPDIR" -mindepth 1 -print -quit) ]]
plan '{"plan":["EDITED_AGENT_WORKED"]}'
run --use engineer -Nni first; ok
contains "$root/peer/requests.jsonl" EDITED_AGENT_INSTRUCTIONS
contains "$root/peer/requests.jsonl" EDITED_GLOBAL_INSTRUCTIONS
contains "$root/peer/headers.jsonl" 'Bearer edit-private-key'

for mode in cancel signal invalid rename; do
  CAI_TEST_EDIT_MODE=$mode run --edit-agent engineer; bad
  [[ ! -e $(cat "$CAI_TEST_EDIT_LOG") ]]
  [[ -z $(find "$TMPDIR" -mindepth 1 -print -quit) ]]
done
printf 'EXTERNAL_MUST_SURVIVE\n' >"$root/external.md"
export CAI_TEST_EXTERNAL="$root/external.md"
CAI_TEST_EDIT_MODE=symlink run --edit-agent engineer; bad
contains "$root/external.md" EXTERNAL_MUST_SURVIVE
[[ -z $(find "$TMPDIR" -mindepth 1 -print -quit) ]]
CAI_TEST_EDIT_MODE=whitespace run --edit-agent engineer; ok
run --include-credentials --dump-agent engineer; ok
contains "$work/engineer/config.ini" edit-private-key
contains "$work/engineer/SYSTEM_PROMPT.md" EDITED_AGENT_INSTRUCTIONS
contains "$work/engineer/files/resource.txt" BUNDLED_UNCHANGED
[[ ! -e "$work/engineer/AGENTS.md" ]]
rm -rf "$work/engineer"

export CAI_TEST_EDIT_CLI="$cli" CAI_TEST_EDIT_WORK="$work" CAI_TEST_EDIT_SOURCE="$root/engineer"
printf 'CONCURRENT_VALID_INSTRUCTIONS\n' >"$root/engineer/SYSTEM_PROMPT.md"
CAI_TEST_EDIT_MODE=conflict run --edit-agent engineer; bad
contains "$root/stderr" 'agent changed while editing'
[[ -z $(find "$TMPDIR" -mindepth 1 -print -quit) ]]
run --dump-agent engineer; ok
contains "$work/engineer/SYSTEM_PROMPT.md" CONCURRENT_VALID_INSTRUCTIONS
[[ $(requests) == 1 ]]
