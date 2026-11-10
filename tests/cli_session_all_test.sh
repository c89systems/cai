#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
mkdir -p "$root/other" "$root/empty"
run -R --all; ok; contains "$root/stdout" 'No sessions.'
run -D --all; ok; contains "$root/stdout" 'No sessions.'
terminal_start -N
first=$(current_session_id)
terminal_close
terminal_start -N
second=$(current_session_id)
terminal_close
[[ -n "$first" && -n "$second" && "$first" != "$second" ]]
terminal_start -N -C "$root/other"
third=$("${base[@]}" -C "$root/other" -R | awk '$1 == 1 { print $2 }')
[[ -n "$third" && "$third" != "$first" && "$third" != "$second" ]]
# Global resume listings use the same ID/directory layout as -L, while local
# listings retain their existing numbers. Listing performs no agent request.
run -L; ok; cp "$root/stdout" "$root/global"
for args in '-R --all' '--all --resume'; do
  read -r -a listing <<<"$args"
  run "${listing[@]}"; ok
  cmp "$root/stdout" "$root/global"
done
for id in "$first" "$second" "$third"; do
  awk -v id="$id" '$1 == id {found=1} END {exit !found}' "$root/global"
done
run -R; ok
contains "$root/stdout" "1  $second"
absent "$root/stdout" "$third"
run -C "$root/empty" -D; ok; contains "$root/stdout" 'No sessions.'
# --all widens the eligible listing without bypassing the open-use guard or
# mutating either session. An explicit ID is global even without --all.
for args in '-D --all' '--all --delete'; do
  read -r -a listing <<<"$args"
  run -C "$root/empty" "${listing[@]}"; ok
  contains "$root/stdout" "$first"; contains "$root/stdout" "$second"
  absent "$root/stdout" "$third"
done
run -C "$root/empty" -D "$third"; bad
contains "$root/stderr" 'currently open'
# Store-wide identity does not escape the selected history store.
run --state-dir "$root/isolated-state" -D "$first"; bad
contains "$root/stderr" 'was not found'
run -R --all; ok; contains "$root/stdout" "$first"
run --export "$first"; ok
exports="$root/data/cai/exports/$first"
[[ -s "$exports/$first.md" && -s "$root/cache/cai/$first.log" ]]
run -C "$root/empty" -D "$first"; ok
contains "$root/stdout" "Deleted session $first."
[[ ! -e "$exports" && ! -e "$root/cache/cai/$first.log" ]]
run -R; ok; contains "$root/stdout" "1  $second"; absent "$root/stdout" "$first"
run -C "$root/empty" -D 1; bad
run -D 1 --all; bad
run -R; ok; contains "$root/stdout" "$second"
terminal_close
# Closed global deletion listings match resume listings. The modifier never
# means bulk deletion, and ID deletion repairs the owning directory's latest.
run -R --all; ok; cp "$root/stdout" "$root/remaining"
run -D --all; ok; cmp "$root/stdout" "$root/remaining"
run -C "$root/empty" -D "$second" --all; ok
run -R; ok; contains "$root/stdout" 'No sessions.'
terminal_start -C "$root/other" -R "$third"
send '/status\r'; expect "$third"
terminal_close
run --resume --all; ok
contains "$root/stdout" "$third"; absent "$root/stdout" "$first"; absent "$root/stdout" "$second"
[[ $(requests) == 0 ]]
