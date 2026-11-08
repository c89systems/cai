#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
case ${2:-all} in
  all) outcomes=(success failure cancel final) ;;
  final_failure) outcomes=(final_failure) ;;
  *) fail 'Unknown responsiveness scenario' ;;
esac
for outcome in "${outcomes[@]}"; do
  unset CAI_TEST_APPEND_GATE_FINAL
  if [[ "$outcome" == final* ]]; then export CAI_TEST_APPEND_GATE_FINAL=1; fi
  export CAI_TEST_APPEND_GATE="$root/gate-$outcome"
  mkdir -p "$CAI_TEST_APPEND_GATE"
  mkfifo "$CAI_TEST_APPEND_GATE/release"
  exec 8<>"$CAI_TEST_APPEND_GATE/release"
  plan "$(jq -nc --argjson chat "$chat" '{chat:$chat,plan:[{deltas:["DURABLE TEXT"," AND FINAL"]}]}')"
  terminal_start -N
  send 'start gated response\r'
  wait_for test -e "$CAI_TEST_APPEND_GATE/entered"
  # The durable worker stays blocked throughout both input observations.
  send 'EDIT_WHILE_STORAGE_IS_BLOCKED'
  expect_prompt EDIT_WHILE_STORAGE_IS_BLOCKED
  send '\177\177\177\177\177EDITED'
  expect_prompt EDIT_WHILE_STORAGE_IS_BLEDITED
  if [[ "$outcome" == final* ]]; then
    wait_for has "$term/message" 'Finishing turn'
    absent "$term/message" 'Worked for'
    wait_for grep -Eq '^[-/|\\] gpt-' "$term/message"
    contains "$term/feed" 'DURABLE TEXT'
    contains "$term/feed" 'AND FINAL'
  else
    absent "$term/feed" 'DURABLE TEXT'
  fi
  if [[ "$outcome" == cancel ]]; then
    send '\033'
    wait_for grep -qx '>' "$term/prompt"
  fi
  if [[ "$outcome" == *failure ]]; then printf f >&8; else printf y >&8; fi
  if [[ "$outcome" == *failure ]]; then
    if [[ "$outcome" == failure ]]; then
      expect 'Request failed'; absent "$term/feed" 'DURABLE TEXT'
    fi
    expect 'Turn failed'; expect 'Worked for'
    if [[ "$outcome" == final_failure ]]; then expect 'gated append failed'; fi
  elif [[ "$outcome" == cancel ]]; then
    expect 'Turn stopped.'
  elif [[ "$outcome" == final ]]; then
    expect 'Worked for'
  else
    expect 'DURABLE TEXT'; expect 'AND FINAL'; expect 'Worked for'
  fi
  send '\025'
  terminal_close
  exec 8>&-
done
