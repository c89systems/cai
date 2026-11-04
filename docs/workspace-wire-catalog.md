# Presentation catalogs

The owner pushes its loaded skill names independently of status, transcript and
input-queue binding. Observers receive the same catalog. Names are presentation
metadata: they do not enter model context, open a resource, authorize an action
or execute a tool. `cai_app_client_config.catalog` receives completed borrowed
rows on its opening thread. It must not reenter pump, detach or close.

Each CATALOG frame has a definite `[kind, phase, code, text]` payload. The
envelope has request zero, conversation equal to the source epoch, stream equal
to a nonzero owner-local revision, and sequence equal to the row index. The
transport supplies and authenticates attachment/generation normally. This uses
the existing workspace envelope without a version negotiation or another
socket.

Kind 0 is SKILLS. Phases are BEGIN=0, ITEM=1, END=2 and ERROR=3. BEGIN has index
zero, code zero and empty text. ITEM has a zero-based contiguous index, code
zero and the skill name. END has index equal to the row count, code zero and
empty text. ERROR uses that count and a nonzero CAI code, with a bounded literal
reason. A completed catalog may subsequently receive ERROR when its context
closes. A new BEGIN must have a newer revision. Repeated terminal errors,
unannounced revisions, changed source epochs, missing/duplicate rows and rows
after END are invalid. The client accepts at most CAI_SKILLS_MAX_PACKAGES
(256) names, each at most CAI_SKILL_NAME_MAX (64) bytes. Error text is at most
191 bytes; strings cannot contain NUL.

The codec derives each row's size from its fields and emits tokens directly.
Its decoder retains one bounded row; provisional bytes do not invoke a consumer.
The owner emits one row per credited event-loop step using the existing public
immutable catalog visitor. Its early-stop lookup is bounded by the runtime's
256-entry cap. There is no catalog copy on the owner, resource read, model call,
worker, SDK client or idle polling timer. Once END is sent, catalog work stops
until another runtime context is bound. Unbind invalidates the old revision
before releasing the borrowed runtime.

The native terminal explicitly retains a bounded candidate name list for
Softline's completion provider. It becomes available only at END. Context
replacement invalidates the previous list. Names containing terminal controls
or invalid UTF-8 are omitted from completion suggestions. `$` is added only to
the editor's literal candidate; Softline owns removing that prefix on submission.
Filesystem completion still uses the selected workspace. Neither path scans
for skills in the frontend. Parked workspaces retain their own catalog/editor.

Tests exercise bytewise CBOR fragmentation, size limits, cancellation before
completion, source/revision/index ordering, duplicate terminal events, callback
refusal and publication of a replacement/empty catalog. The socket and public
terminal PTY tests load 256 skills, replace the client-visible directory with a
decoy, and verify that `$` still resolves the owner's original names. They also
exercise file completion, prefix removal and completion during generation. The
embedding host aborts if the frontend opens an SDK client and runs on a default
worker stack, including musl's 128 KiB stack. The codec and typed-client fixtures
also compile and link with core libcai alone, without application, SDK storage,
Softline or libmdf objects.
