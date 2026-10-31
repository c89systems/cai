# CR: collection-level leases and explicit ownership takeover

Status: implemented upstream in liblockdc 0.25.0; CAI integration is documented
in [the consumer upgrade notes](../docs/liblockdc-0.25.0-upgrade.md).
Date: 2026-10-29.
Target: liblockdc, initially its native Pouch collections implementation, with
public C and Lua API parity.
Reference baseline: liblockdc 0.24.0 public collection and state-lease APIs.

## Problem and desired result

Two CAI processes can currently resume the same conversation. Both restore the
same journal tail and independently choose subsequent event sequence numbers.
When their records differ, an append fails with:

> append ID is bound to different compact JSON bytes

The append-identity check is doing its job. The missing application invariant is
exclusive ownership of that conversation's collection before restoring its
writable runtime.

Collections already serialize individual storage mutations, but their public
API permits concurrent appenders. Opening a receiver does not acquire an
exclusive collection lease. Ordinary state leases cannot address collection
entries. CAI's current shared session-use guard prevents deletion, not a second
runtime opening the same conversation.

Add an actual lease on the collection itself, following the ordinary state
lease model: acquire it once, own the collection while the lease is valid,
keep it alive, then release it. A separate explicit takeover operation permits
recovery without waiting for a crashed owner's long lease to expire.

Lease ownership and rejection of operations from a replaced owner are distinct
requirements. Both are needed; internal operation locks alone provide neither
conversation ownership nor safe ownership takeover.

## Required ownership semantics

- Lease identity is bound to the namespace and immutable collection
  incarnation, not merely its reusable name or an entry position.
- At most one valid writer lease exists for a collection.
- Normal acquire must reject or use the ordinary bounded blocking behavior
  when another lease is held. Matching owner labels do not grant shared
  ownership: two processes both calling themselves "cai" remain distinct
  acquisition attempts.
- Require the current lease for user-initiated append, entry deletion,
  entry-retention changes, collection metadata changes and collection drop.
  Existing mutator entry points must not bypass that requirement.
- Public describe, list, range reads, filtered reads and append-status lookup
  remain available without acquiring the writer lease, subject to ordinary
  endpoint access controls.
- Reading must retain the existing captured-boundary, ordering and streaming
  contracts. A writer lease is not an exclusive read lock.
- Keep existing expiry and keepalive conventions where applicable. A long-lived
  conversation can retain ownership through renewal rather than acquiring and
  releasing a lease for every event.
- Local handle close and durable lease release must remain distinct, following
  the normal SDK ownership convention. Document failure and retry behavior.
- Collection creation/initial acquisition needs a coherent atomic path so an
  initializer can acquire the new collection without another writer winning
  between initialization steps.

The proposed cutover makes collection mutations lease-bound rather than adding
a permanently parallel unfenced mutation API. The upstream implementation
should deliberately review existing multiwriter collection consumers.
The API spelling and receiver layout remain design choices.

## Append publication must remain independent of release

This is the essential difference from treating the entire open conversation
as one staged state-object transaction.

Each successful append must publish and persist its entry independently under
the configured durability policy, while the collection lease remains held.
Public readers can observe that committed entry before the lease is released.

Releasing, expiring or replacing the lease must not roll back committed
entries. Do not accumulate the conversation in an implicit transaction that
only commits when the lease is released. A crash after an hour of appends must
not discard that hour of successfully committed history.

An unfinished append is different: it must not publish a partial entry, and
its writer must fail if ownership is lost before publication. If a release API
offers a rollback option, its collection meaning must be explicit; it cannot
undo already committed appends.

Retain existing append identities across lease changes. An append ID remains
bound to compact JSON bytes within the collection incarnation, not within a
lease. Do not work around CAI's collision by prefixing event IDs with each new
lease identity.

## Explicit force acquire / takeover

Provide a deliberate operation that replaces the current collection lease
without waiting for its TTL. Normal acquire must not silently perform it.

Recommended contract:

1. Inspect the collection's current lease identity and owner.
2. Request replacement conditioned on that observed lease identity.
3. Atomically replace that ownership with a fresh lease identity and the SDK's
   corresponding acquisition token.
4. Return the new owned lease only after the ownership change has succeeded
   under the selected durability policy.

The condition prevents two recovery attempts based on the same observation
from each replacing the other's newly acquired lease. A changed observation
returns a structured precondition/conflict error; callers must reassess.

After successful replacement:

- The old lease cannot append, change metadata, delete entries or drop the
  collection.
- Its keepalive and release cannot revive it or clear the replacement lease.
- A pending pull or push append from the old owner cannot subsequently publish.
- A stale receiver cannot obtain authority merely because its owner label
  matches the replacement's label.
- Dropping and recreating the collection does not make any old lease valid
  against the replacement incarnation.

Use existing SDK lease identity and token conventions where appropriate.
Validation must be atomic with the mutation's publication, not just a check
before a potentially long upload. This is protection against obsolete lease
credentials; it is not a replacement for the requested collection lease.

Force acquire does not establish that a process has died. It is a powerful,
explicit ownership change. Document its authority requirements and expose
enough lease metadata for a caller to make that decision without logging
credentials. CAI must not automatically steal any occupied collection on
startup. Pouch may support automatic recovery where it can reliably establish
that the old owner is gone; otherwise recovery needs an explicit takeover
decision or normal lease expiry.

## Streaming, concurrency and recovery

Preserve real streaming for both pull append and push-fed writers:

- Use bounded transport/parser/storage buffers. Do not materialize or spool the
  entire payload as a hidden substitute for streaming.
