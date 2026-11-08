#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/cli_table_assertions.sh"

cli=$1
build_dir=$2
fixture=$(mktemp -d "$build_dir/cli-providers.XXXXXX")
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/home/.codex" "$fixture/state" "$fixture/work"
export HOME="$fixture/home" XDG_STATE_HOME="$fixture/state" XDG_CACHE_HOME="$fixture/cache"
export XDG_CONFIG_HOME="$fixture/config" CAI_INSECURE=true
mkdir -p "$XDG_CONFIG_HOME/cai"
cat > "$XDG_CONFIG_HOME/cai/models.json" <<'JSON'
{"models":[{"provider":"berget","id":"zai-org/GLM-5.3-Flash","context_window_tokens":131072}]}
JSON
cat > "$fixture/home/.codex/auth.json" <<'JSON'
{"auth_mode":"chatgpt","tokens":{"id_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","access_token":"eyJhbGciOiAibm9uZSJ9.eyJleHAiOiA0MTAyNDQ0ODAwfQ.sig","refresh_token":"fixture","account_id":"fixture"},"last_refresh":"2026-09-25T00:00:00Z"}
JSON
export HTTPS_PROXY=http://127.0.0.1:1 HTTP_PROXY=http://127.0.0.1:1 NO_PROXY=
status=$(printf '/status\n/quit\n' | "$cli" -C "$fixture/work")
[[ "$status" == *"Provider"* && "$status" == *"chatgpt"* ]]
[[ "$status" == *"Model"* && "$status" == *"gpt-6-luna"* ]]

printf 'invalid\n' > "$fixture/home/.codex/auth.json"
if printf '/quit\n' | "$cli" -C "$fixture/work" >"$fixture/out" 2>"$fixture/err"; then
  echo 'Codex auth was not preferred' >&2
  exit 1
fi
grep -q 'failed to parse auth JSON' "$fixture/err"
rm "$fixture/home/.codex/auth.json"
stored=$(printf '/status\n/quit\n' | "$cli" -C "$fixture/work")
[[ "$stored" == *"chatgpt"* ]]
test ! -f "$fixture/state/cai/auth.json"

openai=$(printf '/status\n/quit\n' | OPENAI_API_KEY=fixture "$cli" -N -p openai -C "$fixture/work")
[[ "$openai" == *"Provider"* && "$openai" == *"openai"* ]]
! table_contains /dev/stdin 1 'Weekly limit' <<<"$openai"
! table_contains /dev/stdin 1 'Credits left' <<<"$openai"
router=$(printf '/status\n/quit\n' | OPENROUTER_API_KEY=fixture "$cli" -N -p openrouter -C "$fixture/work")
[[ "$router" == *"Provider"* && "$router" == *"openrouter"* ]]
[[ "$router" == *"Model"* && "$router" == *"openai/gpt-5.6-luna"* ]]
berget=$(printf '/status\n/quit\n' | BERGET_API_KEY=fixture "$cli" -N -p berget -C "$fixture/work")
[[ "$berget" == *"Provider"* && "$berget" == *"berget"* ]]
[[ "$berget" == *"Model"* && "$berget" == *"zai-org/GLM-5.3-Flash"* ]]
table_contains /dev/stdin 1 'Reasoning effort' <<<"$berget"
table_contains /dev/stdin 2 medium <<<"$berget"
! table_contains /dev/stdin 1 'Weekly limit' <<<"$berget"
! table_contains /dev/stdin 1 'Credits left' <<<"$berget"
mv "$XDG_CONFIG_HOME/cai/models.json" "$fixture/models.saved"
berget_no_file=$(printf '/status\n/quit\n' | BERGET_API_KEY=fixture "$cli" -N -p berget -C "$fixture/work")
table_contains /dev/stdin 1 'Context window' <<<"$berget_no_file"
table_contains /dev/stdin 2 1048576 <<<"$berget_no_file"
mv "$fixture/models.saved" "$XDG_CONFIG_HOME/cai/models.json"
printf 'invalid\n' > "$fixture/invalid-models.json"
if printf '/quit\n' | BERGET_API_KEY=fixture "$cli" -N -p berget -C "$fixture/work" \
  --model-metadata "$fixture/invalid-models.json" >"$fixture/out" 2>"$fixture/err"; then
  echo 'invalid model metadata was accepted' >&2
  exit 1
fi
cat "$fixture/out" "$fixture/err" | grep -q 'invalid model metadata JSON'
override=$(printf '/status\n/quit\n' | BERGET_API_KEY=fixture "$cli" -p berget \
  -C "$fixture/work" --model-metadata "$XDG_CONFIG_HOME/cai/models.json")
[[ "$override" == *"zai-org/GLM-5.3-Flash"* ]]
custom=$(printf '/status\n/quit\n' | LOCAL_KEY=fixture "$cli" -p custom \
  --endpoint https://example.test/v1 --api-key-env LOCAL_KEY -m local-model -C "$fixture/work")
[[ "$custom" == *"Provider"* && "$custom" == *"custom"* ]]
[[ "$custom" == *"Model"* && "$custom" == *"local-model"* ]]
default_key=$(printf '/status\n/quit\n' | CAI_API_KEY=fixture "$cli" -p custom \
  --endpoint https://example.test/v1 -m local-model -C "$fixture/work")
[[ "$default_key" == *"custom"* ]]
if printf '/quit\n' | env -u OPENAI_API_KEY "$cli" -N -p openai -C "$fixture/work" \
  >"$fixture/out" 2>"$fixture/err"; then
  echo 'missing OpenAI key was accepted' >&2
  exit 1
fi
grep -q 'OPENAI_API_KEY' "$fixture/err"
if printf '/quit\n' | env -u BERGET_API_KEY "$cli" -N -p berget -C "$fixture/work" \
  >"$fixture/out" 2>"$fixture/err"; then
  echo 'missing Berget key was accepted' >&2
  exit 1
fi
grep -q 'BERGET_API_KEY' "$fixture/err"

export XDG_STATE_HOME="$fixture/state-empty"
if printf '/quit\n' | "$cli" -C "$fixture/work" >"$fixture/out" 2>"$fixture/err"; then
  echo 'missing auth was accepted' >&2
  exit 1
fi
grep -q 'auth JSON is not stored' "$fixture/err"
help=$("$cli" --help)
[[ "$help" == $'cai and libcai Copyright (C) 2026 C89 Systems AB https://c89.systems\n\nUsage: cai '* ]]
[[ "$("$cli" -h)" == "$help" ]]
