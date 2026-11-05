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
