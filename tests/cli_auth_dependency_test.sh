#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
# A separately configured auth store is deliberately unusable, while session
# storage remains available. Global MCP disable must suppress app auth too.
printf 'not a directory\n' >"$root/auth-blocker"
cat >"$root/auth.ini" <<INI
[storage]
auth_store = unavailable
[store.unavailable]
endpoint = pouch://$root/auth-blocker
key_file = $root/auth.key
[mcp.chatgpt]
endpoint = ${endpoint%/v1}/mcp/chatgpt
INI
base+=(--config "$root/auth.ini")
plan '{"plan":["NO AUTH STORE NEEDED"]}'
run --no-mcp -Nni 'use only the configured inference provider'; ok
[[ $(stdout_text) == 'NO AUTH STORE NEEDED' ]]
[[ ! -e "$root/auth.key" ]]
# ChatGPT inference still requires authentication when MCP is disabled.
before=$(requests)
custom_base=("${base[@]}")
base=("$cli" --config "$root/auth.ini" --no-mcp -C "$work")
run --no-mcp -p chatgpt -Nni 'authentication is required'; bad
cat "$root/stdout" "$root/stderr" >"$root/failure"
contains "$root/failure" 'pouch.key is missing for the existing encrypted store'
[[ $(requests) == "$before" && ! -e "$root/auth.key" ]]
# Enabling app connections also retains their authentication dependency.
base=("${custom_base[@]}")
run --mcp -Nni 'app authentication is required'; bad
cat "$root/stdout" "$root/stderr" >"$root/failure"
contains "$root/failure" 'pouch.key is missing for the existing encrypted store'
[[ $(requests) == "$before" && ! -e "$root/auth.key" ]]
# The required-auth failures must not make the separate session store unusable.
plan '{"plan":["INFERENCE STILL AVAILABLE"]}'
run --no-mcp -ni 'continue without app authentication'; ok
[[ $(stdout_text) == 'INFERENCE STILL AVAILABLE' && $(requests) == $((before+1)) ]]
