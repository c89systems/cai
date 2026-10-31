# liblockdc 0.25.0 integration

CAI consumes the published, checksum-pinned liblockdc 0.25.0 core SDK for all
six Linux targets. The optional PostgreSQL provider remains excluded. This
release changes liblockdc's public collection layouts and advances its shared
ABI from 7 to 8; application consumers must rebuild against the new SDK.
CAI's public C/Lua application interfaces and journal record format are unchanged.

## Existing stores

Stop every older client using a root before its first 0.25.0 open. The SDK
upgrades collection control heads from 128 to 384 bytes under exclusive root
access. It preserves collection identities, entries, positions, retention and
payload bytes. A busy root fails with `collection_upgrade_busy`; close the
other instances and retry. Interrupted upgrades resume on a later open.
Do not downgrade or run a 0.24.x binary against an upgraded root.

CAI adds no compatibility reader, conversion middleware, full-history rewrite,
or application-format version increment. Older snapshot-backed CAI journals
remain unsupported; this SDK upgrade concerns existing ordered collections.

## Ownership and recovery

Writable restoration acquires the conversation collection before loading model
context or allocating subsequent event IDs. Creation establishes initial
ownership atomically. The returned credentialed receiver remains open for
commits/events through that application conversation and its child journal
contexts. Prompt history uses its own leased receiver. Retention changes,
imports and deletion also use owned receivers. Committed reads, listing and
export remain public; reader handles are separate from mutation authority.

CAI holds twenty-four-hour leases, released when the context closes. One
store-context worker renews its held leases every twenty-three hours, leaving
an hour of expiry margin without frequent ownership writes. It runs independently
of the journal mutex, UI, provider waits and tool execution. Renewals update
ownership only. They do not
rewrite history, renew collection retention, create another SDK client, or
acquire a state-object lease per event. Writers stop before release/close.
UI health snapshots use a separate short mutex, so a slow renewal does not hold
their read lock while waiting for storage I/O.

The SDK lease is the write authority. A private native liveness guard is held
for its complete lifetime, with a persistent random owner label established
before acquisition. After a process crash, a new CAI instance can hold that
guard, observe its own abandoned lease and conditionally take over its exact
lease ID. A live guard owner is rejected immediately. A foreign SDK owner,
including one using the same application name, is not automatically replaced.
There is no automatic live-owner takeover through `--force`.

If the liveness marker is damaged, CAI does not guess ownership: a live lease
must expire/release first. Once unleased, the marker can be regenerated. Failed
SDK release closes the local receiver/guard and leaves the durable ownership
recoverable by the next context. Already committed appends remain committed.

Renewal failure becomes a sticky context error and wakes the interactive or
batch owner. Further journal writes fail; the application cancels work and
unwinds. Stale SDK credentials cannot release or overwrite a replacement lease.
CAI never silently reacquires underneath an already-restored runtime.

## Append API

CAI retains direct pull append for its finite generated JSON records. Every
source is ready when called; none waits for future model output. The bounded
generator, adapter and SDK encoder remain streaming end to end.

The new SDK push/staged-pull paths now pack entries into shared rolling segments
and can be useful for producers waiting on external input. They stage encoded
bytes before publication; switching CAI's ready finite generators to them would
add an unnecessary staging/copy step. Lease checks apply to all append forms.

## Verification

`cai_cli_collection_leases` checks same-client sharing, duplicate writer
rejection before writable restore, public reads, separate child ownership,
renewal from a default-stack worker with the journal mutex held, forced
takeover, stale writes/releases, wakeup/error propagation, foreign owners,
damaged markers, failed release and immediate SIGKILL recovery. It uses
barriers/direct renewal rather than sleeping out lease TTLs. It verifies the
stored twenty-four-hour expiry and observes the real worker's twenty-three-hour
wait deadline without waiting for either interval.

`cai_cli_collection_ownership` and `cai_app_lua_collection_ownership` exercise
the real C and Lua applications against loopback provider fixtures: a second
writer is refused before inference, another conversation and export still work,
SIGKILL is followed by immediate resume/continued inference, and normal shutdown
permits another reopen. No live user root or paid inference is required.
Existing journal, retention, prompt recall, deletion, compaction, embedding and
responsiveness regressions cover the surrounding storage/runtime paths.

`cai_app_worker_stack` also runs application startup, inference, resume, export,
status/quit and temporary mode on a 128 KiB glibc worker and a default-attribute
musl worker. This exposed the application entry aggregate and nested Markdown
export scratch on the stack; those now live on the heap. Export retains bounded
reader/parser scratch rather than buffering complete records or history.
