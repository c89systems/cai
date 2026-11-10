# Tool execution and display

[Smith](runtime.md) supplies native file, patch, command, goal and review tools.
MCP connections and dynamic tools use the same core runtime; `/tools` shows the active inventory.

## Tool policy

Smith enables hosted `web_search` on Responses providers and the ChatGPT Chrome extension tool by default.
Browser discovery is lazy: ordinary turns work without Chrome, and a browser call reports missing setup or connection.
`cai --chrome-setup` registers cai's own native host.
Setup refuses conflicting registrations; `--chrome-remove` removes only cai's verified registration.
No OpenAI native executable is required.
See [Chrome setup, API, and compatibility](chrome.md).

Tool policy uses CLI > environment > `config.ini` > built-in defaults.
`[env]` is applied first with its documented environment precedence rule.
All boolean environment settings accept the INI boolean syntax.
The following controls apply at startup, including when resuming a session:

| Tool | CLI (also `--no-NAME`) | Environment | INI | Default |
| --- | --- | --- | --- | --- |
| Hosted search (Responses only) | `--web-search` | `CAI_WEB_SEARCH` | `[agent] web_search` | on |
| Chrome extension | `--chrome` | `CAI_CHROME` | `[chrome] enabled` | on |
| Kanban | `--todo` | `CAI_TODO` | `[agent] todo` | off |
| SearXNG function search | `--searxng` | `CAI_SEARXNG` | `[agent] searxng` | off |
| Reverse geocoding | `--revgeo` | `CAI_REVGEO` | `[agent] revgeo` | off |
| File reading | `--read-file` | `CAI_READ_FILE` | `[agent] read_file` | on |
| File listing | `--list-files` | `CAI_LIST_FILES` | `[agent] list_files` | on |
| Workspace patching | `--apply-patch` | `CAI_APPLY_PATCH` | `[agent] apply_patch` | on |
| Local images | `--view-image` | `CAI_VIEW_IMAGE` | `[agent] view_image` | model dependent |
| Goal tools | `--goals` | `CAI_GOALS` | `[agent] goals` | on |
| Skills | `--skills` | `CAI_SKILLS` | `[agent] skills` | on when installed |
| Managed commands | `--terminal` | `CAI_TERMINAL` | `[agent] terminal` | on |
| Image generation | `--image-generation` | `CAI_IMAGE_GENERATION` | `[agent] image_generation` | on |
| Review subagent | `--review-subagent` | `CAI_REVIEW_SUBAGENT` | `[agent] review_subagent` | on |
| Review/fix loop tool | `--review-and-fix-tool` | `CAI_REVIEW_AND_FIX_TOOL` | `[agent] review_and_fix_tool` | off |
| Foreground child steering | `--subagent-steering` | `CAI_SUBAGENT_STEERING` | `[agent] subagent_steering` | on |
| Codex global/ancestor instructions | `--codex-agents-md` | `CAI_CODEX_AGENTS_MD` | `[agent] codex_agents_md` | off |

Startup flags, environment and `config.ini` determine instruction settings; editing the INI requires a restart.
`/resume` keeps those startup settings and reads the instruction files again.
Session storage never supplies configuration or an old global instruction snapshot.
Logs identify each global file's resolved path and loaded byte count, or explicitly report disabled Codex compatibility.

`--searxng-url`/`CAI_SEARXNG_URL` override `[searxng] base_url`; `--revgeo-url`/`CAI_REVGEO_URL` override `[revgeo] base_url`.
`--chrome-socket`/`CAI_CHROME_SOCKET` select `[chrome] socket`.
The generated template documents service timeouts, result limits and language settings.
SearXNG is available to Chat Completions providers without hosted search.
CAI's optional kanban uses one shared encrypted `cai.todo` document, with bounded streaming reads/writes and transaction commit/rollback; it creates no `todo.json` file.
Isolated reviews omit these optional tools and browser access.
Ordinary subagents inherit narrowed local capabilities and hosted search policy, but receive no Chrome connection or optional kanban/service tools.
The separate sandboxed exec preset remains an SDK alternative to the managed terminal pair; registering both under `exec_command` is not supported.

## Native runtime configuration

The equivalent libcai runtime fields are `disable_web_search`, `disable_chrome`, `chrome_config`, `enable_todo`/`todo`, `enable_searxng`/`searxng`, `enable_revgeo`/`revgeo`, and the individual `disable_read_file`, `disable_list_files`, `disable_apply_patch`, `disable_view_image`, `disable_goals`, `disable_skills` policies.
The standard INI parser projects them through `apply_runtime`.
SDK hosts enabling kanban can supply its streaming store callbacks; without them the SDK's existing file store applies.
Preset capability masks also gate search and browser access: `CAI_AGENT_PRESET_TOOL_WEB_SEARCH` and `CAI_AGENT_PRESET_TOOL_CHROME` are set by Smith; custom descriptors grant them explicitly.
Runtime disable policies can narrow these grants.
Global instruction load records include the source, resolved path and byte count in the session log, including after replay.
Session state never supplies CAI startup configuration.

