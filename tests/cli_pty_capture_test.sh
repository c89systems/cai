#!/usr/bin/env bash
# Unshared terminal handoffs stay private; explicit sharing records only a tail.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
controller=$(realpath "$1")
runtime_root=$(mktemp -d "$support/../build/pty.XXXXXX")
no_publication() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
pty_cleanup() {
  local result=$?
  if ! no_publication; then
    "$controller" "$runtime_root" "$work" stop >"$root/stop.out" 2>"$root/stop.err" || true
  fi
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap pty_cleanup EXIT
base+=(--runtime-dir "$runtime_root")
pty_close() {
  terminal_close
  wait_for no_publication
}
export SHELL="$root/local-shell"
cat >"$SHELL" <<'SHELL'
#!/bin/sh
if [ "$1" != -i ]; then exec /bin/sh "$@"; fi
test -t 0 && test -t 1 && test -t 2 || exit 77
stty -echo
printf 'LOCAL_PTY_READY\n'
exec /bin/sh -i
SHELL
chmod 700 "$SHELL"
# Even explicit sharing and generous retention settings cannot enable PTY capture.
terminal_start -N --shell-context --shell-context-max-bytes 4194304
for command in /shell '!sh'; do
  send "$command\r"; expect LOCAL_PTY_READY
  send 'printf "\\033[31mPRIVATE_PTY_RESULT\\033[0m\\n"; exit\r'
  expect 'Shell exited 0.'
  [[ $(requests) == 0 ]]
done
# A screen-oriented program can emit more than the journal parser's per-string
# ceiling. Its frames must not enter either the journal or model context.
send '/shell\r'; expect LOCAL_PTY_READY
send 'i=0; while [ "$i" -lt 2000 ]; do printf "\\033[31mPRIVATE_GAME_FRAME_%04d_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz\\033[0m\\n" "$i"; i=$((i+1)); done; exit\r'
expect 'Shell exited 0.'
plan '{"plan":["LocalHandoffDone"]}'
prompt 'continue after playing a terminal game' LocalHandoffDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" PRIVATE_PTY_RESULT
absent "$root/request" PRIVATE_GAME_FRAME
absent "$root/request" '<user_shell_command>'
# The ordinary pipe command path still shares its results.
send '!printf SHARED_PIPE_RESULT\r'; expect 'Shell command exited 0'
plan '{"plan":["PipeContextDone"]}'
prompt 'inspect the pipe command result' PipeContextDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" SHARED_PIPE_RESULT
pty_close
plan '{"plan":["LocalReplayDone"]}'
terminal_start
prompt 'continue after resuming' LocalReplayDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
absent "$root/request" PRIVATE_PTY_RESULT
absent "$root/request" PRIVATE_GAME_FRAME
pty_close
run --list-all; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
run --export "$id"; ok
journal="$root/data/cai/exports/$id/$id.jsonl"
absent "$journal" PRIVATE_PTY_RESULT
absent "$journal" PRIVATE_GAME_FRAME
contains "$journal" SHARED_PIPE_RESULT
logs
absent "$root/logs.jsonl" PRIVATE_PTY_RESULT
absent "$root/logs.jsonl" PRIVATE_GAME_FRAME
# The owner logs accepted commands; local PTY lifetime belongs to the frontend.
jq -se 'any(.[]; .event == "command_input" and .msg == "/shell")' "$root/logs.jsonl" >/dev/null

# Explicit opt-in aliases share a rolling tail even when ordinary bang sharing
# is disabled. Both use real PTYs and never persist the discarded raw stream.
terminal_start -N --no-shell-context
for command in /shellshare '!sh+'; do
  send "$command\r"; expect LOCAL_PTY_READY
  send 'printf "\\033[32mSHARED_PTY_RESULT_€\\033[0m\\n"; exit\r'
  expect 'Shell exited 0.'
done
send '/status\r'; expect 'Session ID'
[[ $(grep -o SHARED_PTY_RESULT_ "$term/text" | wc -l) == 2 ]]
send '/shellshare\r'; expect LOCAL_PTY_READY
send 'printf PTY_DISCARDED_HEAD; i=0; while [ "$i" -lt 2000 ]; do printf "\\033[31mSHARED_FRAME_%04d_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz\\033[0m\\n" "$i"; i=$((i+1)); done; printf PTY_NEWEST_SUFFIX; exit\r'
expect 'Shell exited 0.'
plan '{"plan":["SharedPtyDone"]}'
prompt 'inspect the shared terminal tail' SharedPtyDone
[[ $(grep -o PTY_NEWEST_SUFFIX "$term/text" | wc -l) == 1 ]]
tail -1 "$root/peer/requests.jsonl" >"$root/request"
jq -e '(.input // .messages) | tostring | contains("SHARED_PTY_RESULT_€")' "$root/request" >/dev/null
contains "$root/request" PTY_NEWEST_SUFFIX
contains "$root/request" SHARED_FRAME_1999
absent "$root/request" PTY_DISCARDED_HEAD
absent "$root/request" SHARED_FRAME_0000
jq -e '(.input // .messages) | tostring | contains("\u001b") | not' "$root/request" >/dev/null
cat >"$work/tail-output.sh" <<'OUTPUT'
#!/bin/sh
printf PIPE_DISCARDED_HEAD
i=0
while [ "$i" -lt 2000 ]; do
  printf 'PIPE_FRAME_%04d_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz_abcdefghijklmnopqrstuvwxyz\n' "$i"
  i=$((i+1))
done
printf PIPE_NEWEST_SUFFIX
OUTPUT
chmod 700 "$work/tail-output.sh"
send '/shell-context on\r'; expect 'shared with the model'
send '!./tail-output.sh\r'; expect 'Shell command exited 0'
plan '{"plan":["SharedPipeTailDone"]}'
prompt 'inspect the shared pipe tail' SharedPipeTailDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" PIPE_NEWEST_SUFFIX
contains "$root/request" PIPE_FRAME_1999
absent "$root/request" PIPE_DISCARDED_HEAD
absent "$root/request" PIPE_FRAME_0000
pty_close
run --list-all; ok
id=$(awk 'NR == 1 {print $1}' "$root/stdout")
run --export "$id"; ok
journal="$root/data/cai/exports/$id/$id.jsonl"
absent "$journal" PTY_DISCARDED_HEAD
absent "$journal" PIPE_DISCARDED_HEAD
contains "$journal" PTY_NEWEST_SUFFIX
contains "$journal" PIPE_NEWEST_SUFFIX
jq -se 'all(.[]; .type != "user_shell_output_hex") and all(.[]; if .data? then (.data|utf8bytelength) <= 131072 else true end)' "$journal" >/dev/null
logs
absent "$root/logs.jsonl" PTY_DISCARDED_HEAD
absent "$root/logs.jsonl" PIPE_DISCARDED_HEAD
plan '{"plan":["SharedTailReplayDone"]}'
terminal_start
expect PTY_NEWEST_SUFFIX
expect PIPE_NEWEST_SUFFIX
absent "$term/text" 'Skipped oversized session event'
prompt 'continue after shared shell replay' SharedTailReplayDone
[[ $(grep -o PTY_NEWEST_SUFFIX "$term/text" | wc -l) == 1 ]]
pty_close
