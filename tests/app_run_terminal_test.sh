#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/run.XXXXXX")
run_cleanup() {
  local result=$?
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap run_cleanup EXIT
export CAI_RUN_ENV=host CAI_RUN_REPORT="$root/report.json"
export SHELL="$work/run shell"
cat >"$SHELL" <<'SH'
#!/bin/sh
printf 'RUN_SHELL_READY\n'
exec /bin/sh -i
SH
chmod 700 "$SHELL"
printf 'config_env_has_precedence = true\n[env]\nCAI_RUN_ENV = configured\n' >"$root/run.ini"
base+=(--config "$root/run.ini" --runtime-dir "$runtime_root")
plan '{"plan":[{"tool":"host_fact","arguments":{}},"HOSTTOOLREPLY"]}'
terminal_start -N
prompt 'use the native host tool' HOSTTOOLREPLY
send '/status\r'; expect 'Session ID'
send 'UNSENT DRAFT\033s'
expect RUN_SHELL_READY
send 'printf "SHELL_CONFIG_%s\n" "$CAI_RUN_ENV"; exit\r'
expect 'SHELL_CONFIG_configured'; expect 'Shell exited 0.'
expect_prompt 'UNSENT DRAFT'
terminal_close
jq -e '.calls == 1 and .ready == 1 and .observations > 1' "$CAI_RUN_REPORT" >/dev/null
[[ $(requests) == 2 ]]
unset CAI_RUN_FAIL_START
plan '{"plan":["SYNCRESUMEREPLY"]}'
terminal_start
expect HOSTTOOLREPLY
prompt 'continue the synchronous owner' SYNCRESUMEREPLY
terminal_close
[[ $(requests) == 3 ]]
export CAI_RUN_FAIL_START=1
terminal_launch -N
wait_for test -s "$term/status"
peer_rc=0; wait "$terminal_pid" || peer_rc=$?; terminal_pid=
[[ $peer_rc == 1 && $(cat "$term/status") == 1 && $(requests) == 3 ]]
[[ -s "$CAI_RUN_REPORT" ]]
[[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]

# A temporary interactive run uses this same public owner/socket composition.
# The caller retains the original configuration/environment; private storage
# and discovery disappear only after the terminal and owner have both ended.
unset CAI_RUN_FAIL_START
for answer in '' n yes force eof; do
  flags=(--temp --persist)
  [[ "$answer" != force ]] || flags+=(--force)
  plan '{"plan":[{"tool":"host_fact","arguments":{}},"TEMPHOSTREPLY"]}'
  terminal_start "${flags[@]}"
  workspace=$(find "$root/cache/cai/temp" -path '*/private/state/pouch.key' -printf '%h\n' | sed 's,/private/state$,/workspace,')
  private="${workspace%/workspace}/private"
  [[ -d "$workspace" && -f "$private/state/pouch.key" ]]
  temp_runtime=$(find "$runtime_root" -mindepth 1 -maxdepth 1 -name 't-*' -type d)
  [[ -n "$temp_runtime" && -n $(find "$temp_runtime" -name 'ws-*.info' -print -quit) ]]
  [[ -z $(find "$runtime_root" -maxdepth 1 -name 'ws-*.info' -print -quit) ]]
  printf 'KEEP_TEMP_CONTENT\n' >"$workspace/kept.txt"
  prompt 'exercise the temporary owner tool' TEMPHOSTREPLY
  send '/status\r'; expect 'Session ID'
  send '/quit\r'
  if [[ "$answer" != force ]]; then
    wait_for has "$term/text" '[y/N]'
    # The question is outside Softline, after release of endpoint and stores.
    [[ ! -e "$term/status" ]]
    [[ ! -e "$temp_runtime" && ! -e "$private" ]]
    if [[ "$answer" == eof ]]; then printf '\004' >&7
    else printf '%s\n' "$answer" >&7; fi
  fi
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -d "$private" && ! -e "$temp_runtime" ]]
  if [[ "$answer" == yes || "$answer" == force ]]; then
    [[ ! -e "${workspace%/workspace}" ]]
  else
    [[ $(cat "$workspace/kept.txt") == KEEP_TEMP_CONTENT ]]
    contains "$term/text" 'Kept temporary workspace:'
  fi
  jq -e '.calls == 1 and .ready == 1' "$CAI_RUN_REPORT" >/dev/null
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
done

# Failure before READY must wake the terminal, skip the confirmation, and
# remove only its owned private tree. Explicit force also removes workspace.
export CAI_RUN_FAIL_START=1
terminal_launch --temp --force
wait_for test -s "$term/status"
peer_rc=0; wait "$terminal_pid" || peer_rc=$?; terminal_pid=
[[ $peer_rc == 1 && $(cat "$term/status") == 1 ]]
absent "$term/text" '[y/N]'
[[ -z $(find "$root/cache/cai/temp" -name private -o -name 'ws-*.info') ]]
