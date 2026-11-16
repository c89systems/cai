#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mkdir "$root/bin"
printf '#!/bin/sh\nexit 0\n' >"$root/bin/xdg-open"
chmod 700 "$root/bin/xdg-open"
export PATH="$root/bin:$PATH"
for mode in login_accept login_partial message_before message; do
  export CAI_TEST_UTILITY_STOP="$mode"
  if [[ "$mode" == message* ]]; then
    "$cli" -C "$work" -q utility-target -M 'Enqueue, then stop' >"$root/stdout" 2>"$root/stderr"
    if [[ "$mode" == message ]]; then
      jq -e '.status == "queued"' "$root/stdout" >/dev/null
    else
      [[ ! -s "$root/stdout" ]]
    fi
  else
    "$cli" -C "$work" --login >"$root/stdout" 2>"$root/stderr"
    contains "$root/stdout" 'Waiting for OAuth callback'
  fi
done

# Host STOP cancels the editor handoff, reaps its group and removes the
# private edit tree before reporting the normal stopped utility outcome.
export CAI_TEST_UTILITY_STOP=edit TMPDIR="$root/edit-tmp"
mkdir "$TMPDIR"
printf '#!/bin/sh\nexec sleep 60\n' >"$root/bin/editor"
chmod 700 "$root/bin/editor"
export EDITOR="$root/bin/editor"
unset VISUAL
"$cli" -C "$work" --edit-agent default >"$root/stdout" 2>"$root/stderr"
[[ -z $(find "$TMPDIR" -mindepth 1 -print -quit) ]]
[[ ! -s "$root/stderr" ]]
