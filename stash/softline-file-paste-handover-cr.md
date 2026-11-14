# Change request: streaming clipboard and file-upload handover

Status: proposal for Softline, requested by CAI on 2026-11-14. This document requests an upstream feature; it does not implement a CAI uploader or patch Softline.

## Outcome

Allow an application to take ownership of pasted images and uploaded files before Softline writes a local copy or inserts a pathname. The application streams the bytes to its backend, where the file is stored. When storage succeeds, Softline inserts the actual backend filesystem path into the originating draft.

The motivating deployment has a terminal frontend and a headless agent on different hosts. The agent needs a real path usable by `view_image`, shell commands, Python and ordinary filesystem tools. A frontend-local pathname or an asset URI is insufficient. Correlation IDs may identify an internal transfer, but they must not replace the filesystem path in the prompt.

This must also support applications whose frontend and backend share a host. CAI should use the same handover path in both deployments.

## Current gap

CAI currently consumes Softline 0.17.0. Its public clipboard surface exposes `disable_image_paste`, `image_paste_path_template` and `sl_set_image_paste_path_template()`. Ctrl-V runs the built-in image capture, saves PNG/JPEG bytes into a local file, and inserts that local pathname into the draft. The inspected implementation calls `sl_clipboard_save_image()` from the key handler and has a fixed 32 MiB image-transfer limit.

A pathname template changes where the frontend saves a file; it cannot move that file to another host. A notification after saving would still impose local staging. Reimplementing clipboard acquisition in every embedding application would duplicate Softline's X11 integration and bypass its native editing behavior.

The requested extension is a file-content handover, independent of terminal ownership handoff and of the remote prompt-queue backend. Softline should not acquire dependencies on CAI, CBOR, HTTP, liblockdc, a particular storage service or an inference provider.

## Sources and user intent

Use one handover mechanism for these sources:

| Source | Required behavior |
| --- | --- |
| Clipboard image | Ctrl-V can stream the selected PNG/JPEG representation without creating an image file on the frontend. |
| Clipboard file selection | Recognize X11 `text/uri-list` and `x-special/gnome-copied-files` when advertised. Open and stream the selected local files. |
| Explicit file upload | Expose a public application entry point that uses the same lifecycle for selected files or application-provided byte sources. A file picker, slash command or other frontend can call it. |
| Ordinary clipboard or terminal text | Preserve normal text-paste behavior. A pathname-looking string is not an upload request. |

The transfer accepts arbitrary file contents, including text, PDF, images and binary data containing NUL bytes. Content is not decoded as UTF-8, rendered as Markdown or automatically unpacked. Uploading a text file inserts its server path, rather than inserting its contents into the draft.

An explicit file selection is a copy operation. A clipboard `cut` marker must not cause the original file to be moved or deleted. Initially support regular files; reject directories and special files without recursive traversal. A selected symlink may resolve to a regular file, subject to the application's admission policy; validate the opened object, not only its pathname.

Decode file-list URIs with bounded, strict parsing, preserving spaces, Unicode and percent-encoded filename bytes without interpreting them as shell syntax. Reject decoded NULs and malformed escapes. Only local `file:` selections are candidates for file opening; accept an empty authority or `localhost`, and reject other authorities and URI schemes. Fetching remote URLs belongs to the application. Do not scan ordinary bracketed paste, completion results, or prompt text for upload candidates.

When a clipboard advertises several supported representations, select one deterministically and report its kind and media type before reading its contents. File-list representations should take precedence over incidental preview images. An application must be able to reject a source before any file-content transfer begins.

## Public API contract

Expose a constructor-configured handover adapter, its borrowed application context, and owner-thread methods to resume, cancel and complete a transfer. Provide equivalent receiver-function and Lua surfaces. Concrete API spelling is an upstream choice; the following names describe the required lifecycle rather than a frozen C ABI.

| Event or operation | Contract |
| --- | --- |
| `begin` | Describe the paste batch, item order, source kind, bounded display filename, media type and optional byte-length hint. Assign a transfer ID before calling the adapter. The application accepts or rejects admission promptly. |
| `data` | Offer a bounded binary chunk with its byte offset. The application accepts the whole chunk or returns would-block without consuming it. |
| `source_end` | Confirm clean EOF and the actual byte count. This means acquisition ended; it is not confirmation that the backend stored the file. |
| `resume` | An owner-thread call after application transport capacity becomes available allows the same unconsumed chunk or next source read to proceed. |
| `complete` | An owner-thread application call, after backend acknowledgement, supplies the stored file's absolute path or a bounded failure reason. |
| `cancel` / terminal notification | Stop acquisition and further data callbacks, release source resources, and notify the application once so it can abort its backend transfer. |

The application can start a handover from an explicit file selection as well as receive one initiated by native Ctrl-V. Support an application-provided incremental source so a future frontend need not manufacture a local file merely to enter this API. Define source ownership and cleanup callbacks explicitly.