- Do not hold a collection-wide physical mutation lock throughout the lease,
  throughout a user callback, or between push-writer calls.
- Check ownership at append admission and at the publication boundary. Release,
  expiry, takeover and publication must have a defined serialization order.
  If publication wins, the committed record survives; if ownership loss wins,
  the obsolete writer cannot publish.
- A rejected obsolete append must not bind an append ID to new bytes. Existing
  permitted position gaps need not be eliminated.
- Keep append-status lookup usable after ownership loss so an interrupted
  caller can resolve an uncertain outcome without another append.
- An identical retry with a valid replacement lease preserves the original
  position and existing retention deadlines under current idempotency rules.
  An obsolete lease is not a valid mutation credential even for a duplicate.
- Recovery must preserve committed records and append identities, and must not
  resurrect a replaced lease under the selected durability contract.
- Keep collection incarnation, permanent positions, captured reads, retention,
  compression, encryption and compaction invariants intact.

Lease expiry and collection retention are separate clocks. Keepalive must not
implicitly reset the configured inactivity-retention policy. Define the
maintenance interaction explicitly; the proposed default preserves retention,
and removal of a collection invalidates its lease rather than recreating the
collection on a later keepalive or append.

## Public API direction

Prefer the established receiver model and names such as acquire, describe,
keepalive, release and close. Conceptual flow, not proposed C declarations:

```text
collection = client.open_collection(namespace, name)
owned = collection.acquire(owner, ttl, bounded_wait)
owned.append(append_id, JSON_source)
owned.keepalive(ttl)
owned.append(next_append_id, next_JSON_source)
owned.release()
collection.close()
```

Takeover is a separate explicit path using the observed current lease as a
precondition. Lease metadata, owned strings, errors, failed-release handling,
callback safety and receiver lifetimes need the same clear public ownership
rules as the existing state API.

Expose capability discovery for collection leases and takeover. An unsupported
backend must report unsupported behavior rather than silently allowing an
unleased write. Pouch is the initial implementation; current remote collection
support must not be implied by exposing the API. Document the corresponding
future server contract separately if remote implementation remains deferred.

Provide equivalent C and Lua behavior using public headers only. Retain C89
compatibility and supported default thread-stack limits.

## Performance requirements

CAI should acquire once before journal restoration, keep ownership for the
context's lifetime, and release after its runtime and prompt-history writers
have stopped. Children acquire their own conversation collections.

Do not reintroduce the previously measured per-event acquire/load/renew/release
cycle. Ownership checks belong in existing mutation authority; keepalive runs
at its renewal cadence, not on every streamed token.

Lease bookkeeping must be bounded independently of collection length. No full
journal replay, collection scan, growing ownership array, duplicate snapshot,
extra SDK client per receiver, or new worker per append is justified.
Existing process-level client sharing for equal Pouch roots must still work.

## Acceptance and regression coverage

Use fast native tests with controlled barriers and clocks, not real TTL sleeps
or model inference. Include the public C and Lua paths.

| Scenario | Required result |
| --- | --- |
| Two writers acquire the same collection, including equal owner labels | Exactly one owns it; the second gets a structured held-lease result |
| Different collections share one Pouch backend/client | Independent owners can work normally |
| A public reader/export runs during a writer lease | Committed entries remain readable in order |
| Valid keepalive and release | Follow normal lease lifecycle; release does not roll back history |
| Expiry or release followed by a new acquire | New owner succeeds; obsolete credentials fail |
| Two conditional takeover attempts observe the same old lease | One replacement succeeds; the other fails its condition |
| Old owner mutates, renews or releases after takeover | Rejected without affecting the new owner or stored content |
| Lease credentials from another namespace or collection are supplied | Rejected; matching owner labels or append IDs do not transfer authority |
| Pull/push upload pauses while ownership is replaced | Old upload cannot publish after resuming |
| Publication races ownership replacement | A defined order preserves committed-before-replacement entries and rejects later obsolete publication |
| Process dies with a long lease and committed history | Explicit recovery does not wait for the TTL or lose committed entries |
| Process dies around takeover/publication acknowledgement | Recovery and status resolve outcomes under the configured durability policy |
| Append retry spans lease replacement | Existing compact-byte binding, positions and retention remain correct |
| Collection is dropped/recreated or expires | Old handles and credentials cannot address the new incarnation |
| Large streamed entry and substantial existing history | Bounded memory and no per-append history traversal |
| Two CAI applications resume the same session | Second writer is rejected before writable replay/activation; first continues without append-ID collisions |

Benchmark the existing append path before/after with matching durability,
compression and encryption, including one writer and several writers owning
different collections. Report operation counts and CPU as well as elapsed time;
do not hide the cost in extra threads or relaxed durability.

## Delivery boundary

The SDK feature and CAI consumer integration are separate deliverables.
The CAI duplicate-session bug remains open until that integration lands.

Once the SDK is available, CAI can adopt native collection leases rather than
adding a separate collection-writer lock protocol. Acquisition must precede
restoring writable runtime state, and a new owner must restore the authoritative
journal tail before allocating event IDs. Ownership takeover cannot merge or
repair an already stale in-memory conversation.
On ownership loss, CAI must stop further model/tool dispatch and report the
loss. Taking over storage ownership does not kill the previous process or undo
external tool side effects that have already occurred.

Review public C ABI changes against the latest released ABI and make the
required single-step ABI bump when layouts or other ABI commitments break.
Do not tie storage/wire format markers to a release number or add compatibility
layers merely because this pre-1.0 implementation changes.
