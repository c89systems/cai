#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# External Codex files are never discovered implicitly, even when malformed.
mkdir -p "$HOME/.codex"
printf 'INVALID_UNREQUESTED_AUTH\n' >"$HOME/.codex/auth.json"
cat >"$root/auth.ini" <<INI
[mcp.chatgpt]
endpoint = ${endpoint%/v1}/mcp/chatgpt
INI
base+=(--config "$root/auth.ini")
plan '{"plan":["NO AUTH REQUIRED"]}'
run --no-mcp -Nni 'use only the configured inference provider'; ok
[[ $(stdout_text) == 'NO AUTH REQUIRED' ]]
before=$(requests)
custom_base=("${base[@]}")
base=("$cli" --config "$root/auth.ini" --insecure -C "$work")
run --no-mcp -p chatgpt -Nni 'authentication is required'; bad
cat "$root/stdout" "$root/stderr" >"$root/failure"
contains "$root/failure" '--login'
absent "$root/failure" INVALID_UNREQUESTED_AUTH
[[ $(requests) == "$before" ]]
# Missing optional app credentials leave independent inference available.
base=("${custom_base[@]}")
plan '{"plan":["INFERENCE STILL AVAILABLE"]}'
run --mcp -ni 'continue without app authentication'; ok
[[ $(stdout_text) == 'INFERENCE STILL AVAILABLE' && $(requests) == $((before+1)) ]]
[[ ! -e "$root/state/cai/auth.json" ]]