All editor and handover operations execute on the editor's owner thread. Callbacks must return promptly and must not recursively call editor, completion or resume methods. Synchronous acceptance of a chunk is allowed; transfer completion and further editor mutation are deferred to the native dispatch boundary. Network workers notify their owner through existing watch/wakeup integration rather than calling Softline from another thread.

Metadata and chunk pointers are borrowed only for the documented callback duration. A successful callback must consume or copy that bounded chunk before returning. Would-block means no consumption; Softline retains only that chunk and offers it again after explicit resume. Define duplicate completion/cancellation behavior, invalid ordering and stale-ID errors so retrying a notification cannot insert a path twice. Calling through a destroyed editor is never valid: the application must remove watches and terminate outstanding handovers before destroying it.

Installing an adapter hands storage ownership to that adapter. Failure must not silently fall back to saving an image in Softline's local cache. Applications without an adapter can retain ordinary standalone clipboard-image saving; this must share acquisition machinery rather than maintain a second clipboard implementation.

## Streaming and responsiveness

Implement actual producer-to-consumer streaming, including clipboard acquisition, application handover and application-provided sources. The first chunk must reach the adapter before the source has finished producing the file.

No complete-file memory buffer, base64 expansion, temporary file, local cache copy, memory mapping of the whole payload, or hidden spool may sit behind this API. Existing source files selected by the user are inputs, not staging copies. Backend storage of the uploaded file is the intended destination.

Use bounded heap buffers and bounded work per event-loop iteration. Clipboard selection negotiation and transfer must participate in asynchronous readiness; the existing synchronous save-image call cannot remain on the active key-handler path for adapter mode. A slow selection owner, source, network or backend must not block typing, cancellation, queue editing, resizing or incoming feed rendering. Regular-file reads that can block require an appropriate asynchronous acquisition adapter; a small read size alone does not make a blocking read safe for the editor thread.

Backpressure must propagate to acquisition. For X11 INCR, do not acknowledge unlimited additional source chunks while the application is blocked; read ordinary properties in bounded segments as well. A source that cannot be backpressured within the bound must fail explicitly, not be collected elsewhere. Capture deadlines must distinguish source inactivity from an application-requested pause.

Expose nonblocking source preparation/pumping so an application's owner loop can advance a handover outside an active readline call; integrate that same mechanism with native watches while editing. Expose a wait-for-backend-completion state without reading further source bytes. Neither the editor nor the application should need periodic polling, a thread per chunk, a larger thread stack, VLAs or `alloca()`.

## Limits

Introduce a configurable maximum payload size of **10 MB (10,000,000 bytes) per file by default**, including clipboard images. The unit and scope must be documented consistently. This replaces the hard-coded image-specific cap for handover mode.

- A zero-initialized size option selects the 10 MB default; zero does not mean unlimited. A positive value supplies an explicit byte limit.
- Enforce the limit using actual bytes, independently of filename, media type, advertised size, `stat()` results or X11 INCR hints.
- Reject a known oversized file before content transfer. For an unknown-length source, stop before passing a chunk that would cross the limit; report a size-limit failure and cancel the backend transfer. An exact-limit source is successful only if clean EOF follows.
- Apply the same cap if a selected file grows during reading. Report truncation, source read errors and clipboard-owner loss distinctly from clean EOF.
- Allow one active paste/upload batch per editor, with items acquired serially. Bound the selected-file list separately; recommend 16 files per batch by default, with zero selecting that default. Reject excess items explicitly rather than silently dropping them.
- Bound metadata, retained result paths and outstanding state independently of payload size. The application may impose stricter per-file, aggregate-batch or backend-storage limits and reject admission or subsequent chunks accordingly.
- Validate arithmetic for counters, lengths and configuration values. Reject allocations and sizes that cannot be represented on supported 32-bit targets.

The application and backend must enforce their own configured limits too. Softline's client-side cap is a resource boundary, not authority to bypass server policy.

## Draft, queue and undo behavior

Softline owns the insertion location and its relationship to native editing. A late upload completion must not insert at whichever cursor or draft happens to be current at that time.

1. Start at an editor-owned anchor in the initiating draft or queued-entry edit. Display a non-submittable pending marker/status with the file names and progress; marker text is never prompt content.
2. Allow surrounding draft edits and keep the insertion anchor consistent with them. Replacing the draft, cancelling that queued edit, removing the pending insertion or destroying its view invalidates the handover and notifies the application.
3. While the draft has unresolved uploads, Enter and Alt-Enter retain the draft and report that uploads are pending. Do not accept a prompt containing temporary markers or frontend paths, create a hidden application queue, or automatically submit when an upload finishes. Existing backend entries keep their identity, edit hold, approval and position.
4. After all items in the batch succeed, insert their returned server paths in original selection order as one undoable edit. Preflight the entire insertion against `line_max_len`; a failure preserves the existing draft rather than inserting a partial path list. Insert paths literally, without executing a command or treating the strings as completion triggers.
5. Undo removes that insertion as normal text editing. It does not unconditionally delete a server file that a submitted prompt or another retained reference may already use.
6. Cancellation or any item failure removes the batch's pending marker, preserves surrounding text and reports a concise inline diagnostic. Do not insert the successful subset of a failed batch. Notify the application so it can release incomplete or unreferenced backend files according to its storage policy.

