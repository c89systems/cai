#!/usr/bin/env bash
# Preserve URL bytes through the real libmdf -> Softline feed and replay.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
width=24
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
urls=(
  'https://example.test/long/path/to/a/resource/bare?query=42#section'
  'https://example.test/long/path/to/a/resource/styled?query=43#section'
  'https://example.test/long/path/to/a/resource/list?query=44#section'
  'https://example.test/long/path/to/a/resource/label?query=45#section'
  '//example.test/long/path/to/a/resource/relative?query=46#section'
)
markdown=$(printf '%s\n\n' "Bare ${urls[0]}" "**${urls[1]}**" \
  "- [docs](${urls[2]})" "[${urls[3]}](https://other.test/target)" \
  "[relative](${urls[4]})" 'UrlDone')
assert_urls() {
  local url output=${1:-$term/feed}
  for url in "${urls[@]}"; do contains "$output" "$url"; done
  absent "$output" '[docs]('
  absent "$output" '**https://'
}
for mode in whole fragmented; do
  plan "$(jq -n --arg text "$markdown" --arg mode "$mode" --argjson chat "$chat" '
    {chat:$chat,plan:[(if $mode == "whole" then [$text]
      else [$text | range(0; length; 7) as $i | .[$i:$i+7]] end)]}')"
  terminal_start -N
  prompt 'render wrapping URLs' UrlDone
  assert_urls
  id=$(current_session_id)
  terminal_close
  before=$(requests)
  terminal_start --resume "$id"
  expect UrlDone
  # Startup replay is printed before the native output-stream capture starts.
  assert_urls "$term/text"
  [[ $(requests) == "$before" ]]
  terminal_close
done
