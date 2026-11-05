#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/pager.XXXXXX")
pager_cleanup() {
  local result=$?
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap pager_cleanup EXIT
base+=(--runtime-dir "$runtime_root")
no_publication() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
pager_close() {
  terminal_close
  # Last-client detach returns before the owner has finished closing storage.
  wait_for no_publication
}
mkdir "$root/programs"
cat >"$root/programs/less" <<'SH'
#!/bin/sh
printf 'EXTERNAL_LESS_SELECTED\n'
IFS= read -r reply
SH
chmod 700 "$root/programs/less"
base=(/usr/bin/env "PATH=$root/programs" "${base[@]}")
plan '{"plan":[]}'
native_opened() { tail -c +$((pager_offset+1)) "$term/raw" | grep -F '[?1049h' >/dev/null; }
native_log() {
  local log
  for log in "$root/cache/cai/"*.log; do
    printf '\n**LOG_LITERAL_SENTINEL**\n' >>"$log"
  done
  pager_offset=$(wc -c <"$term/raw")
  send '/log -p\r'; wait_for native_opened
  send G; expect '**LOG_LITERAL_SENTINEL**'
  send q; expect 'Pager closed.'
  tail -c +$((pager_offset+1)) "$term/raw" >"$root/view"
  absent "$root/view" EXTERNAL_LESS_SELECTED
  send '/status\r'; expect 'Session ID'
}
external_log() {
  send '/log less\r'; expect EXTERNAL_LESS_SELECTED
  send 'q\r'; expect 'Pager exited 0.'
}
terminal_start -N
send '/help\r'; expect 'Queue and approval keys'
for command in '/help -p' '/help less' '/help   less  '; do
  pager_offset=$(wc -c <"$term/raw")
  send "$command\r"; wait_for native_opened
  expect 'Your coding agent'
  tail -c +$((pager_offset+1)) "$term/raw" >"$root/help-view"
  absent "$root/help-view" EXTERNAL_LESS_SELECTED
  absent "$root/help-view" '# Help'
  send q; expect 'Pager closed.'
done
external_log
send '/help invalid\r'; expect 'Use /help'
send '/status\r'; expect 'Session ID'
pager_close

# Each frontend resolves its own pager preference. Explicit flags override
# environment/config without mutating the configured file or owner settings.
printf '[ui]\nprefer_native_pager = true\n' >"$root/pager.ini"
cp "$root/pager.ini" "$root/pager-original.ini"
terminal_start -N --config "$root/pager.ini"
native_log
pager_close
export CAI_PREFER_NATIVE_PAGER=false
terminal_start -N --config "$root/pager.ini"
external_log
pager_close
terminal_start -N --config "$root/pager.ini" --prefer-native-pager
native_log
pager_close
export CAI_PREFER_NATIVE_PAGER=true
terminal_start -N --no-prefer-native-pager
external_log
pager_close
unset CAI_PREFER_NATIVE_PAGER
rm "$root/programs/less"
terminal_start -N
native_log
pager_close
cmp "$root/pager.ini" "$root/pager-original.ini"
[[ $(requests) == 0 ]]
