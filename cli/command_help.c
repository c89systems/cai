#include "command_help.h"
#include "error.h"
#include <cai/cai.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const help[] = {
    "# Help\n\n"
    "Your coding agent for the current directory. Enter a prompt to "
    "start a turn. Local queries respond immediately. While busy, Enter "
    "queues prompts; Alt-Enter steers at a safe boundary. Model, "
    "reasoning, "
    "tier, goal and theme changes use the next safe boundary with either "
    "key. Alt-E edits pending input.\n\n"
    "## Slash commands\n\n"
    "| Command | Action |\n"
    "| --- | --- |\n"
    "| `/help` | Show this help inline. |\n"
    "| `/help -p`, `/help less` | Open rendered Markdown help in libmdf's "
    "pager. |\n",
    "| `/retention [DAYS or off]` | Show or set this conversation's "
    "inactivity retention. 0 or off keeps it forever. |\n"
    "| `/history [COUNT]` | Show or set prompt recall (default 200). "
    "Storage maintenance prunes oldest entries; zero disables recall "
    "and recording. |\n",
    "| `/yolo [on\\|off]` | Show or change the runtime's local execution "
    "policy; changes wait for idle tools and subagents. |\n",
    "| `/theme [NAME or SOFTLINE:LIBMDF]`, `/t` | Show the current pair "
    "and all "
    "themes, or change themes at the next safe response/tool boundary. "
    "|\n",
    "| `/theme-offset [N]` | Show/set the parent libmdf rotation (default "
    "0). |\n"
    "| `/subagent-theme-offset [N]` | Show/set ordinary child rotation "
    "from the parent (default 0). |\n"
    "| `/review-theme-offset [N]` | Show/set reviewer rotation from the "
    "parent (default 1). Signed offsets wrap, excluding boring. |\n",
    "| `/review [uncommitted\\|base REF\\|custom TEXT]` | Run an isolated "
    "reviewer; no argument compares against the Git default branch. |\n",
    "| `/review-and-fix [uncommitted\\|base REF]` | Replace the current "
    "goal with review, fix, and verification until clean; stay in this "
    "conversation. No argument uses the Git default branch. |\n",
    "| `/todo [COMMAND]` | Show the selected board; list, use NAME, "
    "new NAME, see ID, layout board/list, or help. Requires --todo. |\n",
    "| `/queue [on\\|off\\|NAME]` | Show the mailbox, enable/disable "
    "messaging, or replace its full name between turns. Applies to this "
    "app, including /new and /resume; leaves config.ini unchanged. |\n",
    "| `/autoqueue [on\\|off]` | Toggle automatic approval of incoming "
    "next-turn messages (default off). Alt-A reviews; Alt-Y approves; "
    "Alt-N rejects; Alt-E edits. |\n",
    "| `/trim [COUNT]` | List 20 recent whole entries, or remove COUNT "
    "newest entries; 0 removes nothing. |\n"
    "| `/trim list [LIMIT]` | List a different number of entries; "
    "compaction is indivisible. |\n"
    "| `/compact` | Compact conversation context between turns; Escape "
    "cancels. The command remains editable in the queue until ready. |\n"
    "| `/goal [COMMAND]` | Show a goal; new TEXT replaces it, pause/resume "
    "control pursuit, clear removes it, help explains commands. Prefer "
    "asking the agent to manage goals. |\n",
    "| `!COMMAND` | Run a user command without a PTY; retain its newest "
    "literal output. |\n"
    "| `/shell-context [on\\|off]` | Share future command results with the "
    "model, or keep them local. |\n"
    "| `/shell`, `!sh` | Open your interactive shell between turns; return "
    "to the agent when it exits. Terminal output stays local. |\n",
    "| `/shellshare`, `!sh+` | Open a shell and share sanitized output "
    "when it exits. A ring buffer retains only the newest text that fits "
    "the configured line, byte and replay limits; older text falls out. "
    "|\n",
    "| `/model [MODEL [EFFORT]]`, `/m` | Show models or change "
    "model/effort at "
    "the next safe "
    "model/tool boundary, keeping this conversation. |\n",
    "| `/reasoning [EFFORT]`, `/r` | Show model-specific reasoning choices "
    "or "
    "change effort at the next safe model/tool boundary. |\n",
    "| `/reasoning-summary [none\\|auto\\|concise\\|detailed]` | Show or "
    "change reasoning summaries at the next safe model/tool boundary. |\n",
    "| `/reasoning-tool [on\\|off\\|normal LEVEL\\|high LEVEL]` | "
    "Configure "
    "the optional model-controlled effort tool between turns. |\n",
    "| `/tier [list\\|NAME]` | Cycle service tiers, list choices, or "
    "select a tier at the next safe boundary. |\n"
    "| `/fast` | Toggle Fast/priority and explicit normal routing. |\n",
    "| `/foreground`, `/fg` | Wait for command completion or cancellation. "
    "|\n"
    "| `/background`, `/bg` | Enable yielding; if disabled, restore "
    "libcai's "
    "default interval (10000 ms). |\n"
    "| `/yield_time_ms [MS]` | Show or set the host interval, 0..30000 ms. "
    "Zero selects foreground mode. Changes affect subsequent tool waits. "
    "|\n",
    "| `/output [compact\\|full]` | Show or change tool previews, starting "
    "with the next tool call. Compact commands show five lines; generic "
    "and MCP result previews also have a five-line/width budget. Patches "
    "collapse unchanged context. |\n",
    "| `/status` | Show model, reasoning, context, and available cost or "
    "quota metrics and the tool summary. Refresh ChatGPT quota "
    "immediately. |\n"
    "| `/log` | Show the full path of this conversation's log file, when "
    "configured. Also shown in `/status`. |\n"
    "| `/tools` | List installed tools, MCP servers, discovery errors and "
    "tool descriptions. |\n",
    "| `/mcp [COMMAND]` | List connections, inspect NAME, enable/disable "
    "NAME or all, or refresh [NAME]. Updates apply at request boundaries; "
    "config.ini is unchanged. |\n",
    "| `/tools reload [PATH]` | Reload configured tool packages or one "
    "package. |\n"
    "| `/tools unload NAME` | Retire a plugin before the next request. |\n"
    "| `/skills` | List loaded skill names and descriptions. |\n"
    "| `/resume` | List conversations for the current directory. |\n",
    "| `/resume NUMBER` | Resume a listed conversation and replay prompts "
    "and assistant text. |\n"
    "| `/new` | Start a fresh conversation in this directory. |\n"
    "| `/export [BASENAME]` | Export this conversation. `.md` or `.jsonl` "
    "selects one format; other names get both suffixes. No name uses the "
    "default export directory. |\n"
    "| `/quit`, `/exit` | Exit the application. |\n\n",
    "## Completions\n\n"
    "| Prefix/key | Action |\n| --- | --- |\n"
    "| `/` | Complete slash commands and aliases at draft start. |\n"
    "| `@` | Complete workspace file paths; Tab into a directory for its "
    "children. Hidden files except .git are included. |\n"
    "| `$` | Complete names from the loaded skills catalog. |\n",
    "| Tab | Accept a suggestion or cycle matches before ordinary queue "
    "key handling. Enter submits actual text, never the ghost. |\n"
    "| Prefix removal | Softline removes @ and $ when completion ends or "
    "the draft is submitted; command slashes remain. |\n\n",
    "## Queue and approval keys\n\n"
    "| Key | Action |\n"
    "| --- | --- |\n"
    "| Enter | Send when idle; queue the next turn while busy. |\n"
    "| Alt-Enter | Steer while busy; promote the newest queued input. |\n"
    "| Alt-E | Edit the newest queued input. |\n",
    "| Alt-A | Focus an incoming message's approval panel at an empty "
    "ordinary prompt. |\n"
    "| Alt-Y / Alt-N | Approve/reject the focused message; Alt-E edits it. "
    "|\n"
    "| Escape / Ctrl-C | Cancel the active turn and pause an active goal. "
    "|\n\n",
    "## Editing, cut/yank and paste\n\n"
    "| Key | Action |\n| --- | --- |\n"
    "| Ctrl-J | Insert a newline without sending. |\n"
    "| Home / Ctrl-A | Move to the current line's beginning. |\n"
    "| End / Ctrl-E | Move to the current line's end. |\n"
    "| Alt-B / Alt-F | Move backward/forward by Unicode word. |\n",
    "| Ctrl-U / Ctrl-K | Cut from the cursor to line start/end; at an "
    "edge, remove the adjacent newline. |\n"
    "| Ctrl-W | Cut the previous whitespace-delimited word. |\n"
    "| Alt-Backspace / Alt-D | Cut the previous/next Unicode word. |\n"
    "| Alt-K / Alt-J | Stash the whole draft and clear it / append the "
    "saved draft after all current text. |\n"
    "| Alt-N | Clear the draft or discard the queued/approval entry "
    "being edited; a focused approval is rejected instead. |\n"
    "| Ctrl-Y | Yank (paste) the most recent cut. |\n",
    "| Alt-Y | Cycle the last yank through earlier cuts, unless approval "
    "is focused. |\n"
    "| `Ctrl-_` or Ctrl-X then Ctrl-U | Undo a draft edit. |\n"
    "| Ctrl-T | Swap adjacent characters. |\n"
    "| Delete / Ctrl-D | Delete forward inside a draft. |\n",
    "| Terminal paste | Use your terminal's paste shortcut. Bracketed "
    "paste keeps multiline text in the draft without sending it. |\n"
    "| Ctrl-V | Paste a PNG/JPEG from the X11 clipboard as a cached image "
    "path. |\n\n"
    "Cuts live in Softline's ten-entry kill ring for this run, across "
    "prompts. Yanking uses this ring, not the desktop clipboard.\n\n",
    "The separate, reusable draft stash lasts for this run, across "
    "prompts and shell handoff. Stashing an empty draft keeps it. Undo "
    "after discard recovers text only, without queue identity or approval. "
    "Submitting an empty queued edit deletes that entry; Escape cancels "
    "the edit and restores the original entry.\n\n",
    "## History and terminal keys\n\n"
    "| Key | Action |\n| --- | --- |\n"
    "| Up / Down | Navigate draft rows or conversation prompt history. |\n"
    "| Ctrl-P / Ctrl-N | Recall previous/next history entry directly. |\n"
    "| Ctrl-R | Search history; Enter selects for editing, Enter again "
    "sends. |\n"
    "| Ctrl-G | Cancel history search and restore the original draft. |\n",
    "| Alt-S | Open a local shell; retain your draft, "
    "cursor and undo state when it exits. |\n"
    "| Ctrl-L | Refresh terminal layout while retaining the draft. |\n"
    "| Alt-Q | Save the draft to prompt history and quit without sending "
    "it. Up recalls it after restarting. |\n"
    "| Ctrl-D (empty prompt) | Exit. |\n\n",
    "## Startup options\n\n"};
