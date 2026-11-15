# Stored agents and agent builder

Status: draft specification, not implemented. This consolidates the original notes in `stash/TODO.md` and `stash/agent_builder.md` with the requirement that different agents can continue the same session. Open decisions are listed explicitly below. Existing user guides describe current behavior until implementation lands.

## Purpose and terminology

A stored agent is a complete, named agent: its configuration, developer instructions, authentication, tools, skills, MCP connections and bundled files. Selecting one selects that agent's operating environment. An instruction template or a collection of configuration overrides alone is insufficient.

Use the existing [workspace, session and attachment terminology](README.md#terminology). A workspace is the canonical directory served by one backend. A session is the saved interaction. An attachment is a live frontend connection. The agent operating a session can change without changing any of these identities.

The existing C `cai_agent_preset` descriptor supplies only part of the execution policy. It does not represent the complete stored agent described here and must not constrain the repository or builder design.

## Session continuity

Agent selection must be independent of session storage identity. Selecting another agent must not change the session namespace, allocate a replacement session, copy the journal, or hide sessions created by other agents.

The following survive an agent change:

- Session ID, workspace binding, journal order and existing storage ownership.
- Retained model context, compacted context, prompts, replies and tool history.
- Prompt recall, pending backend input, goal status, accounting and retention.
- Files and assets already available to the session, and attached frontends.

The selected agent supplies the instructions, provider settings, capability policy, tool registrations, skills and MCP configuration used for subsequent work. Historical tool calls remain historical data; they neither grant the new agent those tools nor run again. Historical instructions must not override the newly selected agent's instructions.

Switching agents does not merge unrelated sessions. It allows different agents to operate the same saved session successively. Normal review/subagent isolation remains intact.

The journal must identify successful agent activations and the stored definition revision used. Exports and replay retain the original attribution of earlier activity. Export must remain possible after the stored agent is edited or removed; historical instruction provenance cannot depend solely on resolving its current name.

Provider-specific continuation IDs, opaque compaction items and hosted-tool state need explicit handling. They must not be sent blindly to another provider. Preserve the authoritative journal and construct compatible continuation context, or reject the change before activation with an actionable reason. Silently dropping history, starting a new session or claiming unsupported provider portability does not satisfy this requirement.

## Storage ownership

Store agents through liblockdc, using the same configured client/store as the application unless explicitly configured otherwise. The proposed registry namespace is `<application-prefix>.agents`, normally `cai.agents`, with one state object per canonical agent name.

The object contains agent metadata and native authentication data. Attachments contain `config.ini`, `SYSTEM_PROMPT.md`, optional global `AGENTS.md`, and selected tools, skills and other bundled files. Directory structure is preserved in attachment names. MCP definitions and provider endpoints belong to the agent's configuration.

The readable identity is separate from the canonical storage name. Normalize names consistently for lookup, creation and dumps, and reject collisions rather than silently addressing another agent. Keep the existing identity validation rules for display text.

Use liblockdc's state-object lease and revision facilities to publish a validated definition and its attachments consistently. Concurrent edits or credential refresh must not overwrite one another's changes. An active runtime retains the definition it resolved until an explicit activation; editing a stored agent must not mutate a running agent halfway through a response.

Bundled files are materialized beneath the application's agent cache before their tools or skills are registered. Use stable revision-specific paths so an edit cannot overwrite files still used by an active agent. Include the resolved paths in the normal discovery instructions. Materializing files must not execute them; existing execution policy continues to apply.

Transfer attachments through bounded streaming reads and writes. Do not materialize an entire agent archive behind a streaming-looking interface.

Session journals and recall collections remain in their session store independently of the agent registry. Context and todo tools use their configured namespaces; choosing their defaults must not change session ownership. If definitions and tool documents share a namespace, use explicit document kinds and query constraints so context search cannot return configuration or authentication objects. Exact defaults for agent-associated context/todo data remain to be settled.

## Initialization and configuration

The default agent is named `default`. If it does not exist, initialize it from application defaults and the explicitly selected or default seed configuration. Import selected global instructions and embedded resources through the same validation path used for other agents. Concurrent initialization must create one valid default agent without overwriting an existing definition.

Do not log into ChatGPT merely because the default agent is initialized. Native CAI credentials belong in liblockdc. Loading an external Codex `auth.json` remains an explicit configuration choice. Credential import into a new agent is explicit; the draft name for that option was `--from-auth-json`, and its exact interface is not yet settled.

After seeding, the agent's stored `config.ini` is authoritative for agent behavior. Do not keep automatically generating or loading a second disk configuration for the same behavior. Retain `--dump-config` as the configuration-template utility; use the agent dump operations for actual stored definitions.

Explicit invocation flags override the selected agent's configuration. The original proposal removes environment-variable overrides of stored agent behavior. This does not remove `LOG_*`, the captured environment used by commands, or environment lookup explicitly requested by a provider's API-key configuration. Store bootstrap settings require a separate decision below because they are needed before an agent can be read.

Settings that identify the workspace server or locate its store cannot silently relocate an already running backend when an agent is selected. Resolve those host responsibilities separately from the agent's operating settings. Preserve workspace approval and existing execution-policy enforcement.

## Commands and builder workflow

These are proposed interfaces, not currently available commands. Apply `-C` / `--directory` before resolving operation-relative paths. It changes directory and fails if the directory does not exist; it does not create it.

| Interface | Intended behavior |
|---|---|
| `-u NAME`, `--use NAME` | Select the stored agent to operate the requested or automatically resumed session. |
| `--build [DIR]` | Create an editable agent source directory, using the selected agent as the starting point; default DIR is cwd. Create missing output parents. |
| `--make [DIR]` | Validate the source directory and publish the named agent; default DIR is cwd. Reject an existing name unless `--yes` / `--force` explicitly permits replacement. |
| `--builder` | Run the specialized agent builder, which asks what agent to build and produces the same editable source format. |
| `--edit-agent NAME` | Edit the stored configuration, developer instructions and optional global instructions using the user's editor, then validate and publish together. |
| `--dump-agent NAME` | Export one agent into `NAME/` beneath the selected directory. |
| `--dump-agents [DIR]` | Export all agents into one subdirectory each; default root is `agents/`. |
| `--dump-system-prompts [DIR]` | Export the built-in instruction catalog for inspection and use in an agent's instruction file. |
| `/use NAME` | Proposed live equivalent of agent selection, submitted through the ordinary backend command queue. |

For example, `cai --build new_agent --use old_agent` uses `old_agent` to populate `new_agent/`. `cai -u engineer -R ID` selects `engineer` while resuming session ID. It does not import or duplicate ID's history.

The source directory contains `config.ini`, `SYSTEM_PROMPT.md`, generated `EXAMPLE_SYSTEM_PROMPT.md`, optional `AGENTS.md`, and bundled resources. The original notes also request `agent.json`; its relationship to configuration, required fields and resource inventory needs a precise package schema before importer implementation.

The agent builder uses dedicated instructions and the existing tool/runtime architecture. It can produce configuration, MCP definitions, custom tools, skills and files, rather than only writing a prompt. Validate and publish its output through the same operation as `--make`; do not give the builder an independent storage writer. Being in an empty directory or repository is a convenience, not permission to erase an existing workspace.

`--edit-agent` uses a frontend or local utility editor handoff. A persistent headless backend must not launch an interactive editor. Cancellation, editor failure or validation failure leaves the stored agent unchanged. Omit `AGENTS.md` when its edited content contains only whitespace. Edit bundled resources through the source-directory workflow.

## Instruction composition

The default `SYSTEM_PROMPT.md` contains `{{runtime_default}}`. If that token occurs anywhere in the file, select the existing provider/model-dependent base instructions instead of concatenating the surrounding text. Otherwise use the file as the literal base for every provider/model selected for that agent.

`EXAMPLE_SYSTEM_PROMPT.md` is an editable CAI-specific example based on the existing coding-agent template, with provider-specific self-identification removed. It does not become active unless chosen as the actual instruction file.

Keep one instruction-composition path for runtime use, builder output, instruction dumps and exports. Apply explicit parent-only `-z` overrides as currently specified, and append `-I` additions before the automatic tools/MCP/skills guidance. Keep the optional chart guide and existing reviewer instruction selection in this same path. Agent selection must not accidentally propagate a parent-only override to children.

## Live activation and client/server separation

The backend owns selection, validation, activation and persistence. Softline submits and observes the command through its existing queue adapter; another frontend must be able to perform the same operation without Softline. Keep entries editable until applied.

Live selection is a whole-turn operation. A steering submission that cannot be applied at a steering boundary must be reclassified visibly into the turn queue according to the existing command policy. It must not block later steering by remaining falsely classified as steerable.

Resolve the target definition, validate configuration and capabilities, prepare resources and construct a compatible continuation before committing a switch. Keep the existing session collection ownership throughout. Publish the successful activation durably before acknowledging completion or starting work with the new agent. Failure leaves the prior active agent, session and pending input intact.

Keep one backend per workspace and the existing controller/observer rules. An observer cannot change the active agent. An explicit agent selection made while attaching to an existing backend must be an acknowledged owner operation, rather than a second backend or an unnoticed client-local setting. Switching must neither replay already displayed history nor lose the frontend's transcript cursor.

Core libcai owns runtime/session continuation behavior and host-neutral hooks. The application server supplies the liblockdc repository adapter and full application configuration; the terminal adapter supplies editor and presentation behavior. Do not force liblockdc, Softline, libmdf or Lua dependencies into core libcai to implement this feature.

## Required implementation changes and verification

Current runtime resume rejects a saved preset name or prompt revision that differs from the selected runtime. Offline Markdown export also assumes the built-in Smith descriptor. Replace these assumptions for ordinary agent activation, while retaining malformed-state validation and private review/subagent isolation. Existing metadata is provenance, not ownership of a session by one agent.

Required tests include:

- Create, load, edit, replace, dump and rebuild agents through liblockdc; test collisions, invalid input, concurrent publication and interrupted writes.
- Initialize `default` once, without automatic login, and verify subsequent agent configuration comes from storage.
- Resume one session with two agents having different literal instructions and tools. Verify identical session identity and history, changed future requests, and no re-executed tools.
- Switch after compaction, with an active goal, with queued input, and across backend restart. Verify no duplicate replay or lost recall/accounting.
- Verify a queued live switch, steering reclassification, observer rejection, existing-owner attachment and rollback after preparation/publication failures.
- Test changed providers/models and provider-specific continuation state explicitly, including unsupported conversions that leave the old agent usable.
- Export a multi-agent session after its original stored agent is changed or removed. Preserve attribution without including credentials.
- Verify bundled resources and instruction composition, including reviewer policy, runtime-default selection, parent-only overrides and optional guidance.
- Exercise native Debug's constrained worker stacks and the default musl worker stack; keep attachment transfer streaming and allocation bounded.

## Open decisions

1. **Selection when `-u` is omitted on resume.** The old notes say `default`. The current recommendation is to restore the agent selected by the session's last successful activation, using `default` for new sessions. This recommendation has not yet been accepted.
2. **Store bootstrap configuration.** The current recommendation is a minimal disk configuration containing store location/authentication settings only. The alternative is standard local storage or explicit flags. Agent behavior remains in liblockdc either way. This recommendation has not yet been accepted.
3. **Package and credential transfer contract.** Define `agent.json`, resource inclusion, credential import, and whether/how an explicitly requested agent dump can contain secrets before implementing round-trip import/export. Credentials must never appear in ordinary session export or model context.
4. **Agent-associated context and todo defaults.** The original notes suggest storing these beside the agent, but do not settle shared versus agent-specific namespaces and selection rules. These must remain independent of session history.

This document records the design discussion and required behavior. It does not claim these interfaces exist or that all implementation decisions are closed.
