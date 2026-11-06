# Conversations, history and exports

## Find and resume a conversation

Starting `cai` resumes the newest session for the canonical current directory.
`-N`/`--new` starts fresh; `/new` does the same inside the prompt and confirms the new session ID and workspace in a Markdown summary after opening it successfully.
When messaging is enabled, the summary includes its active queue.
`-l`/`--list` shows a numbered conversation list for the current directory.
`-L`/`--list-all` lists conversations across all directories in the configured session store.
`-R`/`--resume` without an ID and `/resume` show the same numbered list for the current directory; `/resume <number-or-ID>` loads an entry.
Rows show ID, directory, and a first-prompt preview, clipped to the terminal width or 80 columns without a terminal.
`-R ID`/`--resume ID` addresses a saved session in the current directory directly.
`-R NUMBER`/`--resume NUMBER` selects the corresponding row from that directory's numbered list, with the same ordering as `/resume`.
An exact numeric ID, if present in the directory, takes precedence over a row number.
Saved prompts, assistant text and concise execution summaries replay without repeating tool side effects.

## Replay and prompt recall

Interactive prompt recall uses Softline's custom backend hooks and encrypted `prompts.<session-id>` collections in the configured sessions namespace, beside the conversation journal.
It shares the session store's Pouch client and key; there are no plaintext `.history` files or separate history-directory settings.
Recall loads only the newest 200 entries by default, oldest first, through a bounded collection range.
Configure recall and the native stored-entry cap with `--prompt-history-limit`, `CAI_PROMPT_HISTORY_LIMIT`, or `[ui] prompt_history_limit` (also accepted through C/Lua application settings).
Zero disables recall and recording and clears the stored count cap without deleting saved prompts.
`/history` shows the current window; `/history COUNT` changes it for this app invocation.
Liblockdc's existing maintenance worker lazily removes the oldest entries beyond `max_entries`; appends never scan or trim prompts synchronously.
The stored cap survives restarts and is updated when recall is configured.
Raising it cannot recover prompts already removed by maintenance.
Conversation journals are never subject to this count cap.
Accepted prompts and local commands append immediately, including steering; replay and Ctrl-R selection do not append.
`/new` starts fresh recall; `/resume` restores the selected conversation's recall.
Each entry appends synchronously without rewriting earlier entries, using the store's configured durability and compression.
Recall is separate from model context and conversation export; deleting a conversation drops its prompt collection too.
Temporary chats use their private session store for recall.
Existing file-backed recall is not imported.
Ctrl-R Enter selects a match for editing; Enter again sends. **Alt-Q** saves the active draft as the newest prompt-history entry and exits without submitting it.
After restarting in the same workspace, press Up to recall it.
Empty drafts add nothing; an identical last entry is not duplicated.
If saving fails, CAI stays open with the draft intact and reports the error.

Console replay defaults to the newest 10 whole turns.
Use `--replay-turns N`, `CAI_REPLAY_TURNS` or `[ui] replay_turns`; `0` hides replay and `-1` displays all history.
Replay does not repeat tool effects.
This display limit does not remove model context or export data.

## Retention and deletion

Conversation journals have a default **30-day inactivity retention**, enforced by liblockdc on the whole `events.<session-id>` collection and its session metadata.
Every successful append renews the deadline; individual journal entries have no separate expiry.
Configure the default through `--conversation-retention-days`, `CAI_CONVERSATION_RETENTION_DAYS`, or `[storage] conversation_retention_days` (also accepted through C/Lua settings).
Zero disables expiry.
`/retention` shows the active conversation's policy; `/retention DAYS` persists its override, and `/retention 0` or `/retention off` keeps it forever.
Changing it waits for a whole-turn boundary through Softline.
Expiry is eligibility for asynchronous SDK maintenance, not exact-time deletion.
Reads do not renew it.
After SDK cleanup, lists omit unavailable conversations and auto-resume selects an available conversation or starts fresh.
Explicitly resuming a removed journal reports that it is unavailable.
Prompt-history aging/pruning remains deferred; conversation retention does not silently expire individual prompt records, exports, or logs.

