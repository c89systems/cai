#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
phase=${1:-1}
protocol=${2:-responses}
peer_start
printf '[client]\napi_protocol=%s\n' "$protocol" >"$root/protocol.ini"
base+=(--config "$root/protocol.ini")
mkdir "$root/gate"
mkfifo "$root/gate/release"
export CAI_TEST_EXPORT_GATE="$root/gate"
export CAI_TEST_EXPORT_GATE_PASS="$phase"
export CAI_TEST_EXPORT_ACTIVE_GOAL=1
chat=false
[[ "$protocol" != chat_completions ]] || chat=true
set_plan() { plan "{\"chat\":$chat,\"plan\":[\"$1\"]}"; }
set_plan ExportSeedReply
terminal_start -N
prompt 'ExportSeedPrompt' ExportSeedReply
before=$(requests)
send '/export first\r'
wait_for test -f "$root/gate/entered"
expect 'Exporting session'
# This persists observations too. It must finish while the read stays gated.
send '/status\r'; expect 'Session ID'
[[ ! -e "$work/first.md" && ! -e "$work/first.jsonl" ]]
set_plan AfterExportReply
send 'AfterExportPrompt\r'; expect 'Q 1. AfterExportPrompt'
[[ $(requests) == "$before" ]]
printf r >"$root/gate/release"
expect 'Session export'
expect_assistant AfterExportReply
[[ -s "$work/first.md" && -s "$work/first.jsonl" ]]
for file in "$work/first.md" "$work/first.jsonl"; do
  contains "$file" ExportSeedPrompt
  contains "$file" ExportSeedReply
  absent "$file" AfterExportPrompt
done

# Escape cancels without waiting for a release token.
rm "$root/gate/entered"
send '/export cancelled\r'
wait_for test -f "$root/gate/entered"
if [[ "$phase" == 3 ]]; then
  [[ -n $(find "$work" -name '.export-*' -size +0c -print) ]]
fi
send '/status\r'; expect 'Session ID'
send '/goal\r'; expect 'Status: active'
send '\033'; expect 'journal projection cancelled'
[[ ! -e "$work/cancelled.md" && ! -e "$work/cancelled.jsonl" ]]
[[ -z $(find "$work" -name '.export-*' -print) ]]
send '/goal\r'; expect 'Status: paused'
[[ $(requests) == $((before+1)) ]]

# A failed read is inline; the same worker can be reused.
printf 'KEEP FAILED DESTINATION\n' >"$work/failed.md"
rm "$root/gate/entered"
send '/export failed.md\r'
wait_for test -f "$root/gate/entered"
printf f >"$root/gate/release"
expect 'ERROR: command failed'
send '/status\r'; expect 'Session ID'
[[ $(cat "$work/failed.md") == 'KEEP FAILED DESTINATION' ]]
rm "$root/gate/entered"
send '/export recovered.jsonl\r'
wait_for test -f "$root/gate/entered"
printf r >"$root/gate/release"
expect 'Session export'
[[ -s "$work/recovered.jsonl" ]]

# Cancellation must also preserve already-existing sibling destinations.
printf 'KEEP MARKDOWN\n' >"$work/overwrite.md"
printf 'KEEP JSONL\n' >"$work/overwrite.jsonl"
rm "$root/gate/entered"
send '/export overwrite\r'
wait_for test -f "$root/gate/entered"
send '\033'; expect 'journal projection cancelled'
[[ $(cat "$work/overwrite.md") == 'KEEP MARKDOWN' ]]
[[ $(cat "$work/overwrite.jsonl") == 'KEEP JSONL' ]]
[[ -z $(find "$work" -name '.export-*' -print) ]]

# Last-client shutdown cancels and joins before the shared store closes.
rm "$root/gate/entered"
send '/export stopping\r'
wait_for test -f "$root/gate/entered"
terminal_close
no_candidates() { [[ -z $(find "$work" -name '.export-*' -print) ]]; }
wait_for no_candidates
[[ ! -e "$work/stopping.md" && ! -e "$work/stopping.jsonl" ]]
unset CAI_TEST_EXPORT_GATE CAI_TEST_EXPORT_GATE_PASS CAI_TEST_EXPORT_ACTIVE_GOAL
# Noninteractive command settlement waits for the real asynchronous result.
run -ni '/export batch.md'; ok
contains "$root/stdout" 'Session export'
[[ -s "$work/batch.md" && $(requests) == $((before+1)) ]]
run -ni '/status'; ok
contains "$root/stdout" 'Session ID'
[[ $(requests) == $((before+1)) ]]