## Managed commands and yielding

`exec_command` starts a child process with pipes by default; `tty=true` allocates a PTY.
Only one managed process may run at a time. A second launch returns a structured busy result identifying the active handle, rather than starting another process.
The model uses `write_stdin` to send input and poll output from the active command.

Background mode waits up to the configured interval before yielding a still-running handle; yielding does not require the command to print output.
The default interval is 10000 ms. `--foreground` or `--yield-time-ms 0` waits for completion or cancellation instead; `--background` re-enables yielding.
The equivalent configuration is `[terminal] foreground` and `yield_time_ms`, or `CAI_FOREGROUND` and `CAI_YIELD_TIME_MS`.

In the terminal, `/foreground` (`/fg`) selects foreground waits, `/background` (`/bg`) enables yielding, and `/yield_time_ms [MS]` inspects or sets the interval from 0 through 30000 ms.
If yielding was disabled, `/background` restores the libcai default interval; otherwise it keeps the selected interval.
Changes apply to subsequent tool waits, including child runtimes; they do not restart the process or change a wait already underway.
See [command boundaries](interactive-command-audit.md) for delivery policy and `cai/tools/terminal.h` for SDK ownership and limits.

## Interactive output

Interactive tool activity uses gray/green/red `*` receipts for neutral/success/failure, with normal-weight H3 activity verbs and durations, bold H2 command/path details, and separate MCP and Chrome receipts.
The heading color is independent of the green/red success/failure star.
Non-command result bodies use the themed Markdown feed.
Tool failures are wrapped literal blockquotes; known tools have concise receipts, MCP text content is rendered as Markdown, and unfamiliar JSON-looking results use code blocks.
These presentation rules apply to replay too; model-visible results and journal payloads are unchanged.
Command runners alone use raw terminal previews.
Commands show their first five logical output lines by default.
`/output` shows the current setting; `/output compact|full` changes it, and `--tool-output compact|full` selects the startup setting.
Commands capture their mode when they start: active commands and already displayed output stay unchanged.
Generic result bodies, tool failures, and MCP content share a compact budget of five source lines and five rows' worth of bytes at the terminal width.
Multiple MCP content blocks share one budget; a long single-line JSON value cannot bypass it.
Clipped previews include an omission notice.
Full mode retains supplied command output, result bodies, and all patch context.
Changing this setting affects non-command results when they are displayed.
Complete model-visible and journal payloads are preserved and remain available in JSONL exports.
Any tool-level output limit is still reported explicitly.
Command previews preserve recognized ANSI SGR colors/styles and discard other terminal controls, including cursor movement and title/clipboard strings.
`NO_COLOR` and plain output strip command styling too.
Styles are isolated per command and reset before returning to the host, including during streaming.
CRLF becomes a newline; standalone carriage returns separate progress lines.
Split control sequences and UTF-8 are handled incrementally; no terminal screen emulation or whole-output buffering is used.
Malformed UTF-8, non-control binary bytes, and control bytes in other tool labels/results/patches are escaped in the UI.
Terminal tool results and JSON logs preserve valid UTF-8 and replace malformed bytes with `?`; their content is unaffected by preview filtering.

The native `apply_patch` matcher follows Codex's ordered line matching, including repeated context, whitespace fallback and Unicode punctuation normalization.
It preserves unchanged context and validates the full plan before publication.
Failed patch diagnostics go through libmdf as quoted prose with literal expected/actual values; source spacing and raw journal/log data are preserved.
See [patch tool behavior](patch-tool.md) for the matching and safety contract.

Successful `apply_patch` calls display the **agent's submitted changes**, with one neighboring unchanged line from the current edit's snapshot when the patch omits context.
No VCS comparison is used.
A gutter shows actual file line numbers: removed rows use the original file, while added and context rows use the result.
Matched hunk starts come from the native patch application, including adjustments for earlier hunks.
Added rows have a green background and removed rows a red background across the entire terminal row, including the gutter and aligned wrapped continuations.
Context rows have no background.
Tabs expand to spaces; UTF-8 glyphs wrap by terminal cell width.
Terminals narrower than 32 columns use the unnumbered format.
Compact mode keeps one context row on either side of changes and uses `⋮` between separated change blocks; outer context is trimmed without an ellipsis.
Full mode retains all supplied context.
`NO_COLOR` disables these colors.
Non-interactive invocations log the full patch preview and tool activity as pslog records on stderr, regardless of the preview setting; stdout still contains only the final reply.

Rejected update context identifies the file and hunk, with bounded escaped expected/actual line excerpts when a starting-line candidate exists.
Internal spacing must match; read the current file before regenerating a rejected hunk.
These preflight failures leave files unchanged.
The interactive feed shows the tool failure once; attributed warning records remain in the JSON session log.

