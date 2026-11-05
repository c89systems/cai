#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
peer_start
runtime_root=$(mktemp -d "$support/../build/bsh.XXXXXX")
base+=(--runtime-dir "$runtime_root")
owner_pid= batch_pid=
shell_cleanup() {
  local result=$?
  for child in "$batch_pid" "$owner_pid"; do
    if [[ -n "$child" ]]; then
      kill -TERM "$child" 2>/dev/null || true
      wait "$child" 2>/dev/null || true
    fi
  done
  [[ $result != 0 ]] || rm -rf -- "$runtime_root"
  cleanup "$result"
}
trap shell_cleanup EXIT
printf 'config_env_has_precedence = true\n[env]\nCAI_SHELL_CAPTURED = configured\n' >"$root/shell.ini"
export CAI_SHELL_CAPTURED=host
"${base[@]}" --config "$root/shell.ini" -x -N --shell-context-max-bytes 1024 --workspace-name shell-fixture </dev/null >"$root/owner.out" 2>"$root/owner.log" & owner_pid=$!
wait_for has "$root/owner.log" 'Workspace server ready'

# No inference and no terminal: use the owner's captured environment/cwd,
# merge stderr, provide EOF on stdin, and preserve exact process bytes.
run -s shell-fixture -ni '!test ! -t 0 && test ! -t 1 && test ! -t 2 && cat && printf "%s:" "$CAI_SHELL_CAPTURED"; pwd; printf STDERR >&2'; ok
printf 'configured:%s\nSTDERR' "$work" >"$root/expected"
cmp "$root/stdout" "$root/expected"
[[ $(requests) == 0 ]]
run -s shell-fixture -ni '!true'; ok
[[ ! -s "$root/stdout" && $(requests) == 0 ]]
run -s shell-fixture -ni '!printf "\000\377\302\233"'; ok
printf '\000\377\302\233' >"$root/expected"
cmp "$root/stdout" "$root/expected"

# Early bytes reach the client while the command remains alive. Completion
# must wait for the real exit/EOF, not the command's launch acknowledgement.
"${base[@]}" -s shell-fixture -ni '!printf EARLY; while [ ! -f shell-gate ]; do sleep .01; done; printf LATE' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for has "$root/batch.out" EARLY
kill -0 "$batch_pid"
[[ $(cat "$root/batch.out") == EARLY ]]
touch "$work/shell-gate"
wait "$batch_pid"; batch_pid=
[[ $(cat "$root/batch.out") == EARLYLATE && $(requests) == 0 ]]

# The retained tail is 1 KiB, but live binary output is complete. Stop the
# reader after one byte so transport credit/rings and the pipe all fill.
mkfifo "$root/output-pipe"
"${base[@]}" -s shell-fixture -ni '!head -c 1048576 /dev/zero' >"$root/output-pipe" 2>"$root/batch.err" & batch_pid=$!
exec 9<"$root/output-pipe"
dd bs=1 count=1 <&9 >"$root/large.out" 2>/dev/null
sleep .1
kill -0 "$batch_pid"
cat <&9 >>"$root/large.out"
exec 9<&-
wait "$batch_pid"; batch_pid=
[[ $(wc -c <"$root/large.out") == 1048576 && $(requests) == 0 ]]
head -c 1048576 /dev/zero >"$root/expected"
cmp "$root/large.out" "$root/expected"

# Failure stops dependent inputs. Successful earlier output is stderr activity;
# a following model input consumes shared context and owns final stdout.
run -s shell-fixture -ni '!printf failed >&2; exit 7' -i '!touch MUST-NOT-RUN'; bad
[[ ! -e "$work/MUST-NOT-RUN" && $(requests) == 0 ]]
contains "$root/stderr" failed
plan '{"plan":["SHARED SHELL USED"]}'
run -s shell-fixture -ni '!printf SHARED-SHELL' -i 'use the user shell result'; ok
[[ $(cat "$root/stdout") == 'SHARED SHELL USED' && $(requests) == 1 ]]
contains "$root/stderr" SHARED-SHELL
contains "$root/peer/requests.jsonl" SHARED-SHELL
plan '{"plan":["PRIVATE SHELL EXCLUDED"]}'
run -s shell-fixture --no-shell-context -ni '!printf PRIVATE-SHELL' -i 'verify private shell context'; ok
[[ $(cat "$root/stdout") == 'PRIVATE SHELL EXCLUDED' && $(requests) == 2 ]]
absent "$root/peer/requests.jsonl" PRIVATE-SHELL
run -s shell-fixture -ni '!sh'; bad
contains "$root/stderr" 'interactive terminal attachment'
[[ $(requests) == 2 ]]

# A lost attachment releases output backpressure without cancelling a
# persistent owner's work or reopening it in another client.
"${base[@]}" -s shell-fixture -ni '!touch detached-started; while [ ! -f detached-gate ]; do sleep .01; done; printf AFTER-DETACH; touch detached-finished' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for test -f "$work/detached-started"
kill -TERM "$batch_pid"
wait "$batch_pid" || true; batch_pid=
"$cli" -p custom --endpoint "$endpoint" --no-terminal --no-image-generation --no-mcp --insecure -C "$work" --runtime-dir "$runtime_root" -s shell-fixture -ni '!printf AFTER-LOST-ATTACHMENT' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for has "$root/batch.err" '!printf AFTER-LOST-ATTACHMENT'
touch "$work/detached-gate"
wait_for test -f "$work/detached-finished"
wait "$batch_pid"; batch_pid=
[[ $(cat "$root/batch.out") == AFTER-LOST-ATTACHMENT && $(requests) == 2 ]]

# Losing the owner cannot be reported as a successful or retryable command.
"${base[@]}" -s shell-fixture -ni '!printf BEFORE-LOSS; sleep 60' >"$root/batch.out" 2>"$root/batch.err" & batch_pid=$!
wait_for has "$root/batch.out" BEFORE-LOSS
kill -TERM "$owner_pid"; wait "$owner_pid"; owner_pid=
result=0; wait "$batch_pid" || result=$?; batch_pid=
[[ $result != 0 ]]

# Quiet autostart, rather than an explicitly persistent server, uses exactly
# the same shell path and removes its endpoint when the batch detaches.
run -ni '!printf AUTOSTART-SHELL'; ok
[[ $(cat "$root/stdout") == AUTOSTART-SHELL && $(requests) == 2 ]]
endpoint_gone() { [[ -z $(find "$runtime_root" -name 'ws-*.info' -print -quit) ]]; }
wait_for endpoint_gone
