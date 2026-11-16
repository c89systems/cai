# Configuration and storage locations

## Configuration sources

The local `${XDG_CONFIG_HOME:-$HOME/.config}/cai/config.ini` is bootstrap configuration and the initial seed for the stored `default` agent. Normal startup does not create or rewrite this file. Without it, existing XDG storage defaults apply.
Use `--dump-config FILE` to explicitly generate a reference template: each supported setting includes its description and commented default.
Explanatory comments start with `##`; setting assignments start with `#` so removing one marker selects the value explicitly.
Automatic global instruction, skills and export paths are shown as resolved at template creation.
Leaving them commented retains automatic lookup on subsequent starts; an empty value has the field-specific discovery, inheritance or unset meaning documented beside that setting.
Existing files are never rewritten.
`-c FILE`/`--config FILE` selects another file; `--no-config` skips disk bootstrap/seed loading while retaining stored-agent resolution.
`--config-dir DIR` also selects `DIR/config.ini`.
Agent startup loads `<namespace-prefix>.agents/agent.NAME` through liblockdc. New sessions use `default`; resuming restores the last selected agent, and `-u NAME` / `--use NAME` selects explicitly. Metadata identifies the agent; its `config.ini` attachment holds agent behavior. If `default` is absent, CAI initializes it once from the valid seed configuration without triggering provider login. Missing named agents are errors. Concurrent initializers publish one complete definition. Later disk edits do not replace stored agent behavior.

Bootstrap fields stay local: named store definitions and selections, storage namespaces/compression, application paths needed before opening storage, workspace-server settings, temporary-run selection, and runtime session identity/ownership settings. A stored attachment containing these fields is rejected. Agent and session stores default to `default` and share one client when they identify the same backend. Selecting a different agent store leaves session history in its configured session store.

An explicit `-s` attachment uses the running backend's agent and opens no local agent or session store. Attachment adapters project only bootstrap settings from disk; agent seed values cannot become requirements on an existing owner. Explicit invocation/embedding overrides still participate in compatibility checks. Temporary runs keep using the persistent agent repository while placing their journals in the private temporary session store.

Explicit CLI and embedding settings override stored agent behavior without being persisted into the initial definition. Only declared INI values seed the attachment; automatic optional paths are rediscovered at startup. Explicit seed paths are resolved against their original file before publication. Application-prefixed environment settings select bootstrap fields only; they no longer override stored model, reasoning, themes, tools or other agent behavior. Explicit API-key environment lookup and the stored `[env]` block remain available. `LOG_*` stays independent; local `[env] LOG_*` entries configure logging without becoming agent behavior.

The bootstrap file must remain valid INI. Its non-bootstrap values seed `default` once. Publication, editing, builder mode and live activation are described below and in [stored agents](stored-agents.md).
Global instruction files are loaded again when a session runtime opens, including on replay; saved session state does not replace those files.
Resuming restores the saved model and reasoning settings unless explicitly overridden by startup flags.
Paths accept `~/` and are relative to the configuration file.
The `[env]` section accepts literal environment assignments.
Existing process variables, including empty ones, win unless the global `config_env_has_precedence = true` setting is uncommented.
Environment values are applied before authentication, logging and worker startup; templates do not copy credentials or environment assignments.
Path defaults may reflect the current XDG roots, HOME or explicit `--config-dir`.

