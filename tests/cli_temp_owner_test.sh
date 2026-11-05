#!/usr/bin/env bash
# Exercise the shipped executable, including config-only temporary selection
# that the preliminary argv parser cannot see. All roots belong to this test.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/tmp-owner.XXXXXX")
temp_owner_cleanup() {
  local result=$?
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap temp_owner_cleanup EXIT
export CAI_RUNTIME_DIR="$runtime_root"
printf 'RETAINED_TEMP_POLICY\n' >"$work/policy.md"
config="$root/temp.ini"
printf '[ui]\ntemporary = true\n[server]\npersist = true\n' >"$config"
cp "$config" "$root/config-before"
base+=(--config "$config" --agents-md policy.md)
for selection in argv config; do
  flags=()
  [[ "$selection" != argv ]] || flags+=(--temp)
  plan '{"plan":["TEMPOWNERANSWER"]}'
  terminal_start "${flags[@]}"
  workspace=$(find "$root/cache/cai/temp" -path '*/private/state/pouch.key' -printf '%h\n' | sed 's,/private/state$,/workspace,')
  private="${workspace%/workspace}/private"
  [[ -d "$workspace" ]]
  socket=$(find "$runtime_root" -type s -print)
  [[ "$socket" == "$runtime_root"/t-*/*.sock ]]
  prompt 'exercise retained temporary policy' TEMPOWNERANSWER
  contains "$root/peer/requests.jsonl" RETAINED_TEMP_POLICY
  send '!pwd; printf KEEP_FILE >kept.txt\r'
  expect 'Shell command exited 0'
  wait_for test -f "$workspace/kept.txt"
  send '/status\r'; expect 'Log file'
  [[ -n $(find "$private/cache/logs" -name '*.log' -print -quit) ]]
  send '/quit\r'
  wait_for has "$term/text" '[y/N]'
  [[ ! -e "$term/status" && ! -e "$socket" && ! -e "$private" ]]
  [[ -z $(find "$runtime_root" -mindepth 1 -print -quit) ]]
  if [[ "$selection" == argv ]]; then printf '\n' >&7
  else printf 'yes\n' >&7; fi
  wait_for test -s "$term/status"
  wait "$terminal_pid"; terminal_pid=
  [[ $(cat "$term/status") == 0 && ! -e "$private" && ! -s "$term/stderr" ]]
  if [[ "$selection" == argv ]]; then
    [[ $(cat "$workspace/kept.txt") == KEEP_FILE ]]
  else
    [[ ! -e "${workspace%/workspace}" ]]
  fi
  cmp "$config" "$root/config-before"
  [[ ! -e "$root/state/cai/pouch" && ! -e "$root/state/cai/pouch.key" ]]
  exec 7>&-
  exec {terminal_event_fd}>&-
  terminal_events=
done
[[ $(requests) == 2 ]]

# Once the plain question appears, privacy cleanup is already complete. Even
# the executable's normal SIGINT disposition cannot leave history behind or
# delete workspace files without an affirmative answer.
plan '{"plan":[]}'
export CAI_TEST_TERMINAL_DEFAULT_SIGINT=1
terminal_start --temp
workspace=$(find "$root/cache/cai/temp" -path '*/private/state/pouch.key' -printf '%h\n' | sed 's,/private/state$,/workspace,')
printf 'INTERRUPTED_KEEP\n' >"$workspace/kept.txt"
send '/quit\r'
wait_for has "$term/text" '[y/N]'
[[ ! -e "${workspace%/workspace}/private" ]]
[[ -z $(find "$runtime_root" -mindepth 1 -print -quit) ]]
printf '\003' >&7
wait_for test -s "$term/status"
peer_rc=0; wait "$terminal_pid" || peer_rc=$?; terminal_pid=
[[ $peer_rc == 130 && $(cat "$term/status") == 130 ]]
[[ $(cat "$workspace/kept.txt") == INTERRUPTED_KEEP ]]
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_DEFAULT_SIGINT

# A socket-path limit fails before READY, reports the real error, and cannot
# strand a private owner, history tree or cleanup question. Force still applies.
long_runtime="$runtime_root/this-runtime-component-is-deliberately-too-long-for-a-unix-socket"
terminal_launch --temp --force --runtime-dir "$long_runtime"
wait_for test -s "$term/status"
peer_rc=0; wait "$terminal_pid" || peer_rc=$?; terminal_pid=
[[ $peer_rc == 1 && $(cat "$term/status") == 1 && $(requests) == 2 ]]
absent "$term/text" '[y/N]'
contains "$term/stderr" 'Workspace socket path exceeds Unix limit'
[[ -z $(find "$root/cache/cai/temp" -name private -print -quit) ]]
[[ -z $(find "$long_runtime" -mindepth 1 -print -quit) ]]
