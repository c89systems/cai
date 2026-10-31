#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["WORKER STACK OK"]}'
run -Nni 'first embedded worker turn'; ok
contains "$root/stdout" 'WORKER STACK OK'
id=$(current_session_id)
[[ -n "$id" ]]
plan '{"plan":["WORKER RESUMED"]}'
run --resume "$id" -ni 'second worker turn'; ok
contains "$root/stdout" 'WORKER RESUMED'
run --export "$id"; ok
contains "$root/data/cai/exports/$id/$id.md" 'WORKER STACK OK'
terminal_start --resume "$id"
send '/status\r'; expect "$id"
terminal_close
plan '{"plan":["TEMP WORKER OK"]}'
run --temp --yes -Nni 'temporary embedded worker'; ok
contains "$root/stdout" 'TEMP WORKER OK'
