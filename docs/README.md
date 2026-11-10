# Documentation

Choose a guide for the job at hand.
User guides describe the application; SDK guides describe embedding; architecture references explain ownership and protocol contracts.
Run `cai --help` for startup flags, `/help` for commands and keys, and `cai --dump-config` for the complete current settings template.

## Terminology

| Term | Meaning |
|---|---|
| **Workspace** | The canonical directory that scopes saved sessions and has at most one running CAI backend. |
| **Session** | A saved agent interaction, identified by a session ID. Its history, input queue and goal can outlive the backend and its attachments. |
| **Attachment** | A live connection from a frontend to a workspace backend. An attachment can control the backend or observe it. |

The backend selects one active session. Selecting another session keeps the attachment, and detaching leaves the saved session available to resume. Use **session** for the saved interaction in CAI prose. Older references to a conversation or chat thread mean a session.

The workspace protocol's `conversation` field carries the **session epoch**, which identifies the active binding. The persistent session ID is separate. OpenAI's **Conversations API** uses a provider session ID, exposed by names such as `cai_conversation` and `conversation_id`. Operating-system threads describe execution.

## Use the agent

| Guide | Covers |
|---|---|
| [Terminal](terminal.md) | Input, steering, goals, model changes, reviews, shells and workspace switching. |
| [Configuration](configuration.md) | Precedence, paths, named stores, approval, execution policy, temporary runs and logs. |
| [Sessions](sessions.md) | Resume, recall, retention, deletion, trim, compaction and export. |
| [Appearance](appearance.md) | Theme pairs, reviewer offsets, status fields and timestamps. |
| [Providers](providers.md) | ChatGPT imports/login, API keys, protocols and retries. |
| [Tools](tools.md) | Capability policy, managed tools and output presentation. |
| [MCP connections](mcp.md) | Named servers, ChatGPT apps, lazy discovery and runtime enable/disable. |
| [Skills](skills.md) | Discovery, selection and installation. |
| [Dynamic tools](plugins.md) | Process/Lua packages and hot reload. |
| [Chrome](chrome.md) | Extension setup, tab ownership, operations and compatibility. |
| [Messaging](agent-messaging.md) | Mailboxes, approval and delivery between sessions. |
| [Shared context](context.md) | Searchable text, binary attachments, cross-agent storage and export. |
| [Kanban](kanban.md) | Shared boards, tasks and optional HTTP integration. |
| [Troubleshooting](troubleshooting.md) | Evidence, logs, storage recovery, terminal and completion limitations. |

## Embed CAI

| Guide | Covers |
|---|---|
| [C SDK](c-sdk.md) | Linking, handles, sessions, tools, authentication, limits and logging. |
| [Agent runtime](runtime.md) | Host loop, Smith, instructions, settings, goals and history APIs. |
| [Lua SDK](lua.md) | Installation, runtime facade and native store ownership. |
| [MCP SDK](mcp-sdk.md) | Framework-neutral server handler and remote client APIs. |
| [Application embedding](embedding.md) | Complete C/Lua application, configuration and supervisory controls. |
| [Application server](application-server.md) | Terminal-free owner lifecycle and package selection. |
| [Application client](application-client.md) | Typed socket attachment without a runtime/store. |
| [Application utilities](application-utilities.md) | Maintenance without inference or a frontend. |
| [Lua owner](lua-workspace-owner.md) / [Lua client](lua-workspace-client.md) | Async lifecycle and typed observations. |

The installed public headers are the API reference for fields, ownership and callbacks.
Source examples live under `examples/` in the repository; [the examples guide](https://github.com/c89systems/cai/blob/trunk/examples/README.md) identifies runnable entrypoints.

## Ownership and protocol references

- [Workspace architecture](workspace-server.md), [process handoff](workspace-process.md) and [terminal attachment](terminal-attachment.md).
- [Backend input queues](remote-input-queue.md), [command policies](interactive-command-audit.md), [semantic command execution](workspace-commands.md) and [producer delivery](workspace-producer-delivery.md).
- [Batch work](workspace-batch.md), [local shell recording](workspace-wire-shell.md) and [live shell output](workspace-live-output.md).
- Wire payloads: [queue](workspace-wire-queue.md), [status](workspace-wire-status.md), [transcript](workspace-wire-transcript.md), [history](workspace-wire-history.md), [catalog](workspace-wire-catalog.md) and [startup claims](workspace-wire-claims.md).
- [Session journal/storage](journal.md), [native export](workspace-export.md), [patch matching](patch-tool.md) and [model metadata maintenance](model-metadata.md).

## Development and measurements

- [Building, tests, local services and releases](development.md).
- [Benchmark workload and commands](benchmarks.md).
- [Initial transport baseline](benchmark-baseline.md) and [presentation attribution / CBOR optimization](performance-cbor-presentation.md).
- [Historical startup/CPU measurements](startup-responsiveness.md), taken before the workspace-server cutover.
- [Network prompt and event transport specification](network-prompt-events.md), agreed behavior for the upcoming TCP/multicast feature; not a shipped interface.

Design proposals and dependency incident reports that have been superseded are not current API documentation.
Their history remains available in Git.
