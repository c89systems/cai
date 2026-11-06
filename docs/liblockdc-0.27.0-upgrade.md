# liblockdc 0.27.0 integration

Historical integration notes; current storage and compatibility details are in the [0.28.0 notes](liblockdc-0.28.0-upgrade.md).

CAI pins the published core SDK from
[c89systems/liblockdc](https://github.com/c89systems/liblockdc/releases/tag/v0.27.0)
for all six Linux targets. SHA-256 pins match both the release CHECKSUMS file
and GitHub's asset digests. PostgreSQL remains excluded.

The core shared-library ABI remains 9. Public C layouts and canonical storage
formats are unchanged from 0.26.0, so this upgrade does not change CAI's ABI,
journal format, collection ownership or retention policy. The existing
[atomic-edit compatibility restrictions](liblockdc-0.26.0-upgrade.md) still
apply to clients predating 0.26.0.

## Storage changes

Upstream fixes obsolete-file cleanup and compaction-candidate views, including
partial failures and retries. Deleted-file directories are synchronized before
obsolete markers are retired, and failures retain candidate path, errno and
phase in diagnostics. This addresses the upstream defect behind CAI's earlier
`failed to open pouch compaction candidate` investigation; CAI adds no
workaround or private storage API dependency.

Explicit compression changes still finish synchronously under exclusive root
ownership before opening returns. Conversion now replaces segments/snapshots
incrementally rather than duplicating the complete retained root. An
interrupted conversion resumes before clients are admitted, even when the
next open omits the compression setting. Temporary payload duplication is
limited to one converted segment or snapshot, plus recovery/index scratch;
decompression still requires space for the final root's growth.

Use the new release until interrupted conversion completes. Correct underlying
space, I/O or integrity failures and reopen; do not delete conversion recovery
metadata. See upstream's
[compression conversion contract](https://github.com/c89systems/liblockdc/blob/v0.27.0/docs/pouch-compression.md).

These are upstream implementation changes, not a claimed measured CAI speedup.
CAI continues to use the SDK's public collection, lease and streaming APIs.

## Verification

The native Debug build passed 534 local/unit checks and 13 targeted
storage/application checks. They cover encrypted journal import/export,
compression conversion in both directions, startup recovery, collection
ownership, atomic trim, prompt recall, replay limits, background compaction
and MCP discovery with either inference protocol. The source-packaging check
also verifies that these notes are present in the unpacked Lua source rock.

The rebuilt static x86_64 musl application passed journal/conversion,
collection lease/trim, background compaction and both MCP discovery fixtures.
The embedding worker check uses musl's default 128 KiB thread stack. Packaged
headers identify version 0.27.0 and ABI 9; native shared metadata exports
`liblockdc.so.9`. Other Linux targets have verified archive pins; cross-target
builds were not run for this upgrade.