For agent behavior, precedence is explicit command line/embedding settings, stored configuration, then built-in defaults. For bootstrap fields, explicit command line overrides the bootstrap environment and INI configuration.
Saved model/reasoning settings are restored on resume unless explicitly overridden.
Use the generated template for the complete current setting descriptions; this guide does not duplicate that catalog.
Base prompt files, offline dumps and parent-only overrides are described in [developer instructions](runtime.md#base-developer-instructions).
The optional chart guide is included by default. Set `[agent] disable_chart_instructions = true` in the seed configuration or use `--no-chart-instructions` to omit it; `--chart-instructions` explicitly includes it. Charts remain on demand unless the user requests ongoing chart use.

```sh
cai --dump-config
cai --dump-config ./defaults.ini
cai -c ./config.ini
cai --no-config
```

`cai --dump-config [FILE]` writes a documented, commented defaults template without starting an agent.
Omit FILE to write to stdout.
An existing output file is overwritten; its parent directory must exist.
This utility does not load the selected config, copy secrets or explicit settings, create the normal config, or open storage.
Application identity/path defaults still apply, including `--home` and embedding path overrides.

## Stored-agent workflow

Create editable source files, change the name and settings, then publish:

```sh
cai --build agents/engineer
# Edit agents/engineer/config.ini and SYSTEM_PROMPT.md.
cai --make agents/engineer
cai --use engineer
cai --use engineer -R SESSION_ID
```

`--build [DIR]` defaults to cwd and creates missing output parents; the destination must be empty. Add `--use NAME` to start from another stored agent. It writes `config.ini`, `SYSTEM_PROMPT.md`, a coding-agent `EXAMPLE_SYSTEM_PROMPT.md`, and available bundled resources. Explicitly configured bootstrap instructions, tools and skills are copied into the source directory. The example removes model-specific self-identification and becomes active only if copied to `SYSTEM_PROMPT.md`.

`[agent] name` is editable. A nonempty value wins at publication; an empty or missing name falls back to the source directory basename. Canonical names are lowercase ASCII letters, digits, `_` and `-`; spaces become `_`, other characters are removed, and names must contain 1–64 usable characters. Metadata and the file inventory live in the lockd object, not in an `agent.json` file.

`--make [DIR]` validates configuration, instructions and bundled tool manifests, then publishes metadata and attachments under one object lease. Existing names require `--yes` / `--force`. Failed publication preserves the previous agent. Resources under `tools/`, `skills/` and `files/` are streamed; symlinks and special files are rejected. Tool validation prepares handlers without invoking them. Instructions must be NUL-free text of at most 1 MiB; whitespace-only `AGENTS.md` is omitted.

`SYSTEM_PROMPT.md` containing `{{runtime_default}}` anywhere uses the normal model-dependent base prompt. Otherwise its entire text becomes the literal base. Reviewer settings, explicit parent-only overrides and automatic tools/skills/MCP additions retain their ordinary composition rules.

API keys can be set directly in `[client] api_key` and are retained in encrypted lockd configuration. `--dump-agent NAME` writes `NAME/` below the selected directory; `--dump-agents [DIR]` writes one directory per agent (default root `agents/`). These exports and `--build` omit declared secret configuration fields and native credentials by default. `--include-credentials` explicitly includes them; native authentication is written as `AUTH.json`. Prompts, scripts and arbitrary environment assignments are ordinary bundled content, so inspect them before sharing an export.

`--make DIR --from-auth-json FILE` explicitly imports ChatGPT credentials without login. `--login --use NAME` logs into that agent directly. Credentials are consumed and refreshed in the selected agent object, even when another provider performs inference and ChatGPT supplies MCP apps. Replacing the definition without a new import preserves its current credentials. `--auth-json FILE` explicitly selects core file-backed authentication instead; external files are never discovered automatically.

Runtime `config.ini` streams from lockd into the parser and never touches the disk cache. Native credentials also stay out of the cache. Bundled resources use private revision-specific directories under `cache_dir/agents/NAME/REVISION`. Session IDs, journals and recall stay in the session store. `/status` identifies the selected definition. `/use NAME`, or explicit `--use NAME` when attaching, activates through the existing owner at a whole-turn boundary. Failed activation preserves the old agent. Unsupported provider/account context conversion is rejected.

`--edit-agent NAME` opens configuration and instruction files using `VISUAL`, then `EDITOR`, then `vi`, in a private temporary directory. Save validates and atomically updates the same name; cancel, failure or a conflicting definition revision preserves storage. The temporary tree, including editor backups, is removed on ordinary exit. Bundled resources use the source workflow instead. `--builder` and `/builder` select dedicated builder instructions, using the same `--make` publisher. `--dump-system-prompts [DIR]` writes the built-in model, reviewer and builder prompt catalog; its default directory is `prompts/`.

Historical exports use recorded activation instructions, including after definitions are edited or removed. The unreleased configuration-only registry format is unsupported; republish or recreate those definitions rather than relying on migration. Session history remains independent.

## One application root

`--home DIR` or `CAI_HOME=DIR` places the application's default directories under one root: `DIR/config`, `DIR/state`, `DIR/cache`, `DIR/data` and `DIR/run`.
The default config becomes `DIR/config/config.ini`; Pouch and its key live under `DIR/state`, logs and clipboard files under `DIR/cache`, exports under `DIR/data/exports`, and Chrome sockets under `DIR/run/chrome`.
Explicit individual paths always win over these derived defaults, even when the home is supplied on the command line.
For example:

```sh
cai --home ./agent-home
cai --home ./agent-home --cache-dir /fast/cai-cache
cai --home ./agent-home -c ./policies/cai.ini
```

`[paths] home_dir` selects the same root from an INI file.
CLI/environment/home embedding fields can select the root before config discovery; a home value read from INI affects derived paths without moving, rewriting or loading another config file.
`-c FILE` and `CAI_CONFIG` keep their explicit selection.
`--no-config` still honors `--home` while skipping disk bootstrap loading. It does not disable the stored-agent repository.
`--runtime-dir`, `CAI_RUNTIME_DIR` or `[paths] runtime_dir` overrides the runtime root.
Without a unified home, existing XDG defaults apply.
Relative CLI home paths are anchored to the workspace selected by `-C`; INI paths remain relative to their config file.
The process `HOME`, workspace, external Codex auth and explicit lockd endpoints are unchanged.
An embedding app named `smith` uses `SMITH_HOME` and the same directory layout, without an extra `smith` suffix.

## Named stores

Named stores separate agent definitions and native credentials, session history, and messaging backends:

```ini
[store.default]
## Empty endpoint derives state_dir/pouch; key_file defaults to state_dir/pouch.key.
endpoint = ""

[store.team]
endpoint = "pouch:///srv/agents/pouch"
key_file = "/srv/agents/pouch.key"

[storage]
sessions_store = default
agents_store = default
messaging_store = team
```

All role selections default to `default`.
The matching flags are `--agents-store`, `--sessions-store` and `--messaging-store`; environment aliases are `CAI_AGENTS_STORE`, `CAI_SESSIONS_STORE` and `CAI_MESSAGING_STORE`. Native authentication belongs to the selected agent, with no separate auth-store selector.
`--lockd` / `CAI_LOCKD` and `--pouch-key-file` / `CAI_POUCH_KEY_FILE` override the default store.
Each named pouch store requires its own explicit `key_file`; new roots generate it, while existing roots require their existing key.
HTTP(S) stores accept `client_pem` and do not use a local pouch key.
INI endpoint/key/TLS settings live under `store.NAME` (`endpoint`, `key_file`, `client_pem`), replacing `storage.lockd`, `paths.pouch_key_file`, and `paths.lockd_client_pem`.
Existing configuration files are never rewritten.

Store handles remain open for the app run, including turns, `/new`, `/resume`, messaging toggles, and nested runtime contexts.
Pouch roots are identified by their filesystem identity: aliases share one process client, references keep it alive, and the final owning context closes it.
Conflicting keys or endpoint options for the same open root are rejected.
Reconnection remains liblockdc's responsibility.
`--temp` substitutes only the session store; configured agent and messaging stores remain available, including native credentials and communication with normal runs.
Its kanban data stays with the private session store.

Session history requires a Pouch store.
Remote stores can still serve supported agent and messaging roles.
See [session journals](journal.md#pouch-storage-and-ownership) for encryption, compression, ownership and compatibility.

## Workspace approval

Agent startup requires workspace approval.
On first use, run `cai -y` (`--yes` or `--force`) to approve the current directory and its descendants for this and future sessions.
Approval is recorded in the session journal and in a small authorization record in the configured session store.
It survives session expiry/deletion; clearing the store removes it.
Imports and model output cannot grant approval.
Help, listing, export, login, and standalone message delivery do not require approval.

`-k` / `--insecure` bypasses this gate for one invocation and allows automatic local instruction loading without saving approval, even with `--yes`.
It also accepts `[agent] insecure = true` in the stored agent definition.
`--no-insecure` overrides those defaults.
This setting does not change TLS verification or the execution sandbox.
Approval follows canonical directory ancestry: siblings and symlinks resolving outside the approved tree do not inherit it.
Repository instructions above the authorized root are excluded.
Global instruction files keep their existing settings.
An old session does not implicitly approve its workspace.

## Execution policy

`--yolo` disables local tool confinement and command environment filtering; `--no-yolo` restores the default restricted mode.
Set `[agent] yolo = true` in the initial agent configuration for the same behavior.
Precedence is explicit CLI/embedding settings, stored agent configuration, then the built-in default `false`.
In the TUI, `/yolo` reports the current policy and `/yolo on` or `/yolo off` changes it once active turns, subagents and terminal commands finish.
Pending changes remain editable in Softline and appear in `/status`.
This does not write configuration or save the policy in session history.

YOLO commands inherit the host environment and ordinary user filesystem permissions; managed commands still force `PAGER`, `GIT_PAGER`, and `GH_PAGER` to `cat`.
Local read/list/patch/image tools may access paths outside the workspace, and reviewers/subagents inherit the selected mode.
Explicitly disabled tools remain disabled; output retention, timeout and cancellation mechanisms still apply.
The legacy synchronous exec preset bypasses Bubblewrap or sandbox-exec in YOLO mode.
Smith's managed command runner has no OS sandbox in either mode: restricted mode scopes file tools and working directories and filters command environments.
Startup warns about the effective policy and `/status` reports it.
`!COMMAND` and `/shell` use ordinary user access independently of the agent's policy.

## Temporary workspaces

`cai --temp -k` starts a new session in a unique `cache_dir/temp/chat-XXXXXX/workspace` directory.
Its sibling `private/` contains the encrypted session pouch and key, prompt history, clipboard, exports, and app-owned logs.
Interactive runs use the common owner/socket frontend, with a separate private `t-XXXXXX` discovery directory under `runtime_dir`; that short-lived directory and its socket/locks are also removed.
Temporary owners never persist after their frontend quits, including with `--persist`.
The normal global or explicit `--config` file is loaded unchanged; path isolation is applied only in memory.
Relative CLI input paths (instructions, skills, model metadata, authentication, client PEM, and Chrome socket) resolve against the original workspace selected by `-C` before entering the temporary directory.
INI paths remain relative to their config file.
On normal teardown, workers and softline close first, and private history/state is deleted.
The plain terminal question `Remove temporary workspace …? [y/N]` controls whether the workspace files are also deleted.
No, empty input, EOF, interruption, host STOP, and non-interactive runs preserve the workspace and report its location.
Use `--temp -y` instead to approve the fresh workspace and delete it without asking.
`--yes` / `--force` are equivalent and delete it without asking, including on failure.
Crashes can leave the cache directory behind.
`-T` still selects review output type.
`[ui] temporary` / `CAI_TEMP` also select this mode; `--no-temp` overrides them.
A supplied logger or explicit `LOG_OUTPUT` keeps its own sink.

## Logs

In prompt mode, logs append to `$XDG_CACHE_HOME/cai/<sessionid>.log`, falling back to `~/.cache/cai/<sessionid>.log`.
Files are private (0600), with a private cai cache directory (0700).
Startup records are transferred to the selected session's log; failed startup records remain in a `startup-*` file.
The UI surfaces readable warning and error notices, including their explanations and available request context (HTTP status, endpoint, provider code, and request ID).
Full pslog records remain in the file; truncated notices direct you to the session log.
Normal session rendering continues through softline and libmdf.

With `-n`, `--review`, or `--review-and-fix`, the default log sink is stderr.
Stdout contains only the latest top-level assistant response of the entire invocation, including when a later turn fails without producing a response.
Earlier replies, replay, reasoning, tools, and child reviews stay in the log.
Activity logging is immediate; final stdout selection uses a private, unlinked file-backed spool with bounded reads, then renders through libmdf.
Markdown rendering detects stdout: terminals allow ANSI styling, while pipes and files receive escape-free UTF-8 rendered at 80 columns.
Help, version, listing, export, and import keep their normal stdout results.
Utility commands (`--list`/`-l`, `--list-all`/`-L`, `--resume` without an ID, `--export`, `--import`, and `--login`) default to warning-level diagnostics on stderr.
Agent sessions and review modes retain all levels by default.
Login URLs, callback guidance, and successful login results are normal stdout output.
`LOG_LEVEL=info`, `debug`, or `trace` enables more utility diagnostics through pslog's normal environment overrides.

Color defaults to automatic terminal detection for stdout/stderr logs; redirected output and interactive log files stay uncolored.
Native pslog environment settings apply, including `LOG_MODE`, `LOG_LEVEL`, `LOG_NO_COLOR`, `LOG_FORCE_COLOR`, `LOG_PALETTE`, `LOG_VERBOSE_FIELDS`, `LOG_DISABLE_TIMESTAMP`, `LOG_TIME_FORMAT`, `LOG_UTC`, `LOG_OUTPUT`, and `LOG_OUTPUT_FILE_MODE`.
`LOG_OUTPUT` explicitly overrides the default sink; `default+PATH` adds a second file while retaining the default sink and its UI warning notices.
`LOG_LEVEL=warn` filters out ordinary session records.

See [troubleshooting](troubleshooting.md) for log inspection and export.

## C and Lua configuration receivers

The shared `<cai/config.h>` receiver provides incremental INI loading, schema validation, typed access, explicit environment/path handling, and `write_template`/`create_template_file`.
Apply explicit fields to initialized client, low-level agent, runtime and terminal configuration records; keep the receiver alive while its borrowed strings are used.
Hosts select their own schema with `cai_config_open_schema`, or use `cai_config_open` for the complete SDK schema plus extensions.
CAI selects only settings its application can honor, retaining its directory-scoped encrypted session storage policy.
Numeric settings use a dot decimal separator regardless of the application's locale.
Schema extensions retain native field types and constraints, and templates place globals before named sections.
An explicit `--config-dir` remains anchored to its original location when a configured workspace changes the working directory.
`section_count`/`section_at` enumerate distinct explicitly configured sections in first-setting order.
Schema sections ending in `.*` accept named instances; the SDK includes the `mcp.*` settings, which embedding hosts can enumerate to construct clients explicitly.
Applying runtime settings does not open transports.
Lua exposes `cai.config()` with `load(text_or_chunk_reader, origin)`, `load_file`, `sections()`, typed getters, `set`, `resolve_path`, `apply_environment`, `write_template(chunk_writer)` and `create_template_file`.
Lua configuration strings reject embedded NUL bytes instead of silently truncating them.
