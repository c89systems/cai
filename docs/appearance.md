# Themes, status fields and timestamps

## Themes

Theme selection belongs to `libcai_app`, including its C and Lua embedders.
`cai --list-themes` lists softline themes, their default libmdf pairings, and all available libmdf themes without opening a session.
`--theme` / `-t` accepts an alias or an explicit `SOFTLINE:LIBMDF` pair:

| Alias | Pair |
| --- | --- |
| `default` | `default:default` |
| `synthwave` | `synthwave:synthwave-84` |
| `plain` | `plain:ascii` |
| `accent` | `accent:night-owl` |
| `dracula` | `dracula:dracula` |
| `gruvbox` | `gruvbox:gruvbox` |
| `monochrome` | `monochrome:horizon` |
| `monogreen` | `monogreen:everforest` |
| `outrun` | `outrun:outrun-electric` |
| `riced` | `riced:catppuccin-mocha` |

For example, `cai -t synthwave` selects its pairing; `cai -t riced:iosvkem` mixes the two libraries' themes.
Names are case-insensitive.
The startup default is `default:default`.
Set `CAI_THEME` or `[ui] theme = "synthwave"` in `config.ini`; argv overrides both.
Embedders use the same `ui.theme` setting, and their configured environment prefix replaces `CAI_`.
In the TUI, `/theme` shows the current pair, all softline pairings, and all libmdf themes.
`/theme synthwave` or `/theme riced:iosvkem` changes future output at the next safe response/tool boundary with either submission key.
The selection survives `/new` and `/resume` within the app invocation; it does not rewrite `config.ini` or session history.
`ascii` is a libmdf output mode, not a theme: it disables Markdown ANSI styling while retaining UTF-8 text.
`NO_COLOR` selects the plain prompt and disables Markdown styling regardless of the selected theme.
The gray reasoning message remains plain text styled by softline, and pslog retains its separate `LOG_` configuration.
Host-supplied raw banners are printed verbatim.

Review calls print a start receipt before preparation, then a completion or failure receipt.
Both model-invoked reviews and `/review` show standalone **Review started** and **Review finished** Markdown titles, including when colors are disabled.
Review output uses the next libmdf theme by default and restores the parent theme when it ends.
These offsets affect Markdown and tool headings; Softline's theme stays unchanged:

| Context | INI setting | Flag and TUI command | Environment | Default |
| --- | --- | --- | --- | --- |
| Parent | `[ui] theme_offset` | `--theme-offset N`, `/theme-offset [N]` | `CAI_THEME_OFFSET` | 0 |
| Ordinary child | `[subagent] theme_offset` | `--subagent-theme-offset N`, `/subagent-theme-offset [N]` | `CAI_SUBAGENT_THEME_OFFSET` | 0 |
| Reviewer | `[review] theme_offset` | `--review-theme-offset N`, `/review-theme-offset [N]` | `CAI_REVIEW_THEME_OFFSET` | 1 |

Child offsets are relative to the effective parent theme.
Signed offsets wrap through libmdf's listed themes, excluding `boring`; `-300` and `1234` are valid.
Zero keeps the selected theme.
`/theme` also shows offsets and the active libmdf theme.
Offset changes stay editable in Softline until the next safe response/tool boundary.
Softline's theme, ASCII output and `NO_COLOR` are preserved, including during replay.
Settings and slash-command changes apply to this invocation without rewriting history or configuration.

## Status fields

