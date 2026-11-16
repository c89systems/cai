#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["What should the agent accomplish?"]}'
run --builder -Nn; ok
contains "$root/stdout" 'What should the agent accomplish?'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'helping the user build a complete stored agent'
contains "$root/request" 'Use only the tools present in this request and follow their schemas.'
contains "$root/request" 'Ask what it should accomplish'
absent "$root/request" '{{agent_identity}}'
plan '{"plan":["BUILDER_EXPLICIT_REQUEST"]}'
run --builder -Nni 'Create a source package for a documentation editor'; ok
contains "$root/stdout" BUILDER_EXPLICIT_REQUEST
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'Create a source package for a documentation editor'
absent "$root/request" 'Help me build an agent. Ask what it should accomplish'
printf 'EXPLICIT_PARENT_OVERRIDE\n' >"$root/override.md"
plan '{"plan":["PARENT_OVERRIDE_WORKED"]}'
run --builder -z "$root/override.md" -Nni 'use the supplied instructions'; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" EXPLICIT_PARENT_OVERRIDE
absent "$root/request" 'helping the user build a complete stored agent'
before=$(requests)
run --builder --review -ni 'must not start'; bad
[[ $(requests) == "$before" ]]
