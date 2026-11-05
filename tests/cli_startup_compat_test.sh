#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
terminal_host=$(realpath "$1")
mode=${2:-native}
runtime_root=$(mktemp -d "$support/../build/cmpat.XXXXXX")
base+=(--runtime-dir "$runtime_root")
owner_base=("${base[@]}")
owner_pid=
compat_cleanup() {
  local result=$?
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap compat_cleanup EXIT
export LOG_LEVEL=info
mkdir -p "$root/config/cai"
# This changes instruction/config roots, never the file that was loaded. An
# explicit attachment to that same original file must still match its owner.
printf '[paths]\nconfig_dir=%s/instructions\n' "$root" >"$root/config/cai/config.ini"
"${base[@]}" -x -N --no-plugins --workspace-name compatibility >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'
if [[ $mode == embed ]]; then
  base=("$terminal_host" "$runtime_root" "$work" "${base[@]:1}")
fi
reject() {
  local setting=$1
  local before_requests=$(requests)
  shift
  terminal_launch "$@"
  wait_for test -s "$term/status"
  wait "$terminal_pid" || :
  terminal_pid=
  [[ $(cat "$term/status") != 0 ]]
  has "$term/stderr" "$setting" || has "$term/text" "$setting" ||
    fail "Missing startup conflict: $setting"
  absent "$term/text" 'Conversation attached'
  kill -0 "$owner_pid"
  [[ $(requests) == "$before_requests" ]]
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
  unset CAI_TEST_TERMINAL_EVENT_FD
}
reject paths.state_dir --state-dir "$root/other-state"
[[ ! -e "$root/other-state" ]]
reject storage.sessions_store --sessions-store alternate
reject storage.namespace_prefix --namespace-prefix another
reject plugins.enabled --plugins
printf '[ui]\ntheme=gruvbox\n' >"$root/other.ini"
reject paths.config_file --config "$root/other.ini"
# Same canonical config, matching explicit values, and frontend-only theme
# changes must remain valid. No failed attempt may have submitted startup text.
terminal_start --config "$root/config/cai/config.ini" --state-dir "$root/state/cai" --no-plugins --theme gruvbox
send '/status\r'; expect 'Session ID'
terminal_close
plan '{"plan":["STARTUP VALUE PRESERVED"]}'
terminal_start -i '--state-dir'
expect 'STARTUP VALUE PRESERVED'
[[ $(requests) == 1 ]]
terminal_close
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=

# A store's label alone does not identify its root/key. Reject a changed
# definition even when the explicit INI path and selected label are unchanged.
printf '[paths]\ndirectory=%s\n[storage]\nsessions_store=team\n[store.team]\nendpoint=pouch://%s/team-pouch\nkey_file=team.key\n' "$work" "$root" >"$root/team.ini"
"${owner_base[@]}" -x -N --config "$root/team.ini" >"$root/team.out" 2>"$root/team.log" & owner_pid=$!
wait_for has "$root/team.log" 'Workspace server ready'
[[ -f "$root/team.key" && ! -e "$work/team.key" ]]
terminal_start --config "$root/team.ini"
send '/status\r'; expect 'Session ID'
terminal_close
if [[ $mode == native ]]; then
  saved_base=("${base[@]}")
  # Fixture base ends in -C WORK --runtime-dir RUNTIME. Remove only -C so
  # the relative config is loaded here, then redirects to its own workspace.
  base=("${base[@]:0:${#base[@]}-4}" --runtime-dir "$runtime_root")
  previous_directory=$PWD
  cd "$root"
  terminal_start --config team.ini
  send '/status\r'; expect 'Session ID'
  terminal_close
  cd "$previous_directory"
  base=("${saved_base[@]}")
fi
printf '[paths]\ndirectory=%s\n[storage]\nsessions_store=team\n[store.team]\nendpoint=pouch://%s/other-team-pouch\nkey_file=team.key\n' "$work" "$root" >"$root/team.ini"
reject storage.sessions_store --config "$root/team.ini"
[[ ! -e "$root/other-team-pouch" ]]
printf '[paths]\ndirectory=%s\n[storage]\nsessions_store=team\n[store.team]\nendpoint=pouch://%s/team-pouch\nkey_file=another.key\n' "$work" "$root" >"$root/team.ini"
reject storage.sessions_store --config "$root/team.ini"
[[ ! -e "$root/another.key" ]]
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=