`-D` / `--delete` lists numbered conversations eligible for deletion in the current directory.
`-D ID` or `-D NUMBER` deletes that conversation, its child and review collections, metadata, managed exports, prompt history, session logs and delivered inbox messages.
Numbers refer to the deletion list; exact numeric IDs take precedence.
Open conversations are omitted and cannot be deleted until closed.
Filesystem locks on the shared Pouch root release automatically on process exit/crash.
Shared authentication, boards, mailboxes and workspace files remain separate.
Deletion is logical; Pouch reclaims dropped collection storage during maintenance.

## Trim history

`/trim` lists the newest 20 complete history entries, newest first.
Row `0` means keep the current history; row `1` identifies the newest entry, row `2` the preceding one.
`/trim N` removes exactly the newest N entries.
`/trim 0` does nothing.
`/trim list N` changes the listing length for that command.
Summaries use the assistant response, falling back to the prompt or shell command, collapse whitespace, and fit the terminal width through libmdf.

A turn includes its reasoning, steering, tool calls and tool results.
A standalone recorded shell execution is one entry; an execution started during a model turn belongs to that turn.
Compacted context is one indivisible entry, including any tail retained by the compactor.
It is never expanded into the original turns.
Boundaries are persisted as incremental journal metadata; steering records remain part of their turn.

Listing and removal require an idle runtime with no queued work, open shell, pending settings, or review handoff.
Removal atomically deletes the selected events and dependent context commits, then appends an edit based on the retained commit prefix.
A failure before publication restores the live history; an uncertain publication blocks writes until reopening.
Restart, subsequent provider requests, and conversation exports exclude trimmed entries.
Diagnostic logs and external tool stores are not erased.
Changes to files, tool stores, selected settings, goals, and incurred usage are not undone.
CAI clears provider continuation handles so discarded context cannot return via an earlier response or conversation ID.

## Compact model context

`/compact` starts a standalone compaction task between turns.
While busy it stays in Softline's native editable queue.
Alt-Enter becomes `Q` and cannot waive the whole-turn hold or steer the model.
Escape cancels the task.
Progress appears in the gray status line and completion in the Markdown feed.
Automatic worker compaction also displays `Compacting context` in that status line until completion, failure or cancellation.

The worker uses the same compactor as automatic compaction: streamed Responses V2 `compaction_trigger` for Responses providers, matching Codex's manual task, or client-side summarization for Chat Completions.
It does not submit a user prompt, execute tools or generate a follow-up answer.
A successful replacement is committed before completion, retains instruction files for subsequent requests, and appears as one indivisible history entry.
Failure or cancellation before the replacement commits preserves the prior history.
An unfinished manual request is not automatically retried after a crash.

## Export and import

`--export ID` and `/export` write paired `ID.jsonl` and `ID.md` files under `$XDG_DATA_HOME/cai/exports/ID` by default (falling back to `~/.local/share/cai/exports/ID`).
`/export` selects the active session.
`--export-dir DIR` overrides the export root.
`/export BASENAME` exports the active conversation to the named path; `--export ID --export-out BASENAME` does the same for an explicit ID.
An exact `.md` or `.jsonl` suffix selects only that format.
Otherwise both suffixes are appended, so `/export output/conversation` creates `output/conversation.md` and `output/conversation.jsonl`; `something.else` creates `something.else.md` and `something.else.jsonl`.
Relative paths use the workspace; missing parent directories are created without changing existing directory permissions.
Existing files are replaced after the selected streams have been written.
Omitting the basename (or leaving `--export-out` empty) retains the default export location.
JSONL contains the complete resumable commit and journal; Markdown uses libcai's existing Markdown exporter, including its instructions, conversation, runtime, and goal sections, followed by a structured execution timeline.
Export performs no model requests or tool execution.
These exports are plaintext private files (mode 0600); exported state contains conversation and execution context, never the credential namespace.
`--import FILE` validates a JSONL export, assigns a new session ID in the current directory, and exits.
Failed imports never appear in the list.
Existing cai JSONL files are not automatically migrated.

