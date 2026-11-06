#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
guard=$(realpath "$1")
peer_start
plan '{"plan":["UTILITY_SEED"]}'
run -Nni 'räv 東京 utility source'; ok
session=$(current_session_id)
before=$(requests)
base=("$guard" --no-config -C "$work")
for flag in --help --list --list-all --resume --delete; do
  run "$flag"; ok
done
contains "$root/stdout" "$session"
contains "$root/stdout" 'räv 東京'
run --export "$session" --export-out result.md; ok
[[ -s "$work/result.md" && ! -e "$work/result.jsonl" ]]
run --export "$session" --export-out records.jsonl; ok
[[ -s "$work/records.jsonl" ]]
run --import records.jsonl; ok
imported=$(stdout_text)
[[ -n "$imported" && "$imported" != "$session" ]]
run --delete "$imported"; ok
run --export missing; bad
run --import missing.jsonl; bad
run --delete missing; bad
run --not-an-option; bad
mkdir -p "$work/package"
printf '%s\n' '---' 'name: utility-skill' 'description: A utility boundary test' '---' 'Body.' >"$work/package/SKILL.md"
run --install-skill package; ok
run --install-skill package; bad
run --install-skill package --force; ok
[[ -s "$root/config/cai/skills/utility-skill/SKILL.md" ]]
run --queue isolated-target --message 'utility only'; ok
jq -e '.status == "queued"' "$root/stdout" >/dev/null
printf 'config_env_has_precedence = true\n[env]\nUTILITY_BOUNDARY = configured\nLOG_LEVEL = error\n' >"$root/view.ini"
export UTILITY_BOUNDARY=host LOG_LEVEL=warn
base=("$guard" -C "$work")
run --config "$root/view.ini" --list; ok
# Logger settings apply to this invocation, and relative file sinks use its
# selected workspace. The guard aborts any transient process-state mutation.
mkdir "$work/logs"
printf 'config_env_has_precedence = true\n[env]\nLOG_OUTPUT = logs/utility.jsonl\nLOG_LEVEL = error\n' >"$root/log.ini"
run --config "$root/log.ini" --export missing; bad
contains "$work/logs/utility.jsonl" 'export conversation'
# Registration uses the captured configured home without exporting it globally.
printf 'config_env_has_precedence = true\n[env]\nCHROME_CONFIG_HOME = %s\n' "$root/browser-config" >"$root/browser.ini"
run --config "$root/browser.ini" --chrome-setup; ok
manifest="$root/browser-config/google-chrome/NativeMessagingHosts/com.openai.codexextension.json"
[[ -s "$manifest" ]]
run --config "$root/browser.ini" --chrome-remove; ok
[[ ! -e "$manifest" ]]
# Native-message errors cannot corrupt reserved stdout, even with LOG_OUTPUT.
rc=0
LOG_OUTPUT=stdout "$guard" --chrome-native-host >"$root/native.out" 2>"$root/native.err" || rc=$?
[[ "$rc" == 1 && ! -s "$root/native.out" && -s "$root/native.err" ]]
for selector in messaging_store sessions_store auth_store; do
  printf '[storage]\n%s = ""\n' "$selector" >"$root/invalid.ini"
  case "$selector" in
    messaging_store) run --config "$root/invalid.ini" --delete "$session" ;;
    sessions_store) run --config "$root/invalid.ini" --list ;;
    auth_store) run --config "$root/invalid.ini" --login ;;
  esac
  [[ "$rc" == 1 ]]
done
run --list; ok; contains "$root/stdout" "$session"
[[ $(requests) == "$before" ]]
[[ -z $(find "$root" -name 'ws-*.info' -o -name '*.sock') ]]
# Both calls must reach their stores concurrently with distinct [env] views.
base=("$guard" --no-config -C "$work")
export CAI_TEST_UTILITY_CONCURRENT=1
run --list; ok
unset CAI_TEST_UTILITY_CONCURRENT
