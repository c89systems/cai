# Using the terminal agent

Start `cai` in a project directory.
It resumes the newest conversation there; `cai -N` starts another.
`cai -R ID` or `cai -R NUMBER` selects that conversation before replay, including when attaching to an existing workspace server.
`cai -R --all` lists conversations across the history store; use `-C DIR -R ID` to resume one in its workspace.
`cai -D --all` lists closed conversations eligible for deletion across that store. `cai -D ID` deletes one by its store-wide identity from any directory; numeric choices remain local.
Startup selection does not submit a `/resume` command or replay a different conversation first.
Ctrl-D on an empty draft follows the same goodbye and detach path as `/quit` and Alt-Q; with a nonempty draft it retains the editor's forward-delete behavior.
First-time workspaces require `cai -y` to save approval for that directory and its descendants.
`-k` bypasses approval for this invocation only.
Use `cai --login` for ChatGPT authentication if no usable imported or stored credentials exist.
See [configuration](configuration.md) and [providers](providers.md) for other setups.

Interactive startup prints the welcome and application banner before attachment or replay.
`--no-banner`, `[ui] no_banner = true`, or `CAI_NO_BANNER=true` suppress this startup text; `--banner` overrides that setting.
Attachment readiness still appears, and utilities and batch/headless runs do not print the welcome.

```sh
cai -y
cai -N
cai -R
cai -R 2
cai -n -i 'Explain the failing tests'
```

## Commands to start with

