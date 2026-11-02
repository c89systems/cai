#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
export CAI_TEST_REQUIRE_APPROVAL=1
unset CAI_INSECURE
peer_start
mkdir -p "$root/config/cai" "$work/sub/deep" "$work-sibling"
printf 'GLOBAL_TRUSTED_POLICY\n' >"$root/config/cai/AGENTS.md"
printf 'WORKSPACE_POLICY\n' >"$work/AGENTS.md"
printf 'CHILD_POLICY\n' >"$work/sub/AGENTS.md"
run -Nni hello; bad
contains "$root/stderr" 'Workspace is not approved'
contains "$root/stderr" '-y/--yes'
[[ $(requests) == 0 ]]
run --help; ok
run --list; ok
plan '{"plan":["BYPASS OK"]}'
run -kNni bypass; ok
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" GLOBAL_TRUSTED_POLICY
contains "$root/request" WORKSPACE_POLICY
id=$(current_session_id)
run --export "$id"; ok
exported=$(head -1 "$root/stdout")
absent "$exported" workspace_approved
run -ni no_saved_approval; bad
plan '{"plan":["APPROVED"]}'
run -yNni approve; ok
id=$(current_session_id)
run --export "$id"; ok
exported=$(head -1 "$root/stdout")
jq -se --arg root "$work" 'any(.[]; .type=="workspace_approved" and .data==$root)' "$exported" >/dev/null
plan '{"plan":["RESUMED APPROVED"]}'
run -ni resume; ok
contains "$root/stdout" 'RESUMED APPROVED'
plan '{"plan":["NEW APPROVED"]}'
run -Nni new_approved_conversation; ok
plan '{"plan":["CHILD APPROVED"]}'
run -C "$work/sub/deep" -Nni descendant; ok
# In a non-Git workspace discovery still uses cwd only; permission inheritance
# does not widen instruction discovery or approve siblings by string prefix.
run -C "$work-sibling" -Nni denied_sibling; bad
ln -s "$work-sibling" "$work/escape"
run -C "$work/escape" -Nni denied_symlink; bad
plan '{"plan":["ENV BYPASS"]}'
CAI_INSECURE=true run -C "$work-sibling" -Nni env_bypass; ok
run -C "$work-sibling" -ni still_unapproved; bad
printf '[agent]\ninsecure=true\n' >"$root/insecure.ini"
plan '{"plan":["INI BYPASS"]}'
run --config "$root/insecure.ini" -C "$work-sibling" -Nni ini_bypass; ok
run --config "$root/insecure.ini" --no-insecure -C "$work-sibling" -ni denied_override; bad
# --yes and --force are interchangeable; -k dominates and never saves approval.
plan '{"plan":["FORCE BYPASS"]}'
run --force -k -C "$work-sibling" -Nni no_save; ok
run -C "$work-sibling" -ni still_no_save; bad
plan '{"plan":["FORCE APPROVED"]}'
run --force -C "$work-sibling" -Nni force_approved; ok
plan '{"plan":["FORCE REUSED"]}'
run -C "$work-sibling" -ni reused; ok
mkdir "$root/untrusted-import"
jq -c --arg path "$root/untrusted-import" \
  'if .type=="workspace_approved" then .data=$path else . end' \
  "$exported" >"$root/untrusted.jsonl"
run -C "$root/untrusted-import" --import "$root/untrusted.jsonl"; ok
run -C "$root/untrusted-import" -ni imported_history_cannot_approve; bad
# Consent belongs to the directory tree, independently of its audit session.
run --delete "$id"; ok
plan '{"plan":["APPROVAL SURVIVES SESSION DELETION"]}'
run -Nni retained_directory_approval; ok
contains "$root/stdout" 'APPROVAL SURVIVES SESSION DELETION'