Execution history records agent/harness activity directly; it does not copy pslog output.
It retains tool names and call IDs, exact arguments or raw custom input, returned output, partial output on failure, and complete error fields (code, HTTP status, message, detail, provider code and request ID).
It also records response/turn outcomes, request failures exposed to the agent loop, compaction outcomes, goal controls and provider-supplied hosted-tool call items.
Delegated records retain their child session and originating parent call IDs; manual review includes the child's actual instruction input.
Model, endpoint and last committed response IDs provide reproduction context without recording the HTTP client's authentication headers.

These `execution` journal records share the session's collection and append order.
Logical payloads use ordered UTF-8 chunks of at most 4096 bytes, with byte offsets and a final marker.
Their source journal sequence is retained when a record is forwarded to a parent.
This adds no per-record threads, attachments or full-payload staging.
The tool API's existing captured output is read incrementally into the journal when it becomes available.

Subagents also copy their durable journal events into the parent journal, independently of live event callbacks.
`subagent` events contain bounded `cai_execution_record` envelopes: `kind` names the original event, and `session_id`, `subagent`, `parent_session_id`, `parent_call_id` and `source_sequence` identify its origin.
Outer sequence numbers order the parent timeline; `offset` and `final` describe chunks of one child event.
Existing child `execution` records retain their type and attribution, without duplicate copies from the UI pump.
This adds no worker threads or attachments.

Replay labels delegated activity, preserves assistant progress and reasoning summaries, and shows validated review findings as Markdown.
Raw review JSON remains in the inspection timeline, not the normal findings display.
Failed review prose and failure details remain available without a logfile.
Successful tool calls use the live tool presentation with one completion heading.
Commands show `* Ran COMMAND (1m3s)` followed by their saved output, without a `Running` row.
Live commands show `Running` after two seconds or before their first output, whichever comes first.
Fast, silent commands show only `Ran`.
Output continues streaming immediately; no output is buffered to wait for the heading.
Durations below one second use milliseconds, for example `(23ms)`.
The duration and parentheses use the same dimmer color as `Ran`, while the command stays brighter.
`write_stdin` output stays attached to the original command and shares its preview limit.
`--tool-output compact|full` controls replay just as it controls new calls.
Saved patch previews retain confirmed line numbers and neighboring context; older records containing only submitted patches cannot recover missing context from files that have since changed.
Startup policy notices precede replay; the idle message distinguishes a resumed conversation from a new conversation.
Previously unrecorded child output cannot be recreated by this change.

Live command completions use the same elapsed-time heading, with seconds, minutes or hours as needed.
Activity and duration use the effective theme's H3 foreground at normal weight; the command uses H2 foreground in bold.
Fast commands may omit the live completion row, but still have a saved completion and output on replay.

Execution records and subagent envelopes never enter model context, recovery actions or compaction prompts.
Context compaction preserves them; explicit history trimming excludes the corresponding entries from replay/export.
JSONL preserves the complete records, while console replay uses tool receipts, bounded output and failures.
Invalid side-band payloads produce a historical warning rather than blocking context recovery; the original record remains in JSONL.
Internal debug/trace logs and ordinary application warnings remain in the pslog file.

For example, inspect tool failures without the log file:

```sh
jq 'select(.type == "execution") | .data | fromjson | select(.kind == "tool.failed")' ID.jsonl
```

The [native export contract](workspace-export.md) describes captured views, streaming and publication.
The [journal reference](journal.md) covers storage records; [troubleshooting](troubleshooting.md) covers logs and recovery.
