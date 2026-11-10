# Configuration and storage locations

## Configuration sources

CAI creates `${XDG_CONFIG_HOME:-$HOME/.config}/cai/config.ini` on first startup (help/version do not create it).
The private file is the configuration reference: each supported setting includes its description and commented default.
Explanatory comments start with `##`; setting assignments start with `#` so removing one marker selects the value explicitly.
Automatic global instruction, skills and export paths are shown as resolved at template creation.
Leaving them commented retains automatic lookup on subsequent starts; an empty value has the field-specific discovery, inheritance or unset meaning documented beside that setting.
Existing files are never rewritten.
`-c FILE`/`--config FILE` selects another file; `--no-config` skips creation and loading.
`--config-dir DIR` also selects `DIR/config.ini`.
Startup configuration is read from these files and CLI/environment inputs, not from pouch storage.
Restart CAI after editing the configuration.
Global instruction files are loaded again when a session runtime opens, including on replay; saved session state does not replace those files.
Resuming restores the saved model and reasoning settings unless explicitly overridden by startup flags.
Paths accept `~/` and are relative to the configuration file.
The `[env]` section accepts literal environment assignments.
Existing process variables, including empty ones, win unless the global `config_env_has_precedence = true` setting is uncommented.
Environment values are applied before authentication, logging and worker startup; templates do not copy credentials or environment assignments.
Path defaults may reflect the current XDG roots, HOME or explicit `--config-dir`.

Configuration precedence is explicit command line, environment, INI, then built-in defaults.
Saved model/reasoning settings are restored on resume unless explicitly overridden.
Use the generated template for the complete current setting descriptions; this guide does not duplicate that catalog.
Base prompt files, offline dumps and parent-only overrides are described in [developer instructions](runtime.md#base-developer-instructions).

```sh
cai --dump-config
cai --dump-config ./defaults.ini
cai -c ./config.ini
cai --no-config
```

`cai --dump-config [FILE]` writes the same documented, commented defaults template generated at startup.
Omit FILE to write to stdout.
An existing output file is overwritten; its parent directory must exist.
This utility does not load the selected config, copy secrets or explicit settings, create the normal config, or open storage.
Application identity/path defaults still apply, including `--home` and embedding path overrides.

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
`--no-config` still honors `--home` while skipping config creation/loading.
`--runtime-dir`, `CAI_RUNTIME_DIR` or `[paths] runtime_dir` overrides the runtime root.
Without a unified home, existing XDG defaults apply.
Relative CLI home paths are anchored to the workspace selected by `-C`; INI paths remain relative to their config file.
The process `HOME`, workspace, external Codex auth and explicit lockd endpoints are unchanged.
An embedding app named `smith` uses `SMITH_HOME` and the same directory layout, without an extra `smith` suffix.

## Named stores

Named stores separate session, authentication, and messaging backends:

```ini
[store.default]
## Empty endpoint derives state_dir/pouch; key_file defaults to state_dir/pouch.key.
endpoint = ""

[store.team]
endpoint = "pouch:///srv/agents/pouch"
key_file = "/srv/agents/pouch.key"

[storage]
sessions_store = default
auth_store = default
messaging_store = team
```

All role selections default to `default`.
The matching flags are `--sessions-store`, `--auth-store`, and `--messaging-store`; environment aliases are `CAI_SESSIONS_STORE`, `CAI_AUTH_STORE`, and `CAI_MESSAGING_STORE`.
`--lockd` / `CAI_LOCKD` and `--pouch-key-file` / `CAI_POUCH_KEY_FILE` override the default store.
Each named pouch store requires its own explicit `key_file`; new roots generate it, while existing roots require their existing key.
HTTP(S) stores accept `client_pem` and do not use a local pouch key.
INI endpoint/key/TLS settings live under `store.NAME` (`endpoint`, `key_file`, `client_pem`), replacing `storage.lockd`, `paths.pouch_key_file`, and `paths.lockd_client_pem`.
Existing configuration files are never rewritten.

Store handles remain open for the app run, including turns, `/new`, `/resume`, messaging toggles, and nested runtime contexts.
Pouch roots are identified by their filesystem identity: aliases share one process client, references keep it alive, and the final owning context closes it.
Conflicting keys or endpoint options for the same open root are rejected.
Reconnection remains liblockdc's responsibility.
`--temp` substitutes only the session store; configured auth and messaging stores remain available, including communication with normal runs.
Its kanban data stays with the private session store.

Session history requires a Pouch store.
Remote stores can still serve supported authentication and messaging roles.
See [session journals](journal.md#pouch-storage-and-ownership) for encryption, compression, ownership and compatibility.

## Workspace approval

Agent startup requires workspace approval.
On first use, run `cai -y` (`--yes` or `--force`) to approve the current directory and its descendants for this and future sessions.
Approval is recorded in the session journal and in a small authorization record in the configured session store.
It survives session expiry/deletion; clearing the store removes it.
Imports and model output cannot grant approval.
Help, listing, export, login, and standalone message delivery do not require approval.

`-k` / `--insecure` bypasses this gate for one invocation and allows automatic local instruction loading without saving approval, even with `--yes`.
It also accepts `[agent] insecure = true` or `CAI_INSECURE=true` (using the configured application environment prefix).
`--no-insecure` overrides those defaults.
This setting does not change TLS verification or the execution sandbox.
Approval follows canonical directory ancestry: siblings and symlinks resolving outside the approved tree do not inherit it.
Repository instructions above the authorized root are excluded.
Global instruction files keep their existing settings.
An old session does not implicitly approve its workspace.

## Execution policy

`--yolo` disables local tool confinement and command environment filtering; `--no-yolo` restores the default restricted mode.
Set `CAI_YOLO=true` or `[agent] yolo = true` in config.ini for the same behavior.
Precedence is CLI, environment, configuration, then the built-in default `false`.
Embedded apps use their configured environment prefix (for example, `SMITH_YOLO`).
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
