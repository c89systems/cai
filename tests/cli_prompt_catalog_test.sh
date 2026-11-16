#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
base=("$cli" --no-config -C "$work")
run --dump-system-prompts "$root/catalog/deep" --identity 'Prompt Tester'; ok
[[ ! -e "$root/state" && ! -e "$root/cache" ]]
[[ $(find "$root/catalog/deep" -maxdepth 1 -name '*.md' | wc -l) -ge 6 ]]
contains "$root/catalog/deep/gpt-6.1-sol.md" 'You are Prompt Tester'
contains "$root/catalog/deep/reviewer.md" 'You are Prompt Tester'
contains "$root/catalog/deep/agent-builder.md" 'You are Prompt Tester'
absent "$root/catalog/deep/gpt-6.1-sol.md" '{{agent_identity}}'
cp "$root/catalog/deep/gpt-6.1-sol.md" "$root/preserved.md"
run --dump-system-prompts "$root/catalog/deep"; bad
cmp "$root/preserved.md" "$root/catalog/deep/gpt-6.1-sol.md"
run --dump-system-prompts; ok
[[ -f "$work/prompts/reviewer.md" ]]
run --dump-system-prompts "$root/rejected" --make "$root/source"; bad
[[ ! -e "$root/rejected" ]]
