# SHOAL -- a proposed content-addressed replication network over smolvfs

Status: **proposed design. Not implemented.** This is a design
proposal: it captures a design conversation and the decisions reached,
not a commitment to build. Nothing here exists in code yet, and the
details will move as work starts.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## What this is

A confederation of trusted servers that share immutable, content-addressed
objects and a small set of mutable, signed named topics. Any object is
reachable by its hash; if a node lacks it, the object is fetched from a
peer into local smolvfs storage and verified against its address. Named
topics are published by a single writer, signed and versioned, and
propagated through the network so subscribers can follow them.

This is the same family as IPFS, Dat/Hypercore, Perkeep, and Tahoe-LAFS:
a content-addressed peer-to-peer store with DHT content routing and a
mutable naming layer. "Replication" describes only the convergence
property (nodes trend toward holding more of the state); the system as a
whole is content-addressed distributed storage with self-certifying
names.

smolvfs is the storage and format substrate. shoal is the network layer
on top of it. The two stay cleanly separated: smolvfs owns immutable
content and local bookkeeping; shoal owns peers, routing, signing, and
propagation.

## Goals

- Fetch any object by hash, from local storage or a peer, verified.
- Offline-first: a node runs and stores independently, then makes its
  content reachable when it joins the confederation.
- Follow signed, versioned named topics (single writer per topic).
- Share large immutable content (assets: models, textures, audio,
  scripts) efficiently, with dedup across versions and variants.
- Lazy / partial fetch: hold an index (a tree) of content you do not
  have, and fetch on demand.

## Non-goals (for now)

- Multi-writer topics. Single-writer per topic is chosen deliberately.
- A redundancy-reduction algorithm. Nodes may converge toward full
  copies; trimming that is a later, optional concern.
- Deletion / retraction of already-replicated content (see Open
  questions).

## Motivating use cases

- **Game shards.** A shard runs offline with its clients, storing state
  and assets in local smolvfs. A client migrates to another shard,
  carrying the hashes of important state out of band. The new shard
  fetches that state from the player's old shard (a direct provider
  hint), verified by hash.
- **Asset sharing.** 3D models, textures, music, and scripts are
  immutable content shared across clients and shards; identical content
  (and, with chunking, identical parts) is stored and transferred once.

## Layering

```
  application (game shard, client, asset pipeline)
  ------------------------------------------------
  shoal network layer
    - peer transport, membership, K-closest routing
    - signing / verification of topic heads
    - fetch orchestration (provider hints, chunk swarming)
    - chunking (content-defined) and file reassembly
  ------------------------------------------------
  smolvfs
    - content-addressed objects (immutable, self-verifying)
    - trees / htrees, refs (local head + history), packs
    - local gc
```

A key invariant of the layering: the **content address** is smolvfs's
(`BLAKE2b("type len\0" || plaintext)`), immutable and independent of the
network. The **placement key** used for routing is a separate function
computed only in the network layer. smolvfs never sees routing state, so
routing choices (salts, resharding) never rewrite stored objects.

## Decisions

### D1. Content layer is smolvfs CAS

Objects are immutable and self-verifying by BLAKE2b address. `get(hash)`
= local (`cas_exists` / `cas_open`) if present, else fetch from a peer,
verify against the hash, store (`cas_put_object_at` / import), return. A
faulty or malicious peer cannot poison content: the address is the
integrity check. Trees and htrees let a node hold an index of child
hashes without the children, enabling lazy / partial fetch.

### D2. Topics are single-writer, signed, versioned; self-certifying names

