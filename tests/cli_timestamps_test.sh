#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
export TZ=UTC0
printf 'timestamp fixture\n' >"$work/source.txt"
plan '{"plan":[{"reasoning":["**Checking**\n\nReasoning body."],"tool":"read_file","arguments":{"path":"source.txt"}},"TIMESTAMP ANSWER"]}'
terminal_start -N --utc
prompt 'timestamp task' 'TIMESTAMP ANSWER'
grep -Eq '^\* [0-9]{6}Z Read source.txt$' "$term/feed"
grep -Eq '^  [0-9]{6}Z$' "$term/feed"
grep -Eq '^! [0-9]{6}Z Worked for ' "$term/message"
grep -F $'\033[0;90m' "$term/raw" >/dev/null
! grep -F $'\033[0;2;90m' "$term/raw" >/dev/null
send '/status\r'; expect 'Status'
grep -Eq '^\$ [0-9]{6}Z /status$' "$term/feed"
send '/log\r'; expect '.log'
! sed -n '/^\$ [0-9]*Z \/status$/,$p' "$term/feed" | grep -Eq '^  [0-9]{6}Z$'
terminal_close
id=$(current_session_id)
run --export "$id" --export-out "$root/conversation.jsonl"; ok
jq -se 'any(.[]; .timestamp_ns? > 0 and .utc_offset_seconds == 0 and .type == "assistant_text_delta")' "$root/conversation.jsonl" >/dev/null
before=$(requests)
terminal_start --utc
expect 'TIMESTAMP ANSWER'
grep -Eq '^\* [0-9]{6}Z Read source.txt$' "$term/text"
grep -Eq '^  [0-9]{6}Z$' "$term/text"
terminal_close
[[ $(requests) == "$before" ]]
# INI, environment and argv use the same setting; explicit argv wins.
printf '[ui]\nutc=true\n' >"$root/utc.ini"
terminal_start -N --config "$root/utc.ini"
send '/status\r'; expect 'Status'
grep -Eq '^\$ [0-9]{6}Z /status$' "$term/feed"
terminal_close
export CAI_UTC=true
terminal_start -N
send '/status\r'; expect 'Status'
grep -Eq '^\$ [0-9]{6}Z /status$' "$term/feed"
terminal_close
terminal_start -N --no-utc
send '/status\r'; expect 'Status'
grep -Eq '^\$ [0-9]{6} /status$' "$term/feed"
terminal_close
