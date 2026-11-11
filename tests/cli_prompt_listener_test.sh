#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
network_fixture=$1
feeder=$2
peer_start
port=$("$network_fixture" port)
plan '{"plan":["TCP FIRST ANSWER"]}'
terminal_start -N --prompt-listener "$port"
expect "TCP prompt listener: 127.0.0.1:$port"
# The executable utility must not read configuration, auth or state.
printf 'first line\nsecond line: € 😀\n' |
  CAI_CONFIG_FILE=/no/such/config CAI_HOME=/no/such/home "$feeder" --feed-tcp "$port" >"$root/feeder.jsonl" 2>&1
expect 'TCP FIRST ANSWER'
expect 'Worked for'
[[ $(requests) == 1 ]]
jq -se 'any(.[] | .. | objects; .text? == "first line\nsecond line: € 😀\n")' "$root/peer/requests.jsonl" >/dev/null
json "$root/feeder.jsonl" '.msg | contains("TCP")'
# A partial socket receipt is editable but Enter cannot submit its prefix.
plan '{"plan":["TCP SECOND ANSWER"]}'
exec 9<>"/dev/tcp/127.0.0.1/$port"
printf 'partial ' >&9
expect_prompt partial
send '\r'
[[ $(requests) == 1 ]]
send '\033\r'
[[ $(requests) == 1 ]]
# One active receipt owns the slot; a second must be rejected immediately.
printf 'MUST NOT ARRIVE' | "$feeder" --feed-tcp "$port" >"$root/rejected.jsonl" 2>&1 || :
printf '\342\202' >&9
printf '\254 complete' >&9
exec 9>&-
expect 'TCP SECOND ANSWER'
expect 'Worked for'
[[ $(requests) == 2 ]]
absent "$term/feed" 'MUST NOT ARRIVE'
# Invalid and incomplete UTF-8 never admit a prefix.
printf 'bad\300\200' | "$feeder" --feed-tcp "$port" >"$root/invalid.jsonl" || :
expect 'TCP prompt rejected'
printf 'truncated\360\237' | "$feeder" --feed-tcp "$port" >"$root/truncated.jsonl" || :
expect 'incomplete UTF-8'
[[ $(requests) == 2 ]]
# Empty receipts are inert; NUL and oversized receipts never admit a prefix.
"$feeder" --feed-tcp "$port" </dev/null >"$root/empty.jsonl"
send '/status\r'; expect 'Session ID'
send ''
printf 'bad\0suffix' | "$feeder" --feed-tcp "$port" >"$root/nul.jsonl" || :
expect 'TCP prompt rejected'
send ''
head -c 66049 /dev/zero | tr '\0' x | "$feeder" --feed-tcp "$port" >"$root/large.jsonl" || :
expect 'prompt size limit'
[[ $(requests) == 2 ]]
# Escape cancels a partial receipt, and its eventual close cannot submit it.
exec 9<>"/dev/tcp/127.0.0.1/$port"
printf 'CANCEL ME' >&9
expect_prompt 'CANCEL ME'
send '\033'
expect 'TCP prompt receipt cancelled'
exec 9>&-
[[ $(requests) == 2 ]]
# A detected reset discards already displayed text, just as a validation error.
exec 9<>"/dev/tcp/127.0.0.1/$port"
printf 'RESET PARTIAL' >&9
expect_prompt 'RESET PARTIAL'
"$network_fixture" reset-fd 9
exec 9>&-
expect 'TCP prompt receive failed'
[[ $(requests) == 2 ]]
# Preserve the local draft; a complete peer prompt uses the native turn queue.
send 'LOCAL DRAFT'
expect_prompt 'LOCAL DRAFT'
plan '{"plan":["TCP QUEUED ANSWER"]}'
printf 'REMOTE QUEUED' | "$feeder" --feed-tcp "$port" >"$root/queued.jsonl"
expect 'TCP QUEUED ANSWER'
expect_prompt 'LOCAL DRAFT'
# Validation failure in the separate queue path also preserves local input.
send ''
printf 'bad\0suffix' | "$feeder" --feed-tcp "$port" >"$root/queued-invalid.jsonl" || :
expect 'TCP prompt rejected'
expect_prompt 'LOCAL DRAFT'
send '\025'
terminal_close
# A fragmented scalar must still progress under paced insertion.
port=$("$network_fixture" port)
plan '{"plan":["PACED ANSWER"]}'
terminal_start -N --prompt-listener "$port" --simulate-typing 2
expect "TCP prompt listener: 127.0.0.1:$port"
exec 9<>"/dev/tcp/127.0.0.1/$port"
printf '\360' >&9
printf '\237\230\200 paced input' >&9
exec 9>&-
expect 'PACED ANSWER'
# Quit must discard unfinished remote text rather than save it in recall.
exec 9<>"/dev/tcp/127.0.0.1/$port"
printf 'NEVER RECALL THIS PARTIAL' >&9
expect_prompt 'NEVER RECALL THIS PARTIAL'
send '\033q'
wait_for test -s "$term/status"
wait "$terminal_pid" || fail 'Terminal CLI failed'
terminal_pid=
[[ $(cat "$term/status") == 0 && ! -s "$term/stderr" ]]
exec 7>&-
exec {terminal_event_fd}>&-
terminal_events=
unset CAI_TEST_TERMINAL_EVENT_FD
exec 9>&-
terminal_start
send '\033[A'
expect_prompt 'paced input'
absent "$term/prompt" 'NEVER RECALL'
terminal_close
logs
jq -se 'any(.[]; .event == "network_notice" and (.msg | contains("TCP prompt rejected")))' "$root/logs.jsonl" >/dev/null
