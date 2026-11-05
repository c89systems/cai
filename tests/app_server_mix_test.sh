#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
runtime_root=$(mktemp -d "$support/../build/server-mix.XXXXXX")
trap 'status=$?; rm -rf -- "$runtime_root"; cleanup "$status"' EXIT
peer_start
mkdir -p "$work/child"
export CAI_TEST_SERVER_CHILD="$work/child" CAI_TEST_SERVER_LOG="$root/mixed.jsonl"
export CAI_TEST_SERVER_RUNTIME="$runtime_root"
plan '{"plan":["SURVIVING OWNER WORKS"]}'
run -N; ok
[[ ! -s "$root/stdout" && $(requests) == 1 ]]
jq -se 'map(select(.sys == "storage.pouch" and .msg == "open")) | length == 1' "$root/mixed.jsonl" >/dev/null
