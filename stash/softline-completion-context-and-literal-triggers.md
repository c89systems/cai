# Softline completion activation needs a host-context veto

Observed with Softline 0.16.0 on 2026-11-04. This is independent of the CAI
workspace-path refactor: both the preceding x86_64 musl fixture binary and the
changed native Debug binary reproduce it.

CAI registers `$` skill and `@` filename providers with `SL_COMPLETION_WORD`
and `strip_trigger = 1`, as required for prompt completion. Shell input uses
`!COMMAND` in the same editor, and must preserve its literal bytes.

## Reproduction and evidence

1. Enable those prompt providers.
2. Type `!kill -TERM $$` and press Enter, without selecting a completion.
3. The submitted command is `!kill -TERM $` and `/bin/sh` reports
   `kill: Illegal number: $`, exiting 2 instead of terminating itself.

CAI's `tests/cli_shell_test.sh` reproduces this at its explicit shell-exit-143
assertion. Logs in this checkout:

- `build/workspace-path-focused-tests.log` (changed native code)
- `build/workspace-path-baseline-shell.log` (preceding musl fixture)

The command_input journal event itself contains the single `$`; this is not a
renderer artifact. The completion callback never emits a shell command.

The installed public documentation says strip_trigger applies on normal exit
or submission. Upstream `src/softline.c`, `sl_completion_finish`, removes an
active provider's trigger unconditionally, including when the token has no
matching candidates or no candidate was accepted. `sl_completion_prepare`
activates WORD providers solely by token position, including inside `!` shell
input. Returning no candidates does not decline activation. Returning an error
disables stripping but also sets an editor error and rings the bell, so it is
not a suitable ordinary context veto.

## Requested API and semantics

Add an optional provider activation predicate, evaluated on the input owner
before activation, snapshots or stripping. The host can inspect the draft and
cursor through public APIs and decline completions in shell input or another
literal context. Declining must preserve all text, produce no bell/error, and
leave ordinary editing, queueing and steering behavior unchanged. It must not
be a failed-provider result or require per-keystroke provider reinstallation.

Also consider preserving unmatched literal triggers at normal exit/submission:
a completion trigger should not silently corrupt `$5`, `$$` or an unmatched
`@` token merely because a provider exists. Matched/accepted completions should
still strip their configured prefix, including directory continuations and
manually typed valid candidates. Keep this decision inside Softline's editor
and undo model rather than asking hosts to repair submitted text afterwards.

CAI would use the predicate to disable its prompt-only `$`/`@` providers for
`!` commands. It should not guess how to reconstruct already stripped text.

## Verification requested

Cover literal shell PID/environment variables and `@` text, no-match prefixes,
normal prompt skill/file acceptance and prefix removal, directory continuation,
Undo, paste/yank, draft replacement, queued/steered submission and editing a
queued shell command. Vetoed activations must neither run callbacks nor emit
errors/bells; ordinary matched completion behavior must remain intact.
