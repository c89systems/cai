#!/usr/bin/env bash
# Real sockets and Softline deferred edits: no delivery grace sleeps.
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
runtime_root=$(mktemp -d "$support/../build/edit.XXXXXX")
queue_edit_cleanup() {
  local result=$?
  if [[ $result == 0 ]]; then rm -rf -- "$runtime_root"; fi
  cleanup "$result"
}
trap queue_edit_cleanup EXIT
base+=(--runtime-dir "$runtime_root")
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false; [[ "$protocol" != chat_completions ]] || chat=true
command_plan() { plan "$(jq -n --argjson chat "$chat" "{chat:\$chat,plan:$1}")"; }
command_plan '[]'
terminal_start -N
# Recover each command in the same input burst as submission. The owner must
# accept BEGIN_EDIT before it can claim, including dangerous turn commands.
for command in '/review uncommitted' '/model must-not-select low' \
  '/goal new Must not create' '/new' '/shell' '/theme gruvbox' \
  '!touch must-not-exist'; do
  send "$command\r\033e"; expect_prompt "$command"
  absent "$term/feed" "\$ $command"
  send '\025/output compact\r'; expect 'Tool output: compact.'
done
[[ $(requests) == 0 && ! -e "$work/must-not-exist" ]]
command_plan '["EDITEDREPLY"]'
send 'ORIGINAL_PROMPT_MUST_NOT_RUN\r\033e'
expect_prompt ORIGINAL_PROMPT_MUST_NOT_RUN
[[ $(requests) == 0 ]]
send '\025EDITED_PROMPT_ONLY\r'; expect EDITEDREPLY
[[ $(requests) == 1 ]]
contains "$root/peer/requests.jsonl" EDITED_PROMPT_ONLY
absent "$root/peer/requests.jsonl" ORIGINAL_PROMPT_MUST_NOT_RUN

printf 'content\n' >"$work/input.txt"
command_plan '[{pause:true,pause_text:"ACTIVEEDITSEAM\n\n",tool:"read_file",arguments:{path:"input.txt"}},"STEEREDREPLY"]'
send 'read the fixture\r'; expect ACTIVEEDITSEAM
send 'ORIGINAL_STEER_MUST_NOT_RUN\033\r\033e'
expect_prompt ORIGINAL_STEER_MUST_NOT_RUN
[[ $(requests) == 2 ]]
send '\025EDITED_STEER_ONLY\r'; expect 'S 1. EDITED_STEER_ONLY'
release; expect STEEREDREPLY
[[ $(requests) == 3 ]]
contains "$root/peer/requests.jsonl" EDITED_STEER_ONLY
absent "$root/peer/requests.jsonl" ORIGINAL_STEER_MUST_NOT_RUN
terminal_close
no_publication() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
# Endpoint release follows runtime/store closure, including last-detach shutdown.
wait_for no_publication