| Command | Purpose |
|---|---|
| `/help` | Commands and editor shortcuts; `/help -p` opens rendered help in the pager. |
| `/status` | Model, usage, session/response IDs, effective policy and log path. |
| `/model`, `/m` | Inspect or change model and optionally reasoning effort. |
| `/reasoning`, `/r` | Inspect or change reasoning effort. |
| `/theme`, `/t` | Inspect or change the Softline/libmdf theme pair. |
| `/goal` | Inspect the goal; `new`, `pause`, `resume` and `clear` control it. |
| `/new`, `/resume` | Start or select a conversation in this workspace. |
| `/tools`, `/skills`, `/mcp` | Inspect tools/skills and manage configured MCP connections. |
| `/export [BASENAME]` | Export the active conversation; see [format selection](conversations.md#export-and-import). |
| `/log`, `/log -p` | Show the log path or inspect the log in a pager. |
| `/quit` | Detach this frontend; the last detach stops a nonpersistent owner. |

## Input and queue

Enter submits ordinary input: it starts work when idle and otherwise queues a later turn.
Alt-Enter submits steering for the next response/tool boundary.
Steering never interrupts a streamed assistant message.
Accepted input belongs to the backend queue for that conversation; Softline provides its visible, editable view.
`/new` and `/resume` switch queues along with conversations.
Read-only commands can run between feeds.
Changes to model, reasoning, tier and theme wait for a safe response/tool boundary.
Whole-turn operations such as `/review`, `/compact` and `/yolo on|off` wait until idle.
Unsupported steering commands become ordinary queued entries.
See [command policies](interactive-command-audit.md) for the complete boundary inventory.

| Key | Action |
|---|---|
| Enter / Alt-Enter | Ordinary submission / steering or explicit queue promotion. |
| Escape / Ctrl-C | Stop active work; an active goal is durably paused. |
| Alt-E | Edit pending queue input. |
| Alt-A / Alt-Y / Alt-N | Focus approvals / approve / reject or discard the selected entry. |
| Alt-K / Alt-J | Stash the draft / append the stashed draft. |
| Alt-S | Open a local shell while retaining the draft. |
| Alt-Q | Save the draft as the last prompt-history entry and detach. |
| Ctrl-Y | Yank from the editor's kill ring. |
| Ctrl-V | Paste an X11 clipboard image as a private file path. |
| Tab | Accept/cycle completion; slash commands, `@` paths and `$` skill names. |
| Right / Ctrl-F | Commit a completion and preview its continuation; otherwise move forward normally. |

An unsubmitted draft remains local.
Editing an accepted entry holds it in the backend; cancelling an edit does not duplicate or execute it.
Escape stops automatic delivery of the existing backlog; an explicit new `/goal resume` does not release unrelated stopped entries.
`@` and `$` completion remove the trigger before submission; slash command prefixes remain.
Tab cycles files and directories in the current list without entering a directory. Right or Ctrl-F commits the selected directory and previews its children; Tab accepts that preview. Repeated forward keys at the end of a completed file keep its name intact.
Enter sends actual draft text, not an unaccepted suggestion.
See [completion limitations](troubleshooting.md#completion-triggers).
For all movement, selection, undo, history and queue shortcuts, use `/help` in the running frontend.

Ctrl-V requires `DISPLAY`, including forwarded X11, and saves PNG/JPEG files privately under `${XDG_CACHE_HOME:-$HOME/.cache}/cai/clipboard/`.
These files remain after exit until removed; ordinary terminal paste shortcuts still paste clipboard text.
Ctrl-Y uses the editor's kill ring rather than the system clipboard.

## Goals

`/goal` or `/goal status` shows the current durable goal and accounting.
Prefer asking the agent to create and manage goals.
For explicit control, `/goal new TEXT` replaces any current goal without confirmation, resets its accounting and token budget, and starts pursuit.
`/goal pause` pauses at the next safe model/tool boundary; `/goal resume` resumes pursuit.
`/goal clear` removes goal tracking while allowing the current user turn to finish.
`/goal help` renders the command table.
Unknown commands and missing objectives are errors; they never become goal instructions.
Commands remain responsive during a turn, and goal state survives session replay.
Goals created by model tools, slash commands, startup options or host controls use the same pursuit loop: an active goal continues after each completed turn and when resumed from storage.
Approved queued prompts take priority over generated continuations.
Escape/Ctrl-C cancels the active turn and durably pauses its goal, including across restart.
`/goal resume` restarts pursuit immediately, including when the last turn was cancelled or failed.
An already-active unfinished goal can also be resumed without resetting accounting; an achieved goal cannot.
Ordinary user turns still work while paused.
The model may pause with `update_goal(status="paused")` when requested or when it needs a user answer or approval, then ask and finish the turn.
In a later turn it may decide to resume with `update_goal(status="active")` without an explicit resume request, respecting outstanding approvals and instructions to remain paused.
Completed goals cannot be resumed.
Provider failure stops automatic pursuit for the current invocation until another user turn or `/goal resume`, leaving goal state unchanged.
Completed or stopped pursuit leaves the interactive prompt open.

Automatic continuations use the embedded Codex goal prompt, including the current objective, budget, progress checks, and completion/blocked audits.
CAI omits unavailable planning-tool guidance and adapts pause/resume policy.
Three consecutive empty automatic turns with no text, reasoning, tool or compaction activity block the goal; user turns and activity reset the streak.
`/goal resume` can restart a blocked goal.
No runtime template file is required.
C hosts use `cai_agent_runtime_continue_goal(runtime, &started, &error)`; Lua hosts use `runtime:continue_goal()`, returning whether a continuation started.
Pending user work or goal controls take priority, and completed goals never start continuations.

## Models and reasoning

`/model [MODEL [EFFORT]]` shows the current and pending settings and the provider catalog, or changes model and optionally reasoning effort.
`/model high` changes only effort; `/model MODEL` keeps the current effort.
`/reasoning [EFFORT]` changes effort or lists the selected model’s advertised choices and provider default.
If model-specific choices are unavailable, it says so rather than claiming support.
Unsupported advertised choices are rejected.
ChatGPT lists every model returned by the signed-in account catalog, including new IDs absent from CAI's compiled metadata; no local visibility, API-support, or minimum-version filtering is applied.
OpenRouter skips catalog listing; other providers show at most 50 models, and any model ID can still be selected explicitly.

Idle changes apply immediately; active requests and tools finish before the change applies at the next durable tool-round or turn boundary.
Subsequent changes merge into pending settings.
Applied settings are committed atomically and restored on automatic or explicit resume; explicit `-m`, `-r`, and `--reasoning-summary` independently override saved values.
`/new` uses the startup defaults.
Catalog discovery uses a separate client and a five-minute cache; it makes no additional startup request.
Explicit `/model` and `/reasoning` listings refresh discovery using the configured client request timeout.
A failed refresh retains the last catalog when available; repeat the command to retry immediately.
Every delivered slash command is printed with a green `$` before its handler starts, including discovery and other operations that may wait.
Pending commands stay editable in Softline until delivery.
The marker acknowledges submission; failures have a separate red diagnostic.
Literal, non-italic command text uses the effective theme's H2 foreground in bold.
Interactive warnings and errors use a red `*` with `WARN:` or `ERROR:` and readable details, with one blank line between feed blocks.
Complete diagnostic records stay in the session log.

## Service tiers

`/tier` cycles available tiers; `/tier list` displays choices and the selected and pending tier.
`/tier NAME` selects one.
`/fast` toggles Fast (`priority`, or the provider's advertised `fast` ID) and explicit normal routing (`default`).
Changes apply at the next safe model/tool boundary and persist with the session.
Enter and Alt-Enter both use this next-request boundary for tier changes, even during an active turn.
The command remains editable in Softline until applied; the request already in flight keeps its original tier.
Routing commands are independent of queued conversation prompts and do not submit model input.
`--service-tier NAME`, `CAI_SERVICE_TIER`, and `[agent] service_tier` provide startup overrides.
An unset value restores saved routing, or leaves new-session routing to the provider.
CAI never opts into a paid tier from a catalog default automatically.
Explicit normal routing omits the tier on ChatGPT requests, matching Codex; the public OpenAI API receives `"service_tier":"default"`.

ChatGPT's signed-in model catalog supplies `service_tiers` and `default_service_tier`.
OpenAI's published API request options are shown when model metadata is absent; the provider still validates account/model access.
For other providers, explicit tier IDs can be sent when metadata is missing; cycling and enabling Fast need known choices.
A fallback models.json entry may declare `"service_tiers":["priority","flex"]` and an optional `"default_service_tier":"priority"`.
Live metadata takes precedence.

## Reviews

`cai --review` runs one isolated review against the Git default branch and exits.
`cai --review uncommitted` explicitly reviews staged, unstaged, and untracked changes.
Resolution uses local `origin/HEAD`, then requires exactly one local `main`, `master`, or `trunk`; it never fetches.
A missing or ambiguous default requires `--base REF` or another explicit scope.
`--base REF` selects a base reference, or one `-i TEXT` supplies custom review instructions.
Activity, assistant output, and reasoning are pslog records on stderr; the validated Markdown findings report appears on stdout.
`-o FILE` writes only the findings to that file and leaves stdout empty.
`-T json` selects the validated structured report instead of Markdown.
Failure returns a nonzero exit status.
`cai --review-and-fix` starts a new non-interactive Smith goal that invokes the built-in review subagent, fixes actionable issues, verifies, and repeats until a clean review; stopped or failed goals return nonzero.
It has no default iteration limit.
Review model and reasoning flags apply to review subagents in both workflows.
Review-and-fix uses the same default base; `cai --review-and-fix uncommitted` selects uncommitted changes instead.
The default `run_review` tool performs one isolated review.
A request to review, or a later request to fix its findings, does not authorize repeated reviews or a review/fix goal.
The optional `run_review_and_fix` tool requires both explicit opt-in and a user request or permission to run a loop.
Enabling the tool alone does not grant that permission.
It replaces the current conversation's goal and queues the same review/fix/verify workflow as the explicit command; it does not launch an isolated fixing agent.
It requires goal tools and the reviewer and is never inherited by subagents.
Its optional `target` is `base` or `uncommitted`, with `base` required for `target=base`; no arguments use the Git default branch.
The tool acknowledges the queued goal, rather than waiting for the loop to finish.
No token budget is imposed.
In the TUI, `/review` starts an isolated reviewer between turns and preserves the current conversation.
`/review uncommitted`, `/review base REF`, `/review commit SHA`, and `/review TEXT` select an explicit scope.
Findings are rendered as Markdown and become context for subsequent prompts.
Escape/Ctrl-C cancels the review; prompts sent while it runs queue for the parent conversation.
`/review-and-fix` starts the same review/fix/verify goal in the active conversation, replacing its current goal without opening a new session.
It compares against the Git default branch; `/review-and-fix uncommitted` or `/review-and-fix base REF` selects another scope.
It has no default iteration limit and leaves the TUI open when pursuit completes or stops.
Goal and review tools must be enabled; `/goal pause` and `/goal clear` control pursuit.
The C API's zero-initialized review request and Lua's `submit_review()` or `start_review()` use the same default.
`cai_agent_review_default_base` and Lua `cai.review_default_base(workspace)` expose local base discovery to hosts.

## Shell commands and handoff

Exact `!sh` and `/shell` commands hand the real terminal to `$SHELL -i` (`/bin/sh -i` when unset or empty) between turns.
They require terminal input and output.
While busy, typed shell commands remain editable in Softline's queue until a whole-turn boundary; Alt-Enter cannot interrupt a turn to hand off the terminal.
The shell runs with normal user permissions in the workspace.
On exit, CAI restores terminal ownership, prompt settings and current geometry, including after a screen clear or resize.
The shell uses the terminal directly, with native window changes and foreground-job control.
Terminal output stays local: it is neither journaled, logged nor sent to the model, including with `--shell-context` enabled.
Logs retain only shell start/exit metadata.
A failed launch or nonzero shell exit reports details and returns to CAI.

**Alt-S** opens that same local shell between turns while retaining the current draft, cursor and native editing state, including undo.
Exit the shell to resume editing; the shortcut does not submit the draft or add it to prompt history.
The command forms return to an empty prompt. **Ctrl-L** refreshes terminal layout without discarding the draft, queues or transcript.

`/shellshare` and `!sh+` explicitly share the shell's newest sanitized output when it exits.
These shared variants use a PTY relay for capture.
This applies to that invocation even if ordinary bang-command sharing is disabled.
The same rolling-tail storage described below applies; older output is discarded as new text arrives.
A shell that has not finished does not save its captured output.

Other `!COMMAND` inputs run captured commands as described below.

Enter `!COMMAND` to run your shell (`$SHELL`, falling back to `/bin/sh`) with normal user permissions in the workspace, outside the agent sandbox.
Each invocation uses `-c`, pipes and closed stdin, without a PTY or login profiles.
Output appears literally, without Markdown parsing or preview truncation.
UTF-8, newlines and tabs are preserved; other controls and invalid bytes are shown as `\xNN`.
Stdout and stderr share one pipe.
Ctrl-C/Escape cancels the shell first; quit also cancels it.
Commands have a one-hour timeout, and their background descendants are terminated at completion.
Ordinary prompts entered while a shell runs stay in Softline's visible, editable queue until its result has been recorded; they are not moved into libcai's internal turn queue.

Commands are journaled at start; only the newest sanitized output is saved at completion.
Replay displays that tail without executing commands.
By default, a completed result becomes tagged user-role context, including exit code and elapsed time, at the next safe model/tool boundary.
It does not start inference, steer the agent or advance a goal turn.
A rolling buffer retains up to 5,000 logical lines and the configured byte ceiling (4 MiB by default), further limited by the replay parser's dynamic-string bound (currently 128 KiB), including completion metadata and a truncation notice.
The newest text that fits is kept; earlier text falls out of the buffer.
The saved tail, logged output and model copy use that same text.
`--shell-context-max-lines N` and `--shell-context-max-bytes N` override `CAI_SHELL_CONTEXT_MAX_LINES` / `CAI_SHELL_CONTEXT_MAX_BYTES`, which override `[runtime] shell_context_max_lines` / `shell_context_max_bytes` in `config.ini`.
Zero in SDK/INI selects the default; CLI values must be positive (bytes >=4).
An oversized line keeps its newest UTF-8-aligned suffix.
ANSI/VT colors, CSI and OSC/DCS/control strings are removed before retention; CR, backspace and horizontal cursor/erase controls update the current logical line.
Incomplete controls are dropped, invalid UTF-8 is replaced, and split controls/UTF-8 are handled across reads.
This is a text transcript, not a full screen or alternate-screen emulator.
JSONL contains only the retained tail.
Markdown export uses the existing libcai exporter of retained model context.

`--no-shell-context`, `CAI_SHELL_CONTEXT=false`, `[ui] shell_context=false`, or `/shell-context off` keeps future `!COMMAND` results out of model context while still executing, displaying and saving them.
`/shell-context on` enables sharing; `/shell-context` shows the setting.
These controls never enable recording or sharing for `/shell` or `!sh`; `/shellshare` and `!sh+` explicitly opt in for one invocation.
The setting is captured when each command starts.
Earlier shared results stay in context.
Unshared results replay locally and remain in JSONL exports.

## Batch input

`-C DIR`/`--directory DIR` changes directory before the agent opens its session.
Repeat `-i TEXT`/`--instruction TEXT` to run prompts in order, after the saved conversation has replayed.
`-g TEXT`/`--goal TEXT` starts or replaces a durable goal; the agent keeps working until the goal completes or stops.
`-n`/`--non-interactive` runs the supplied turns or goal and exits without reading terminal input.
For example, `cai -Nni 'write a snake game in Lua' -i 'rewrite it in Go'` starts fresh, completes both turns, then exits.

With redirected input and no supplied instruction, goal or review, CAI accepts newline-separated prompts and slash commands through the workspace server.
Responses and tool activity render as they arrive, without an editor, status rows or interactive margins.
Commands retain their normal scheduling: `/status` can run while inference is in flight, and `/goal pause` applies at a request boundary.
EOF or `/quit` stops further admission and drains accepted work.
The final line need not end with a newline.
`-s NAME` selects an existing owner for either terminal or redirected input.

Batch invocations use the same workspace owner and accepted-work identity as interactive prompts.
Stdout contains the final selected top-level reply without interactive margins; activity and diagnostics use stderr.
See [batch attachment](workspace-batch.md) for host controls and settlement.

## Workspaces and server lifetime

`cai -x` starts a persistent headless server in the current directory.
Ordinary `cai` attaches to an existing owner or quietly starts a nonpersistent one.
`cai -s NAME` attaches to an existing owner by name, workspace path or stable ID.
`/workspace` lists running owners; `/workspace SELECTOR` switches the frontend.
Drafts, accepted input and recall stay with their original workspace/conversation.
One attachment controls the owner; other attachments observe until explicit `/takeover`.
`/shutdown` stops the selected server; `/quit` detaches all visited workspaces.
A persistent server continues after the last detach.
See [terminal attachment](terminal-attachment.md) and [server architecture](workspace-server.md) for lifecycle and SDK details.

## More guides

- [Conversations, replay, retention and exports](conversations.md)
- [Themes, status fields and timestamps](appearance.md)
- [Configuration, workspace approval and temporary runs](configuration.md)
- [Troubleshooting and logs](troubleshooting.md)
