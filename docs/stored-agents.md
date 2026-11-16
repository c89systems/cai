# Stored agents and agent builder

A stored agent is a named operating environment: configuration, developer instructions, native authentication, tools, skills, MCP connections and bundled files. It can operate any compatible session in the configured session store. The C `cai_agent_preset` descriptor supplies execution defaults; it is only one part of a stored agent.

A workspace is the canonical directory served by one backend. A session is the saved interaction. An attachment is a live frontend connection. Changing the agent changes none of these identities. See [terminology](README.md#terminology) and the [configuration guide](configuration.md#stored-agent-workflow).

## Selection and session continuity

New sessions use `default`. Resume restores the last successfully selected agent. `-u NAME` / `--use NAME` explicitly selects an agent before replay, including when attaching to an existing workspace owner. `/use NAME` changes the active agent; `/use` reports its name and definition revision. An observer cannot activate an agent.

Live activation waits for a whole-turn boundary. A steering submission of `/use` is visibly reclassified into the turn queue. The backend prepares the selected definition and capabilities, verifies continuation compatibility and publishes the binding durably before acknowledging it or admitting work under it. Failure leaves the previous agent usable.

Activation transfers the existing context and pending input queue by ownership. It does not restore the journal again, copy the session or replay already displayed output. Session ID, journal ownership, retained and compacted context, prompt recall, queued input, goal, accounting and retention survive. Historical tool calls remain history; they are neither executed again nor granted as capabilities to the new agent.

The selected definition supplies future instructions, provider/client settings, tools, skills and MCP connections. Explicit invocation flags override its settings. Parent-only `-z` instructions apply to the initial parent and are cleared when selecting another agent. Resuming the same definition restores saved model/reasoning controls; selecting a different definition uses that definition's defaults, subject to explicit controls from the attaching invocation. `/new` selects `default`; `/resume` restores the destination session's selection. Operational controls remain in effect when changing sessions without changing agents.

Provider-specific continuation IDs, opaque compaction and hosted-tool state are not portable by implication. Backend/account changes and models requiring incompatible context conversion are rejected before live activation. Select a compatible model first or use another session. Automatic cross-provider context conversion is not implemented. API-key rotation alone is not treated as proof of a different account; explicit organization/project IDs and native OAuth account identity participate in compatibility checks.

## Storage and credentials

The registry uses the configured `agents_store`, normally the same client/store as sessions. Each definition is an ordinary state object in `<namespace-prefix>.agents`, normally `cai.agents`, keyed by canonical name: `agent.engineer`. Its JSON body contains kind, name, definition revision, bundled-file inventory and optional native authentication. There is no metadata file named `agent.json`.

Attachments contain `config.ini`, `SYSTEM_PROMPT.md`, optional global `AGENTS.md`, and selected tools, skills and other resources. Directory structure is preserved in attachment names. Publication validates the package and commits metadata and attachments under one object lease. Replacing or editing a definition preserves its latest native credentials unless an explicit credential import replaces them. Concurrent edits use the definition revision to detect conflicts.

Runtime `config.ini` streams directly from lockd into the configuration parser. It is never written to the runtime cache. Native authentication remains bounded parsed JSON, at most 1 MiB, and is never cached as a file. Bundled resources are streamed to private revision-specific cache directories before registration, so editing a definition cannot overwrite an active runtime's files. Existing tool execution policies still apply.

`cai --login --use engineer` stores native ChatGPT credentials in that agent object. `--make DIR --from-auth-json FILE` explicitly imports credentials. Login and token refresh update the native field without replacing the definition revision or its attachments. ChatGPT apps may use these credentials while another provider performs inference. Missing required credentials produce an actionable login error; optional apps do not force login. External files are never discovered implicitly. `--auth-json FILE` explicitly selects the core file-backed authentication path instead of native agent storage.

API keys may be placed directly in `[client] api_key` in the editable configuration and are retained in encrypted registry storage. Package exports omit declared secret fields and native authentication unless `--include-credentials` is explicit. Arbitrary prompt text, scripts and environment assignments are ordinary bundled content: inspect them before sharing. Session exports do not include authentication objects or secret configuration fields.

Session journals and recall remain independent of the agent registry. Context and todo tools retain their configured namespaces; this cutover introduces no agent-specific namespace policy.

## Bootstrap and initialization

The local `config.ini` locates storage and the workspace server. Its non-host settings seed `default` once. Initialization validates and captures explicitly configured instructions, tools, skills and reviewer resources through the same validators and uploader used by source publication. Concurrent initializers publish one complete definition; a failed attempt leaves initialization retryable. Initialization never triggers provider login.

After initialization, stored agent configuration controls agent behavior. Editing the bootstrap file does not replace a stored definition. CAI does not generate or rewrite that file automatically; `--dump-config` remains an explicit reference-template utility. Application-prefixed environment variables select bootstrap settings only. Captured command environments, explicit API-key environment lookup and `LOG_*` retain their normal behavior.

Workspace identity, store endpoints/selectors, host paths and server lifetime belong to the host. Agent configuration cannot relocate an existing backend or session store. Explicit host requirements still participate in attachment compatibility checks; another agent's bundled resource paths and configuration origin do not identify the workspace server. Workspace approval and execution policy remain enforced.

This is a clean cutover of the unreleased agent format. Earlier configuration-only definitions are unsupported: republish them from source or recreate the registry definition. There is no automatic migration or parallel authentication namespace. Session storage remains independent.

## Source and builder commands

Apply `-C` / `--directory` before operation-relative paths. It selects an existing directory and does not create it.

| Interface | Behavior |
|---|---|
| `--build [DIR]` | Export editable source based on the selected agent; default DIR is cwd. Create missing parents; require an empty destination. |
| `--make [DIR]` | Validate and publish source; default DIR is cwd. Existing names require `--yes` / `--force`. |
| `--builder` | Start with dedicated builder instructions and ask what to build unless an initial prompt was supplied. |
| `/builder` | Activate builder instructions in the current session at a whole-turn boundary. |
| `--edit-agent NAME` | Edit configuration, base instructions and optional global instructions locally, then validate and publish together. |
| `--dump-agent NAME` | Export into `NAME/` below the selected directory. |
| `--dump-agents [DIR]` | Export each agent into a subdirectory; default root is `agents/`. |
| `--dump-system-prompts [DIR]` | Export each built-in model-profile prompt, reviewer instructions and builder instructions; default root is `prompts/`. No runtime or authentication is opened. |

For example:

```sh
cai --build agents/engineer
# Edit agents/engineer/config.ini and SYSTEM_PROMPT.md.
cai --make agents/engineer
cai --use engineer -R SESSION_ID
cai --edit-agent engineer
cai --builder
cai --dump-system-prompts prompts
```

Source contains `config.ini`, `SYSTEM_PROMPT.md`, generated `EXAMPLE_SYSTEM_PROMPT.md`, optional `AGENTS.md`, and bundled resources. `[agent] name` is editable; a nonempty value wins on publication, while an omitted or empty name uses the source directory basename. Build initially infers the destination basename. Canonical names use lowercase ASCII letters, digits, `_` and `-`; spaces become `_`, other characters are removed, and 1–64 usable characters are required. Display identity is separate.

Resources under `tools/`, `skills/` and `files/` use bounded streaming transfer. Symlinks and special files are rejected. Instruction files must be NUL-free text of at most 1 MiB; whitespace-only `AGENTS.md` is omitted. Tool manifest validation prepares registrations without invoking tools. The builder produces the same source format and uses the ordinary `--make` publisher, rather than an independent storage writer.

## Editing lifecycle

`--edit-agent` is a local utility, never an editor launched by a persistent headless owner. It selects `VISUAL`, then `EDITOR`, then `vi`. Only the three editable text files are exported into a private temporary directory; bundled tools and native authentication are not exported for editing. Configuration may contain keys, so the directory is mode 0700 and files are mode 0600.

After the editor exits, CAI validates the whole edit and publishes only if the stored definition revision is unchanged. Rename the agent through the source workflow; editing keeps the canonical name fixed. Editor cancellation, signal failure, validation failure and revision conflicts preserve the stored definition. Host STOP cancels and reaps the editor. The temporary tree, including editor backups, is removed on save, cancel and ordinary failure. An uncatchable process/host crash cannot run cleanup.

## Instructions and historical export

If `SYSTEM_PROMPT.md` contains `{{runtime_default}}` anywhere, it selects the provider/model-dependent built-in base. Otherwise the entire file is the literal base. The example prompt is inactive unless chosen as the actual base. Reviewer policy, parent-only `-z`, `-I` additions, optional chart guidance and automatic tools/MCP/skills instructions use the existing composition path. Builder instructions remain parent-only.

Successful activation records name, revision and effective instructions as bounded `agent_activation` journal chunks. Ordinary commits retain a compact binding reference and instruction digest rather than copying the instructions repeatedly. The references link only successful activations; failed publication chunks cannot become exported provenance. Model changes record a new instruction stream only when the effective instructions change.

Markdown and portable JSONL exports preserve this provenance independently of the current registry. Editing or removing a definition cannot rewrite historical instructions. History trimming removes model turns while preserving successful agent attribution; JSONL export/import retains the linked records. Offline export does not resolve a mutable agent definition to reconstruct its recorded prompt.

## Embedding and verification

Core libcai owns session continuation, compatibility checks, runtime activation and instruction provenance. `cai_agent_runtime_open_agent` gates recovered work until the selected binding is durable. `cai_agent_runtime_activate` returns a prepared replacement owning the same session/context/queue; the host closes the retired runtime before retiring its clients and capability receivers. Preparation/publication errors leave the old runtime usable. These APIs add no liblockdc, frontend or Lua dependency to core libcai.

libcai_app supplies registry/configuration adapters and services. `cai_app_client_select_agent` selects a destination and optional agent before replay/producer binding. Softline is one frontend adapter and submits `/use` through the ordinary backend input policy.

Executable coverage includes [package publication and startup selection](../tests/cli_agent_packages_test.sh), [editing and cleanup](../tests/cli_agent_edit_test.sh), [headless activation and historical exports](../tests/cli_agent_activation_test.sh), [builder mode](../tests/cli_agent_builder_test.sh), [prompt catalog](../tests/cli_prompt_catalog_test.sh), and [core activation/rollback](../tests/runtime_activation_test.inc). These complement the standard Debug units and default-worker-stack application checks. Unsupported provider conversion is an explicit rejection, not a portability claim.
