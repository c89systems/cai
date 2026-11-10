# Shared context

Context is a shared document store, independent of sessions and workspaces. CAI enables its tools by default. The agent stores or retrieves context when the user's task calls for it; starting or resuming a session does not search context automatically. Items have no expiry by default and survive session deletion, trimming and compaction.

## Tools

| Tool | Purpose |
|---|---|
| `context_create` | Store its entire freeform input as a new Markdown/text document. Returns an XID and version. No metadata header or delimiters. |
| `context_set` | Change supplied metadata: title, filename, tags, date or media type. Omitted fields are preserved; null clears a field. |
| `context_query` | List items or search using LQL, returning bounded metadata pages and a cursor. Default 20 items; maximum 32. |
| `context_get` | Read metadata, current version and at most 16 KiB of text. A truncated preview directs the agent to download the complete document. |
| `context_upload` | Classify and store a local regular file, capturing its filename, media type and size. |
| `context_download` | Write the complete text or binary file directly to a local path, returning a receipt rather than sending file bytes through inference. |
| `context_replace` | Replace content from a local file using automatic text/binary detection, preserving identity and editable metadata. |
| `context_delete` | Delete the shared document and its attachments. |

`context_set`, `context_replace` and `context_delete` require `id` and `expected_version`, taken from the most recent read or successful receipt. A conflict requires reading the item again before deciding whether to retry. Deletion affects every agent using that namespace and store.

Replacement preserves filename, title, tags and document date. It updates size and system modification time; changing between text and binary also updates kind and resets media type to the corresponding default. Binary-to-text replacement removes the obsolete attachment.

A text creation call contains only the desired text:

```markdown
# Deployment

Build, deploy to staging, then verify health checks.
```

After receiving its ID and version, the agent may set metadata separately:

```json
{"id":"<returned-xid>","expected_version":1,"filename":"deployment.md","tags":["operations"]}
```

System `created_at` and `updated_at` timestamps are Unix seconds and maintained automatically. The optional `date` is a separate user-defined document date. Unset or cleared string metadata is stored as an empty string; cleared tags are an empty array. Filenames are safe basenames, never keys. Tags are arrays of up to 32 strings, each at most 128 bytes. Titles are limited to 1,024 bytes, filenames to 255 bytes, and dates/media types to 127 bytes.

## Storage and search

Each XID identifies one ordinary lockd state object. Text items have `kind = "text"` and their original text in the JSON `content` string. File items have `kind = "file"`, metadata in the state, and bytes in the native attachment named `content`. There is no workspace collection or shared catalog object to lease. Mutations acquire an exclusive object lease and release it after the operation; long transfers renew at chunk boundaries, without an idle polling service.

Text upload detection examines every byte: valid UTF-8 without binary control bytes is text, including empty files. NULs, invalid UTF-8 and recognized binary signatures make an attachment. Non-UTF-8 text is preserved as a binary attachment; it is not silently transcoded. Detection uses a first pass over the open file, followed by a streamed upload from the same descriptor. CAI checks for file changes before completing the operation. Extensions do not decide text versus binary. Text MIME defaults to `text/plain`, or `text/markdown` for `.md`/`.markdown`; otherwise files use `application/octet-stream`.

Examples of `context_query` arguments:

```json
{}
{"query":"icontains{field=/content,value=\"deployment\"}","limit":10}
{"query":"eq{field=/title,value=\"Deployment procedure\"}"}
{"query":"eq{field=/tags[],value=\"operations\"}"}
```

Pass the returned cursor with the same query to retrieve another page. LQL supports the backend's native selector/index capabilities; this is not semantic or relevance-ranked search. The default listing uses `exists{/id}`. Query results exclude document bodies and attachment bytes. On current Pouch, metadata reads still consume the matching state's JSON stream because projected query documents are not supported; they discard content rather than materializing it.

File operations follow the runtime's existing filesystem policy and live YOLO setting. Restricted access rejects paths outside the workspace, symlink traversal and hard-linked upload files. Downloads never overwrite an existing destination and publish each completed file atomically. Upload/download and content replacement are streamed, with bounded buffers. The model transport may already have spooled completed freeform input before invoking the tool; the context adapter reads that input without making another full copy. Array-valued metadata updates join the SDK's push read and pull update through a bounded pipe on a default-size worker, without a full-document spool.

## Configuration

```ini
[agent]
context = true

[storage]
context_store =
context_namespace =
```

An empty store inherits the active session store and reuses its existing client. This also applies to `--temp`: its context is temporary unless an explicit named context store selects persistent shared storage. An empty namespace derives from the application's namespace prefix, normally `cai.context`. Agents with different application prefixes can share context by explicitly selecting the same namespace and store.

| Flag | Environment | Meaning |
|---|---|---|
| `--context` / `--no-context` | `CAI_CONTEXT` | Enable/disable tools; default enabled. |
| `--context-store NAME` | `CAI_CONTEXT_STORE` | Select a configured `[store.NAME]`; empty inherits history. |
| `--context-namespace NAME` | `CAI_CONTEXT_NAMESPACE` | Override the shared namespace. |

Embedded applications use their configured environment prefix instead of `CAI_`. Explicit flags override environment and INI settings. These are workspace-server startup settings; an attached frontend cannot silently change a running owner's storage identity.

A separate endpoint uses the existing named-store configuration:

```ini
[storage]
context_store = knowledge

[store.knowledge]
endpoint = pouch:///srv/knowledge
key_file = /srv/knowledge.key
```

HTTP(S) lockd endpoints and their existing `client_pem` setting are supported too. Features selecting identical resolved connection settings share the app's client handle; namespaces remain per-operation.

## Export without inference

```sh
cai --context-export output/knowledge
cai --context-store knowledge --context-export output/knowledge
```

The utility opens storage directly, without starting an agent, connecting to an inference provider or needing workspace approval. Each item becomes `output/knowledge/<id>/<filename>`, with metadata and the exported version in `output/knowledge/<id>.context.json`. Keeping metadata outside the item directory preserves even a file named `.context.json`. Missing filenames use `<id>.md` for text or `<id>.bin` for binary data. Duplicate filenames remain distinct because each item has its own ID directory. Text JSON strings are decoded back into original text; attachment bytes are copied unchanged.

Export uses bounded pages, creates missing directories, and refuses existing destination files. Earlier completed items remain if a later item fails. It is not an atomic snapshot of the entire store: a changed item produces a version conflict rather than mismatched metadata and content.

## Embedding

The optional `libcai_context` product owns the liblockdc dependency. Core libcai remains independent of liblockdc, terminal rendering and application storage. Enable `CAI_BUILD_CONTEXT`, include `cai/context.h`, and consume `cai::context_static` or `cai::context` from `find_package(cai_context CONFIG REQUIRED)`. The pkg-config name is `cai-context`. Application-server builds include this adapter automatically.

Initialize `cai_context_config`, supply a borrowed `lc_client *`, namespace and optional filesystem root, then call `cai_context_open`. Register on an agent with `cai_context_register`, or on a tool registry with `cai_context_register_registry`. The adapter and client must outlive every registered agent/registry. Registering does not query or load documents. `cai_context_create`, `cai_context_call` and `cai_context_export` are also usable directly without inference.

Close agents/registries before the adapter, then close the borrowed client. Change file policy with `cai_context_set_yolo` only while tools are idle. The adapter never closes the client.