Configure the status order with `--status-fields FIELDS`, `CAI_STATUS_FIELDS`, or `[ui] status_fields` in `config.ini` (also available through libcai_app's C/Lua settings).
The default is `model:ctx:dir:branch:weekly:crd:yolo:goal`.
The Git branch appears after the directory and is omitted when unavailable, including outside Git repositories or with detached HEAD.
Discovery reads Git metadata in-process, including ancestor repositories and linked worktrees; it does not launch Git.
Labels are literal, safely truncated UTF-8 with terminal and direction/format controls rejected or escaped.
Discovery runs at startup, then at tool/turn or user-shell boundaries with a 60-second debounce.
There is no idle polling; explicit `/status` refreshes immediately.
`default` expands that list: `--status-fields provider:default` prepends the provider.
Aliases are `context` for `ctx`, `directory`/`folder`/`workspace` for `dir`, `w` for `weekly`, and `credits` for `crd`.
Names are lowercase; duplicates keep their first position.
Unavailable values are omitted.
`weekly` includes any short quota window, or the API cost estimate for API-key providers.
`YOLO` appears only while the effective mode is enabled, after credits in the default order.

The softline prompt is `> `.
Its status bar shows model and reasoning effort, the last measured context percentage, a shortened workspace path, available ChatGPT subscription quota remaining and credits, `YOLO` when enabled, and goal status with elapsed active time, such as `goal=active 45m` or `goal=active 10d10h45m`, without the objective.
Completed goals show `goal=achieved` with their final elapsed active time until replaced or cleared; `/goal` shows the objective and accounting.
Quota appears as `w 72%` when only a weekly window is available, or `w 72% 1h 48%` when the backend also reports an hourly window.
Windows are identified by their returned duration.
Available credits follow the quota as `crd 43` or `crd unlimited`, rounded to whole numbers; `/status` retains fractional balances.
Missing credit values are hidden.
`libcai` exposes windows and available credit balance through `cai_client_chatgpt_quota` in `<cai/quota.h>`.
The status bar requests quota in the background after session startup and after completed turns, at most once every five minutes automatically.
Quota retrieval does not block prompt readiness, and idle sessions make no periodic quota requests.
Quota requests have a 10-second default deadline.
An explicitly configured `[client] timeout_ms` overrides it, independently of the five-minute debounce.

## Timestamps

Interactive activity headings, command receipts, diagnostics and Markdown blocks use gray DTGs without additional dimming.
Slash-command replies share the input's timestamp.
Tool-result bodies share their receipt's timestamp, which uses the recorded call-start time when available.
Assistant and reasoning blocks keep their own timestamps.
Same month and year: `DDHHMM` in local time, or `DDHHMMZ` with `--utc`.
Older entries use `DDHHMMZONEMMMYY`, for example `021230ANOV26`.
Months are JAN through DEC; years are always two digits.
Local zones follow the [USNO/HMNAO chart](https://aa.usno.navy.mil/downloads/TimeZoneMap_2021.pdf), using ASCII letters and star codes (`E*` for +05:30, `M*` for +13:00).
Offsets requiring Unicode chart symbols, or having no listed code, use `J` (local time); exact UTC offsets remain in event metadata.
Time is never rounded.
`CAI_UTC=true`, `[ui] utc=true`, or the application `ui.utc` setting selects Zulu time; `--no-utc` restores local display.
Batch stdout stays unchanged.
Replay uses original event times; entries without recorded times stay unstamped.
Submitted prompts use libmdf's `write_literal` method inside blockquote tokens; Softline receives the rendered bytes and owns the editor and queue.
The bounded adapter filters terminal controls and repairs UTF-8 without escaping Markdown; thinking blockquotes keep normal Markdown interpretation.
Word decisions may span input chunks, and transport boundaries never force output.
Prompt stamps beside `>` still need presentation support in that libmdf feed path.
Presentation text never enters drafts, prompt history, or model context.
The sticky status DTG currently shares Softline's status-message style; independent metadata styling there needs a Softline status metadata extension.

## Rendering and tool receipts

Live command completions use the same elapsed-time heading, with seconds, minutes or hours as needed.
Activity and duration use the effective theme's H3 foreground at normal weight; the command uses H2 foreground in bold.
Fast commands may omit the live completion row, but still have a saved completion and output on replay.

See [tool execution and display](tools.md) for output limits and [performance measurements](performance-cbor-presentation.md) for renderer attribution.