A `Delete File` followed by `Add File` for the same path replaces that file atomically, including when other file sections separate the two.
It preserves the original file permissions and participates in whole-patch rollback.
Other repeated operations on a path are rejected; combine update hunks into one `Update File` section or use separate calls.

## Display API for embedders

SDK hosts can use `<cai/display.h>` without softline or libmdf.
Create a `cai_tool_display` with a borrowed sink and zero-initialized `cai_tool_display_config` (compact, five preview lines, no colors), feed it owner-thread runtime events with `cai_tool_display_event`, then close it with `cai_tool_display_close`.
`cai_tool_display_set_mode` affects future calls.
`cai_tool_display_set_color` (Lua `display:set_color`) changes future styling between events without losing active command identity, preview limits or split UTF-8/control decoding state.
`cai_tool_display_set_palette(display, activity_foreground, label_foreground, &error)` (Lua `display:set_palette(activity_foreground, label_foreground)`) copies two bounded foreground-only SGRs; empty strings select terminal-default foreground.
It preserves active calls and rejects attributes, backgrounds and non-color controls.
Standalone displays default to cyan activity and bright cyan labels. libcai_app obtains H3/H2 from libmdf's public palette for the effective theme, including live changes and parent/reviewer offsets.
It keeps status symbols gray/green/red and patch diff colors independent of that palette.
Hosts with structured tool presenters can use `cai_tool_display_summary` instead of the generic completion event, then feed details to their Markdown renderer.
It emits a colored success/failure marker with escaped plain-text labels and releases the originating display call.
Lua exposes `display:summary(event, verb, label, success)` with the same behavior.
CAI's interactive presenter uses this for reasoning requests, messaging, kanban, local search and reverse geocoding.
It renders details through libmdf; the original tool results remain in model context, structured logs and exports.

For streaming saved results, `cai_tool_display_replay_begin` (Lua `display:replay_begin(event, verb, label, success)`) emits that heading but retains the call's preview and control-decoding state.
Supply body chunks as `TERMINAL_OUTPUT` events, then an empty `TERMINAL_COMMAND_COMPLETED` event carrying the saved exit status to flush and release the call without another heading.
This renders saved data; it never executes commands.
`cai_tool_display_set_width(display, columns, &error)` enables the numbered, wrapped layout (32..4096 columns; zero restores unwrapped output).
Hosts set `LC_CTYPE` to their UTF-8 locale for glyph widths.
Width changes apply to the next logical patch row, including within an active call.
Successful native patch calls emit `CAI_AGENT_EVENT_TOOL_PATCH_LOCATIONS` first: bounded chunks of ASCII `old new\n` rows, one per update hunk in submitted order, then an empty chunk.
These are one-based matched original/result file positions.
The positions include any added leading preview context.
Added files start at one.
Next come bounded `CAI_AGENT_EVENT_TOOL_PATCH` chunks containing submitted changes and neighboring preview context, followed by an empty chunk.
Rejected patches emit neither locations nor patch bytes.

`CAI_AGENT_EVENT_REASONING_SUMMARY_STARTED` precedes each provider summary section, so a status renderer can replace its heading instead of concatenating unrelated titles.
Lua exposes the same event constant.

`cai_patch_render(source, &config, &error)` also renders a patch directly from a source to a sink, with bounded line buffers and no rewind or full-body materialization.
`cai_patch_render_width(source, &config, columns, &error)` also accepts explicit columns.
Direct sources have no matched file metadata: additions start at one; updates may specify `@@ -OLD +NEW @@`, otherwise the gutter stays blank.
No file is opened to guess missing positions.
Changed rows flow in chunks; exceptionally long compact context rows are visibly clipped.
Sources and sinks remain caller-owned.

Lua exposes the same renderer:

```lua
local display = assert(cai.tool_display({mode = "compact", color = true, width = 80},
  function(chunk) io.write(chunk); return true end))
-- Forward each runtime event from its owner-thread callback:
-- assert(display:event(event))
-- At safe feed boundaries, reuse your UI timer for delayed Running headings:
-- assert(display:tick())
-- display:needs_tick() also keeps that timer active after a command yields.
assert(display:set_mode("full")) -- future calls only
assert(display:set_width(100)) -- next logical patch row
assert(display:close())
-- Direct rendering accepts a patch string or a source callback returning chunks.
local patch = "*** Begin Patch\n*** Add File: example.txt\n+hello\n*** End Patch\n"
assert(cai.patch_render(patch, {mode = "full", width = 80},
  function(chunk) io.write(chunk); return true end))
```

## Additional capabilities

- [Dynamic process and Lua tools](plugins.md)
- [MCP and connected apps](mcp.md)
- [Skills](skills.md)
- [Chrome](chrome.md)
- [Kanban](kanban.md)
- [Session messaging](agent-messaging.md)
