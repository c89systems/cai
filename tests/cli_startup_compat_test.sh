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
  absent "$term/text" 'Session ready'
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
reject paths.clipboard_dir --clipboard-dir "$root/other-clipboard"
reject paths.export_dir --export-dir "$root/other-exports"
reject agent.shell_context_max_lines --shell-context-max-lines 19
reject agent.shell_context_max_bytes --shell-context-max-bytes 4096
reject messaging.ack_timeout_ms --message-ack-timeout-ms 17
reject messaging.delay_seconds --message-delay-seconds 17
[[ ! -e "$root/other-clipboard" && ! -e "$root/other-exports" ]]
printf '[ui]\ntheme=gruvbox\n' >"$root/other.ini"
reject paths.config_file --config "$root/other.ini"
# Same canonical config, matching explicit values, and frontend-only theme
# changes must remain valid. No failed attempt may have submitted startup text.
terminal_start --config "$root/config/cai/config.ini" --state-dir "$root/state/cai" --no-plugins --theme gruvbox \
  --shell-context-max-lines 5000 --shell-context-max-bytes 4194304 \
  --message-ack-timeout-ms 300000 --clipboard-dir "$root/cache/cai/clipboard" \
  --export-dir "$root/data/cai/exports"
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

# Supplementary INI values are checked per field, rather than fingerprinting
# an entire profile: a matching partial profile must attach successfully.
profile="$root/config/cai/config.ini"
printf '[client]\ntimeout_ms=12345\nca_bundle_path=trust/ca.pem\napi_key=owner-private-token\n[mcp.team]\nenabled=false\nendpoint=http://127.0.0.1:1/mcp\ntimeout_ms=24000\n[review]\nallowed_models=first\nallowed_models=second\n[env]\nCAI_COMPAT_PRIVATE=owner-environment\n' >"$profile"
"${owner_base[@]}" -x -N --shell-context-max-lines 7000 >"$root/extended.out" 2>"$root/extended.log" & owner_pid=$!
wait_for has "$root/extended.log" 'Workspace server ready'
printf '[client]\ntimeout_ms=00012345\nca_bundle_path=trust/ca.pem\n[ui]\ntheme=gruvbox\n' >"$profile"
terminal_start
send '/status\r'; expect 'Session ID'
terminal_close
printf '[client]\ntimeout_ms=12346\n' >"$profile"
reject client.timeout_ms
printf '[runtime]\nshell_context_max_lines=5000\n' >"$profile"
reject agent.shell_context_max_lines
printf '[client]\nca_bundle_path=other/ca.pem\n' >"$profile"
reject client.ca_bundle_path
printf '[client]\napi_key=other-private-token\n' >"$profile"
reject client.api_key
absent "$term/stderr" owner-private-token
absent "$term/stderr" other-private-token
printf '[mcp.team]\nendpoint=http://127.0.0.1:2/mcp\n' >"$profile"
reject mcp.team.endpoint
printf '[mcp.team]\ntimeout_ms=24001\n' >"$profile"
reject mcp.team.timeout_ms
printf '[mcp.unknown]\nendpoint=http://127.0.0.1:1/mcp\n' >"$profile"
reject mcp.unknown.endpoint
printf '[review]\nallowed_models=second\nallowed_models=first\n' >"$profile"
reject review.allowed_models
printf '[env]\nCAI_COMPAT_PRIVATE=other-environment\n' >"$profile"
reject env.CAI_COMPAT_PRIVATE
absent "$term/stderr" owner-environment
absent "$term/stderr" other-environment
# Empty optional paths/default numeric fields compare like the owner's schema.
printf '[client]\nca_path=""\nhttp_2_disabled=false\n[runtime]\nshell_context_max_lines=7000\n[env]\nCAI_COMPAT_PRIVATE=owner-environment\n[mcp.team]\ntimeout_ms=24000\n' >"$profile"
terminal_start
send '/status\r'; expect 'Session ID'
terminal_close
kill -TERM "$owner_pid"
wait "$owner_pid"
owner_pid=
