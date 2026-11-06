# liblockdc 0.28.0 integration

CAI pins the published [c89systems/liblockdc 0.28.0](https://github.com/c89systems/liblockdc/releases/tag/v0.28.0) core SDK for all six Linux targets.
SHA-256 pins match the release CHECKSUMS file and GitHub's asset digests.
The optional PostgreSQL provider remains excluded.

## Storage and ABI

The public C API and layouts retain core ABI 9.
This dependency update changes neither CAI's ABI nor its journal schema, collection ownership, retention or compression defaults.
Existing contiguous collection entries remain readable without rewriting their payloads.

Large push and staged-pull appends can now publish chunk/index record kinds that older liblockdc releases cannot read.
Upgrade every client sharing a Pouch root before using these paths, and do not downgrade after writing chunked entries.
This restriction concerns the native store format, independently of the unchanged C ABI and portable CAI JSONL format.

## Streaming and performance

Push writers now encode bounded chunks directly into shared canonical segments and publish the completed entry through metadata.
They use no whole-entry buffer, spool, commit-time payload copy or per-entry segment inode.
`append_staged` retains its public name but now feeds that same bounded writer from a single-pass source.
The earlier workspace push-streaming blocker is therefore resolved.
See the upstream [incremental collection append contract](https://github.com/c89systems/liblockdc/blob/v0.28.0/docs/collection-writer.md).

CAI retains direct pull for finite journal records whose source data is already available.
Upstream still recommends that path for fast inputs; this upgrade does not introduce another append engine or switch storage formats unnecessarily.
Externally paced producers can now use the public push/staged-pull APIs without hidden full-entry staging.

Upstream also bounds chunked-record reads, recovery, compaction and compression conversion.
Large reads use up to 64 KiB of heap scratch; small reads retain 16 KiB.
Uncompressed decryption avoids an intermediate plaintext copy, and operations reuse authentication setup and manifest bindings.
These are upstream implementation changes, not a measured CAI speedup.

## Verification

The native Debug build passed 607 normal unit/local checks and 25 focused storage/application checks.
The rebuilt static x86_64 musl application passed the 24 target-dependent focused checks on default worker stacks.
These cover encrypted journal import/export, compression conversion, recovery, leases, atomic trim, prompt history, compaction, asynchronous exports and both MCP discovery protocols.

`cai_cli_chunked_journal` writes 256 KiB generated payloads through both public push and staged-pull paths, mixed with ordinary direct appends.
It checks identical retries across append paths, abandoned writers, unchanged publication bounds, reuse of an abandoned identity, exact streamed native projection and reopening with compression converted in both directions.
The bounded recovery API retains its oversized-event omission notices; the native projection still delivers all original text.
The check runs on an ordinary pthread, including musl's default 128 KiB stack.

Storage-diagnostic fixtures now inspect newly appended owner-log records rather than expecting server diagnostics in terminal output.
The initial assertion failures also reproduced against the previous musl binary.
Their synthetic Markdown replies now end their paragraphs, and exported continuation checks use the current `input_presented` event.
The Lua source-package fixture includes the current frontend/Lua inputs and verifies that these upgrade notes are shipped.

Both application dependency manifests identify 0.28.0 and its target-specific checksum.
Published native headers and shared metadata retain ABI 9 and `liblockdc.so.9`; the SDK's shared runpath is `$ORIGIN`.
All six Linux archive pins match the published checksums and asset digests; only native x86_64 and static x86_64 musl were built and exercised here.
Live-provider and full cross-target release checks remain outside this upgrade's verification scope.
