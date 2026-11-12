#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
plan '{"plan":["BANNER BATCH"]}'
run -Nni 'batch has no welcome'; ok
absent "$root/stdout" 'Welcome to'
run --help; ok
absent "$root/stdout" 'Welcome to'
run --not-a-real-option; bad
absent "$root/stdout" 'Welcome to'
terminal_start
absent "$term/text" 'Welcome to'
terminal_close
terminal_start --no-banner
absent "$term/text" 'Welcome to'
terminal_close
export CAI_NO_BANNER=true
terminal_start
absent "$term/text" 'Welcome to'
terminal_close
terminal_start --banner
absent "$term/text" 'Welcome to'
terminal_close
unset CAI_NO_BANNER
printf '[ui]\nno_banner = true\n' >"$root/banner.ini"
terminal_start --config "$root/banner.ini"
absent "$term/text" 'Welcome to'
terminal_close
