#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
mode=${1:-host}
peer_start
plan '{"plan":["STARTUP ANSWER"]}'
run -Nni 'STARTUP USER'; ok
[[ $(stdout_text) == 'STARTUP ANSWER' ]]
banner='libcai (c) 2026 C89 Systems AB'
introduction='Enter sends or queues; Alt-Enter steers.'
repo='https://github.com/c89systems/cai/'
cai_intro=$'cai is an example of embedding libcai_app: https://github.com/c89systems/cai/.\nEnter sends or queues; Alt-Enter steers. /help or cai -h for options.'
layout() {
  absent "$term/text" 'Welcome to cai.'
  if [[ "$mode" == cai ]]; then
    jq -Rse --rawfile banner "$support/../banners/cai.txt" --arg intro "$2" \
      'contains($banner + $intro + "\n\n")' "$term/text" >/dev/null || fail 'Incorrect example banner or introduction layout'
    return
  fi
  awk -v banner="$1" -v intro="$2" '
    BEGIN {count=split(intro,lines,"\n")}
    $0==banner && !start {start=NR}
    start && NR==start+1 && $0!="" {exit 1}
    start && NR>=start+2 && NR<start+2+count && $0!=lines[NR-start-1] {exit 1}
    start && NR==start+2+count && $0!="" {exit 1}
    END {if (!start || NR<start+2+count) exit 1}' "$term/text" || fail 'Incorrect startup rows or margins'
}
absent "$root/stdout" "$banner"
absent "$root/stdout" "$introduction"
absent "$root/stdout" "$repo"
for utility in --help --version --list; do
  run "$utility"; ok
  absent "$root/stdout" "$banner"
  absent "$root/stdout" "$introduction"
  absent "$root/stdout" "$repo"
done
terminal_start
expect 'STARTUP USER'
if [[ "$mode" == cai ]]; then
  contains "$term/raw" $'\033[38;2;0;158;255m'
  contains "$term/text" "$introduction"
  absent "$term/text" '**Enter**'
  contains "$term/text" "$repo"
  absent "$term/text" "<$repo>"
  absent "$term/text" '`libcai_app`'
  layout "$banner" "$cai_intro"
  contains "$term/text" '  STARTUP ANSWER'
  jq -Rse --arg banner "$banner" --arg intro "$introduction" \
    'index($banner) < index($intro) and index($intro) < index("> STARTUP USER")' "$term/text" >/dev/null
else
  absent "$term/text" 'Welcome to cai.'
  absent "$term/text" "$banner"
  absent "$term/text" "$introduction"
  absent "$term/text" "$repo"
fi
terminal_close
if [[ "$mode" == cai ]]; then
  export NO_COLOR=1
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" "$banner"
  absent "$term/raw" $'\033[38;2;0;158;255m'
  layout "$banner" "$cai_intro"
  terminal_close
  terminal_start -N
  layout "$banner" "$cai_intro"
  absent "$term/text" 'STARTUP USER'
  terminal_close
  unset NO_COLOR
  export TERM=dumb
  terminal_start
  layout "$banner" "$cai_intro"
  absent "$term/raw" $'\033[38;2;0;158;255m'
  terminal_close
  export TERM=xterm-256color
  terminal_start --no-banner
  absent "$term/text" "$banner"
  absent "$term/text" "$introduction"
  absent "$term/text" 'Welcome to CAI.'
  terminal_close
else
  export CAI_TEST_APP_BANNER=$'HOST STARTUP BANNER\n\n'
  export CAI_TEST_APP_INTRODUCTION='**HOST INTRODUCTION** with `rendered code`.'
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" 'HOST INTRODUCTION'
  contains "$term/text" 'rendered code'
  absent "$term/text" '**HOST INTRODUCTION**'
  layout 'HOST STARTUP BANNER' 'HOST INTRODUCTION with rendered code.'
  contains "$term/text" '  STARTUP ANSWER'
  jq -Rse 'index("HOST STARTUP BANNER") < index("HOST INTRODUCTION") and index("HOST INTRODUCTION") < index("> STARTUP USER")' "$term/text" >/dev/null
  terminal_close
  export NO_COLOR=1
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" 'HOST INTRODUCTION with rendered code.'
  absent "$term/text" '`rendered code`'
  if grep -Eq $'\033\\[[0-9;]*[1-9][0-9;]*mrendered code\033\\[0m' "$term/raw"; then
    fail 'NO_COLOR introduction unexpectedly has a styled code span'
  fi
  terminal_close
  unset NO_COLOR
  run --help; ok
  absent "$root/stdout" 'HOST STARTUP BANNER'
  absent "$root/stdout" 'HOST INTRODUCTION'
  export CAI_TEST_APP_BANNER='INLINE:' CAI_TEST_APP_INTRODUCTION_LEFT=3
  terminal_start
  expect 'STARTUP USER'
  contains "$term/text" 'INLINE:   HOST INTRODUCTION with rendered code.'
  contains "$term/text" '  STARTUP ANSWER'
  terminal_close
  export CAI_TEST_APP_BANNER=$'CUSTOM MARGINS\n\n'
  export CAI_TEST_APP_INTRODUCTION_RIGHT=9
  export CAI_TEST_APP_INTRODUCTION='one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen'
  width=60
  terminal_start
  expect 'STARTUP USER'
  awk '
    $0=="CUSTOM MARGINS" {start=NR; next}
    start && NR==start+1 && $0!="" {exit 1}
    start && NR>=start+2 && /^$/ {exit lines>1 ? 0 : 1}
    start && NR>=start+2 {if (substr($0,1,3)!="   " || length($0)>51) exit 1; lines++}
    END {if (!start || lines<2) exit 1}
  ' "$term/text" || fail 'Introduction did not honor both configured margins'
  contains "$term/text" '  STARTUP ANSWER'
  terminal_close
fi
