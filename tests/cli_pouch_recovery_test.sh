#!/usr/bin/env bash
# Damage only a disposable store's derived index, never authoritative records.
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["DURABLE CONVERSATION ANSWER"]}'
run -Nni 'durable conversation prompt'; ok
run --list-all; ok
cp "$root/stdout" "$root/list"
id=$(awk 'NR == 1 {print $1}' "$root/list")
[[ -n "$id" ]]
logfile="$root/cache/cai/$id.log"
manifest="$root/state/cai/pouch/namespaces/cai.sessions/index/query.manifest"
key="$root/state/cai/pouch.key"
[[ -s "$manifest" ]]
cp "$key" "$root/original-key"
for mode in missing empty malformed; do
  case "$mode" in
    missing) rm "$manifest" ;;
    empty) : >"$manifest" ;;
    malformed) printf 'interrupted derived index\n' >"$manifest" ;;
  esac
  run --list-all; ok
  cmp "$root/stdout" "$root/list"
  cmp "$key" "$root/original-key"
  [[ -s "$manifest" ]]
  # Recovery must install a readable index; the warning must not recur.
  run --list-all; ok
  cmp "$root/stdout" "$root/list"
  absent "$root/stderr" cache.load.index.error
done
run --export "$id"; ok
contains "$root/data/cai/exports/$id/$id.md" 'DURABLE CONVERSATION ANSWER'
contains "$root/data/cai/exports/$id/$id.jsonl" 'durable conversation prompt'
plan '{"plan":["RESUMED AFTER INDEX RECOVERY"]}'
run -ni 'continue after recovering the index'; ok
contains "$root/stdout" 'RESUMED AFTER INDEX RECOVERY'
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" 'durable conversation prompt'
contains "$root/request" 'DURABLE CONVERSATION ANSWER'
# Auto-resume followed immediately by /quit must complete recovery without
# a user prompt, /resume listing, or a commit-producing model turn.
count=$(requests)
# An unusable derived-index path must report the repair failure without
# preventing direct auto-resume of the intact conversation.
rm "$manifest"
mkdir "$manifest"
offset=$(wc -c <"$logfile")
terminal_start
expect 'RESUMED AFTER INDEX RECOVERY'
terminal_close
tail -c +$((offset+1)) "$logfile" >"$root/restart.log"
contains "$root/restart.log" 'Session search index recovery failed'
rmdir "$manifest"
printf 'interrupted derived index\n' >"$manifest"
for mode in missing empty malformed; do
  case "$mode" in
    missing) rm "$manifest" ;;
    empty) : >"$manifest" ;;
    malformed) printf 'interrupted derived index\n' >"$manifest" ;;
  esac
  offset=$(wc -c <"$logfile")
  terminal_start
  expect 'RESUMED AFTER INDEX RECOVERY'
  terminal_close
  tail -c +$((offset+1)) "$logfile" >"$root/restart.log"
  if [[ "$mode" != missing ]]; then
    jq -se 'any(.[]; .lvl == "warn" and .msg == "cache.load.index.error" and .ns == "cai.sessions")' "$root/restart.log" >/dev/null
  fi
  for restart in 1 2; do
    offset=$(wc -c <"$logfile")
    terminal_start
    expect 'RESUMED AFTER INDEX RECOVERY'
    terminal_close
    tail -c +$((offset+1)) "$logfile" >"$root/restart.log"
    absent "$root/restart.log" cache.load.index.error
  done
done
terminal_start
send '/resume\r'; expect "$id"
terminal_close
[[ $(requests) == "$count" ]]
run --list-all; ok
absent "$root/stderr" cache.load.index.error
cmp "$key" "$root/original-key"
# Auto-resume reads the workspace lookup directly too. Repair its derived
# index during open rather than waiting for a later indexed query or prompt.
manifest="$root/state/cai/pouch/namespaces/cai.scopes/index/query.manifest"
[[ -s "$manifest" ]]
printf 'interrupted workspace index\n' >"$manifest"
terminal_start
expect 'RESUMED AFTER INDEX RECOVERY'
terminal_close
offset=$(wc -c <"$logfile")
terminal_start
expect 'RESUMED AFTER INDEX RECOVERY'
terminal_close
tail -c +$((offset+1)) "$logfile" >"$root/restart.log"
absent "$root/restart.log" cache.load.index.error
[[ $(requests) == "$count" ]]
cmp "$key" "$root/original-key"