The normal Enter/Alt-Enter queue and steering policies apply once paths are present and the user submits. Prompt history records resolved prompt text through its normal path; it must not contain temporary upload markers or duplicate entries caused by progress callbacks.

Application-level context also matters. CAI binds a transfer to its initiating workspace server, session and draft/edit identity. Switching to another server or session must never redirect its upload or insert that server's path into the new session. The adapter needs enough identity to reject stale completions and cancel the old handover.

If rendering/input is suspended for a pager or terminal handoff, transfer progress can continue through the application's event loop. Retain bounded completion state and reconcile the insertion on safe resumption of the original editor. A destroyed or replaced editor receives no late insertion.

## Server-path completion and CAI responsibilities

The final result is a real absolute pathname on the backend host, such as `/server/state/cai/uploads/session-id/report.pdf`. It must be usable by the agent under the backend's tool policy; it is neither a frontend pathname nor a public download URL. A transfer ID remains internal correlation metadata.

The backend chooses and validates its storage location and generated name. Clipboard filenames, source paths and media types are untrusted metadata, not authority to select a server destination. Reject terminal controls in completion text and render metadata safely. Backend acknowledgement follows a complete, successful upload and publication of the file; partial files never become usable prompt paths.

CAI owns the reusable streaming upload protocol, server-side storage, authentication/authority checks, cancellation, tool accessibility and session retention/export policy. Its terminal adapter bridges Softline's callbacks to the backend transport. A Web UI uses the same backend upload interface with its own capture surface. Softline owns acquisition and editor behavior, and requires no knowledge of server storage or LLM tools.

No client-side persistent image file is needed in this mode. Reading clipboard bytes or an explicitly selected existing file necessarily occurs on the frontend host; storage and subsequent agent processing occur on the server.

## Acceptance checks

Use deterministic sources and transport gates; test the observable contract rather than relying on real inference or arbitrary sleeps.

1. Paste PNG/JPEG images, upload a text file, a PDF and binary data containing NULs. Verify byte identity at the destination and insertion of the server-returned path only after acknowledgement.
2. Delay the last source chunk and prove the application already received earlier bytes. Hold the destination blocked, verify bounded memory/acquisition, and exercise typing, feed output, resize and cancellation while it remains blocked.
3. Cover normal X11 properties and INCR acquisition, missing DISPLAY, unavailable clipboard targets, selection-owner loss, source read errors, and nonresponsive source/backend cancellation. Confirm no cache or temporary file is created in adapter mode, including failure paths.
4. Accept exactly 10,000,000 bytes at the default limit and reject 10,000,001. Cover early known-size rejection, unknown or dishonest sizes, file growth, custom limits, the zero default, overflow and 32-bit size handling. Verify no excess bytes reach the adapter and cancellation reaches the destination after partial upload.
5. Paste a multi-file selection in order, including spaces, Unicode and percent-encoded names. Cover the list bound, duplicate names, empty files, a failed middle item, directories, special files, disappearing files, symlinks, remote file authorities and malformed URIs. Confirm source files are never moved or deleted.
6. Edit before and after the pending anchor, move the cursor, cancel/remove the pending insertion, replace the draft, switch sessions/workspaces and destroy the view. Deliver late/duplicate completion and cancellation notifications; no wrong-draft insertion, duplicate path, use-after-free or cross-session routing is permitted.
7. Exercise ordinary draft, queued-edit and steering-edit modes. Enter/Alt-Enter during upload must preserve the draft without submitting markers; after completion, explicit submission must retain normal FIFO, approval, edit-token and steering semantics.
8. Verify one undo restores the pre-insertion draft, redo restores all paths, an insertion exceeding the line limit is atomic, and upload progress does not create prompt-history entries. Verify safe reconciliation after pager/terminal suspension.
9. Fail adapter admission, pause/resume a chunk repeatedly, fail backend commit and exhaust allocation/transport capacity. No image-cache fallback, hidden full-file buffering, duplicate chunk acceptance or automatic prompt submission is permitted.
10. Run equivalent C and Lua checks, exercise standalone clipboard behavior without an adapter, and run acquisition/transfer callbacks on default-stack musl workers. Preserve ordinary text paste, bracketed paste, completion, history, undo and remote-queue regressions.

An end-to-end example should connect an editor to a fixture destination with a different filesystem root, stream a clipboard image and a selected file, and insert paths verified from the destination side. It must work when the frontend has no writable staging directory. CAI can then integrate the handover without recreating Softline's clipboard and draft state machines.