struct cai_cli_help_reader {
  size_t index, offset;
  const char *tail[2];
  char example[4096];
};
int cai_cli_help_reader_open(const char *invocation, int attached,
                             cai_cli_help_reader **out, cai_error *error) {
  cai_cli_help_reader *reader;
  if (out == NULL)
    return cai_cli_error(error, CAI_ERR_INVALID, "help reader output required");
  *out = NULL;
  reader = calloc(1U, sizeof(*reader));
  if (reader == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate help reader");
  if (invocation == NULL || invocation[0] == '\0')
    invocation = "cai";
  snprintf(reader->example, sizeof(reader->example),
           "```sh\n%s %-31s # new conversation\n%s %-31s # send a prompt at "
           "startup\n"
           "%s %-31s # run non-interactively\n%s %-31s # review against "
           "default branch\n```\n\n"
           "Run `%s --help` for all startup options; see the application's "
           "manual.\n\n"
           "Dim timestamps are date-time groups: `DDHHMM` (day, 24-hour "
           "hour, minute) in local time. Older months/years add a zone, "
           "month and two-digit year, e.g. `021230ANOV26`. Use `--utc` "
           "for Zulu time (`Z`).\n",
           invocation, "-N -C ./project", invocation,
           "-i 'Explain this project'", invocation,
           "-Nni 'Fix the failing tests'", invocation, "--review", invocation);
  if (attached) {
    reader->tail[0] =
        "\n## Workspace connections\n\n"
        "| Command | Action |\n| --- | --- |\n"
        "| `/workspace [SELECTOR]` | List verified owners or select by name, "
        "ID or path relative to this workspace. Visited attachments, drafts "
        "and queues remain separate until terminal quit. |\n"
        "| `/takeover` | Explicitly acquire control of this workspace. |\n"
        "| `/shutdown` | Stop this owner; requires control. |\n\n"
        "Observers can switch workspaces or detach. Quit detaches all visited "
        "owners; nonpersistent owners stop after their last attachment "
        "leaves.\n\n";
    reader->tail[1] =
        "Alt-S opens a local shell while retaining the editor draft and undo "
        "state, including while the owner generates a response. `/shell` "
        "and `!sh` use the ordinary turn queue. To escape a local program, "
        "press Enter then `~.`. `/log -p` (or `/log less`) opens `less`, "
        "with libmdf's pure-text log viewer as the fallback. "
        "`--prefer-native-pager` uses only libmdf for logs. Help paging "
        "always uses libmdf's Markdown viewer. Owner work "
        "continues during either viewer; return displays the missed suffix.\n";
  }
  *out = reader;
  return CAI_OK;
}
static const char *help_paragraph(cai_cli_help_reader *reader, size_t index) {
  const size_t count = sizeof(help) / sizeof(help[0]);
  return index < count        ? help[index]
         : index == count     ? reader->example
         : index < count + 3U ? reader->tail[index - count - 1U]
                              : NULL;
}
size_t cai_cli_help_reader_read(void *context, char *out, size_t capacity,
                                int *error) {
  cai_cli_help_reader *reader = context;
  size_t used = 0U, count = sizeof(help) / sizeof(help[0]);
  if (error != NULL)
    *error = 0;
  if (reader == NULL || (out == NULL && capacity != 0U)) {
    if (error != NULL)
      *error = 1;
    return 0U;
  }
  while (used < capacity && reader->index < count + 3U) {
    const char *text = help_paragraph(reader, reader->index);
    size_t n;
    if (text == NULL)
      break;
    n = strlen(text) - reader->offset;
    if (n > capacity - used)
      n = capacity - used;
    memcpy(out + used, text + reader->offset, n);
    used += n;
    reader->offset += n;
    if (text[reader->offset] == '\0') {
      ++reader->index;
      reader->offset = 0U;
    }
  }
  return used;
}
void cai_cli_help_reader_close(cai_cli_help_reader *reader) { free(reader); }
int cai_cli_command_help(const char *invocation, int attached,
                         int (*write)(void *, const char *, size_t),
                         void *context) {
  cai_cli_help_reader *reader = NULL;
  size_t index;
  const char *text;
  int rc = cai_cli_help_reader_open(invocation, attached, &reader, NULL);
  if (rc != CAI_OK)
    return rc;
  for (index = 0U; (text = help_paragraph(reader, index)) != NULL; ++index)
    if (write(context, text, strlen(text)) != 0) {
      rc = CAI_ERR_TRANSPORT;
      break;
    }
  cai_cli_help_reader_close(reader);
  return rc;
}
