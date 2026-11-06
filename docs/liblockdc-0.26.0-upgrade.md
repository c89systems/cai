# liblockdc 0.26.0 integration

Historical integration notes.
The current dependency is documented in [the 0.28.0 integration notes](liblockdc-0.28.0-upgrade.md).

CAI consumes the published, checksum-pinned liblockdc 0.26.0 core SDK for all
six Linux targets. PostgreSQL remains excluded. The SDK advances its shared
ABI from 8 to 9; consumers must rebuild against matching headers and libraries.
This dependency upgrade does not itself change CAI's public ABI or journal
format. Existing append/read calls retain their behavior with initialized
options and no selected edit/head preconditions.

## Existing stores

Existing 0.25.x Pouch roots need no conversion or entry-body rewrite. Once a
collection contains a new atomic edit, older clients cannot replay it: upgrade
all clients before edits are used, and do not downgrade or mix older binaries
against an edited collection. For roots older than 0.25.0, the separate
[control-head upgrade](liblockdc-0.25.0-upgrade.md) still applies.

Collection leases, conditional abandoned-owner recovery, shared-root client
reuse, compression and the twenty-four-hour ownership policy remain unchanged.
Atomic edits preserve ownership rather than requiring per-entry lease churn.

## Newly available storage operations

- `head` returns one coherent collection identity, mutation token, counts and
  recovery descriptor, without reading entry bodies.
- `edit` atomically removes up to 64 sorted, disjoint position ranges and may
  append one streaming replacement plus a recovery descriptor and anchor.
- `edit_status` reconciles committed operation outcomes without scanning entries.
  Retry receipts survive later appends, deletion and compaction.
- Expected-head read admission rejects a stale observation before body delivery.
  Already admitted readers retain their pinned view across edits.

Recovery text is bounded to 1024 UTF-8 bytes; the SDK can bind its anchor to
the replacement entry without requiring the caller to guess that position.
The payload remains application-defined, not a full-context snapshot.
These APIs are Pouch-only; remote collection capabilities remain unsupported.

The SDK also reuses bounded decoder/key/binding contexts, coalesces encrypted
frame reads and output, reduces edit metadata work and removes the fixed
two-millisecond wait before ready durable-sync groups are dispatched. It keeps
independent authentication, streaming backpressure and configured durability.
Those are upstream implementation changes, not an assumed measured CAI speedup.

The dependency is acquired from published release archives through the normal
cache and hash checks. Adjacent source checkouts and their build artifacts are
not used as dependency inputs.

## Upgrade verification

The native Debug application and static x86_64 musl executable build against
the published SDK. Eight focused native checks pass: journal import/export,
current trim/replay behavior, Pouch startup/recovery, collection leases and
writer ownership, replay limits, prompt recall and application worker-stack
operation. The matching musl worker-stack and collection-lease checks also pass.
The SDK reports version 0.26.0 and exports shared ABI 9. These checks establish
the dependency cutover; the atomic trim implementation has its own verification.