One writer per topic. The namespace is `server-id/topic` where
**server-id = hash(identity public key)**, so the name binds to the key
authorized to write it -- no key registry is needed to verify topic
authenticity (self-certifying names, per SFS/IPNS). External bootstrap
(finding a server's address to start) uses HTTPS/DNS out of band; that
is orthogonal to the self-certifying content id.

### D3. Version records are first-class content-addressed objects

Each topic generation is a signed object (a commit-like record):

```
{ topic-id, seq, root_hash, prev_record_hash, timestamp, signature }
```

stored via CAS like any other object. The **topic head is the hash of
the latest record** -- the entire mutable surface per topic is one hash.
`prev_record_hash` forms an immutable, signed history chain that
distributes and verifies like any content. `seq` plus the prev-links
give a signed total order, so subscribers reject stale or rolled-back
heads. For an honest single writer a fork is impossible, so the
existence of two records at the same `seq` under one id is evidence of
key compromise. Fetch verifies the record's hash (it is content), then
its signature and monotonicity.

### D4. Head cache reuses smolvfs local refs

A node stores the current head per topic in a local smolvfs ref (name =
`topic`, value = head-record hash). The existing `.root` / `.log` /
`.prev` machinery then provides the local head, an update log, and
crash-recovery rollback for free. (Implementation note: `server-id/topic`
contains a `/`; confirm `valid_ref_name` accepts it, or encode the id.)
The immutable version chain is the network-shared history; the local ref
is just the current-head cache.

### D5. Signing and key management

Ed25519 (small, deterministic, vendorable public-domain implementation,
matching smolvfs's bundle-or-BYO pattern). All signing lives above
smolvfs, which only stores the bytes. Self-certifying ids remove most of
the key-distribution problem for topic authenticity. TLS-peer vouching
and any "voting keys in" apply only to two narrower, separable concerns:
admission (who is in the confederation -- a consensus/quorum problem,
cheap if the set is known via allowlist or threshold-signed admission)
and friendly-name aliases. **Key rotation** uses a hierarchy: `id =
hash(long-lived identity key)`; the identity key signs short-lived
signing keys via a delegation record in the version chain, so rotation
does not change the topic id.

### D6. Content location: K-closest placement

Place each object on the **K nodes closest** to its placement key in the
routing metric (Kademlia-style), not on a single owner. This gives churn
resilience (minimal reshuffling on join/leave with consistent /
rendezvous hashing), anti-targeting (grinding into one bucket still hits
K nodes), and durability (K holders) at once. **Ask-neighbor with a TTL**
is the fallback during membership churn when ranges are transiently
reassigned. A gossip / SWIM-style membership view tracks who is in and
their ranges.

### D7. Placement key vs content address are separate

The placement key is `H(salt || content_hash)`, computed only in the
routing layer. A network-wide (optionally epoch-rotated) salt provides
anti-grinding decorrelation. Because the salt never touches the content
address, storage is unaffected and the network can re-salt or reshard
without rewriting a single stored object. A fixed public salt only
decorrelates; K-closest placement (D6) is what actually blunts targeted
grinding, and an epoch-rotated salt moves the target.

### D8. Replication: provider index + policy replication

Bucket / K-closest owners maintain a **provider index** ("node N holds
hash X") -- complete and cheap -- rather than storing the bytes for their
whole key range. Byte replication is a separate policy: maintain K copies
and pin the closures of subscribed topics; owners may cache-through
popular content. This scales for large assets far better than mirroring
every object into its range owners. **Direct provider hints** are a
first-class fetch path: a migrating client carries hashes plus the
knowledge that its old shard holds them, so the new shard fetches
directly and verifies, using DHT lookup only as a fallback.

### D9. Chunking for large content

Large files are stored content-defined-chunked, reusing the same
"canonical uncompressed form is the address" mechanism as htree and
compression:

- Canonical address `A = BLAKE2b("blob" len\0 || whole_plaintext)`.
- At `A`, store a **manifest** object (`type = "chunked"`) listing
  `(chunk_hash, chunk_len)` per chunk. Each chunk is an ordinary
  self-addressed blob.
- `chunked` becomes the third **re-encoded** type alongside `htree` and
  compressed objects: the stored bytes are not what hashes to `A`, so it
  joins `cas_type_is_reencoded`, and fsck / import / pack treat it like
  htree. Unlike htree it is fully verifiable by reassembly (concatenate
  chunks, hash, compare to `A`).
- Use **content-defined chunking** (rolling hash / FastCDC), not
  fixed-size, so a small edit changes only nearby chunks and everything
  else dedups across versions and variants.
- Reassembly is **transparent by default**: the file-read API resolves a
  chunked manifest into a continuous byte stream, fetching missing chunks
  as needed, so a caller reading a file never has to know it was chunked.
  This keeps the fetch-on-miss for a chunk out of the low-level object
  store (`cas_open` still yields the manifest bytes for a chunked
  address, unchanged) and in the file layer where network access belongs.
- Alongside the transparent reader, expose a **low-level chunk-index
  API**: given a chunked address, return the ordered `(chunk_hash,
  chunk_len)` list (and, for multi-level manifests, walk the manifest
  tree) without reassembling. Protocol work needs this directly to decide
  which chunks to fetch or advertise, compute what a peer is missing, and
  drive parallel multi-source fetch.
- Per-chunk lengths in the manifest enable **ranged reads** (fetch only
  the chunks covering a byte range). For very large files, make the
  manifest a **multi-level tree** of manifests.
- Chunks are placed independently by their own address, so a file's
  chunks scatter across the network and can be fetched in **parallel
  from many providers**; a migrating client fetches only the chunks the
  new shard lacks.

### D10. GC with partial state

A node keeps its subscribed roots and discards anything unreachable from
them, even when it holds only part of a root's graph. This needs:

- The reachability walk must **tolerate missing children**: a tree /
  htree / chunked node that is absent means "reachable but absent -- stop
  descending, do not error." (As written, `cas_tree_gc` aborts the whole
  GC when a referenced tree object cannot be loaded; this must change.)
- The walk must descend all container types -- `tree`, `htree`, and
  `chunked` manifests -- ideally via one "given an object, yield its
  child hashes" dispatch so new container types are one case.
- Keep a **grace period** (smolvfs's `cas_tree_gc` grace already spares
  objects newer than the cutoff) and a **pending / wanted pin set**
  (objects a known tree references but that are not yet local, plus
  in-flight fetches) so an object being downloaded is not swept.
- Prefer **spine-first fetch** (whole tree / htree / manifest graph
  before non-container content) so reachability is always computable and
  a child rarely arrives before its parent.

## Required smolvfs changes

Most of shoal is above smolvfs. The base needs:

- `cas_tree_gc`: tolerate missing tree/subtree during the reachability
  walk (currently aborts) and descend `htree` (already) and the new
  `chunked` manifests; a "wanted/pending pin" concept.
- Add `"chunked"` to `cas_type_is_reencoded` and handle it in fsck /
  import / pack as a re-encoded type.
- Confirm `valid_ref_name` accepts `server-id/topic` (a `/` in the name)
  or define an encoding.

Chunking (content-defined split + manifest build/reassembly), signing,
and all networking live in the shoal layer, not in smolvfs.

## What smolvfs already provides

No change needed for: content addressing and BLAKE2b verification;
automatic dedup; Merkle trees (`tree` / `htree`) for indices and lazy
fetch; local refs with head, history, and rollback (`.root` / `.log` /
`.prev`); the re-encoded-object model (htree is the precedent for
chunked); optional compression; packs / bundles; the incremental,
verify-on-fetch download protocol (see DOWNLOAD.md); atomic,
concurrent-safe object writes.

## Open questions

- **Unpublish / retraction.** Immutable, replicated content has no cheap
  delete; a tombstone plus cooperative-purge protocol would be needed.
- **Storage sizing.** A node's footprint is its provider/replica
  obligations plus the closures of topics it subscribes to; lazy fetch
  bounds the latter, replication policy the former.
- **Admission / membership consensus.** How servers join the
  confederation and how (if at all) keys are voted in; Sybil resistance.
- **CDC target chunk size.** One network-wide average (maximizes
  cross-producer chunk dedup) vs per-content-class.
- **Manifest shape in v1.** Include per-chunk lengths for ranged reads
  from the start (cheap now, hard to retrofit); single-level vs
  multi-level threshold.

## Prior art referenced

IPFS (content routing / provider records; IPNS self-certifying names;
UnixFS chunking), Dat / Hypercore (signed append-only logs -> the version
chain), Perkeep (content-addressed store), Tahoe-LAFS (capabilities),
casync/desync, bup, FastCDC (content-defined chunking), Kademlia
(K-closest routing), SWIM (membership), git (commit / tree / ref model).
