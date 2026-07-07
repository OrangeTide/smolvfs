# smolvfs incremental download protocol

This describes how a client fetches a snapshot from a remote depot over
HTTP while downloading only the objects it does not already hold, in the
spirit of a git fetch. It targets static hosting: the server is a plain
HTTP origin or CDN serving files, with no smolvfs-specific logic. All
negotiation happens on the client, which is possible because objects are
content-addressed and the client can test locally what it already has.

Byte layouts referenced here (objects, trailers, the packfile index and
footer, tree and htree directories) are specified in
[FORMAT.md](FORMAT.md); this document does not restate them.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Goals

- Fetch only the objects a client lacks when moving to a new snapshot.
- Run against static hosting (S3, a CDN, any file server), needing only
  HTTP `GET` and, for the pack transport, byte-range requests.
- Verify every fetched object against its address, so the transport and
  the origin's cache need not be trusted for integrity.
- Report download progress by byte count, exactly when possible.
- Resume after interruption without refetching completed objects.

## Non-goals

- No server-side negotiation, delta compression, or dynamic pack
  assembly. Those belong to a later smart-server extension.
- No object mutation or upload. This is a read/fetch protocol.
- No transport-level authenticity of object bytes: integrity comes from
  content addressing (see Trust). Ref authenticity is the trust anchor
  and is the caller's responsibility (fetch refs over TLS from an origin
  you trust).

## Server layout

A published depot is a set of static files under a base URL. Two layouts
are supported; a server may offer either or both.

```
<base>/refs/<name>.root         text file: 64 hex chars, the root address
<base>/<xx>/<hash>              one loose object (data region + trailer)
<base>/packs/<name>.pack        one packfile
```

- `<xx>` is the first two hex characters of the address. This layout is
  exactly a local depot directory, so publishing a depot for the
  loose-object transport is serving that directory as static files.
- A loose object file is byte-identical to its local form: the data
  region followed by the 64-byte trailer.
- A packfile is byte-identical to a local `pack.dat`. The pack transport
  needs the origin to honor `Range` requests (`Accept-Ranges: bytes`).

Content-addressed objects and packs are immutable and should be served
with a long-lived immutable cache policy. Refs are mutable and should be
served with revalidation:

```
objects, packs:  Cache-Control: public, max-age=31536000, immutable
refs:            Cache-Control: no-cache        (validate with ETag)
```

## The core idea

A client updating to a snapshot walks the snapshot's tree from its root
and, for each object it reaches, fetches the object only if it is not
already in the local depot. The "have" set is never sent to the server;
the client resolves it locally with `cas_exists`, because an object's
address is a hash of its content. Structural sharing does the rest: an
unchanged subdirectory keeps its address across snapshots, so the walk
skips its entire subtree on an update.

This yields git-style minimal fetching without any server participation.

## Discovery

The client obtains the root address by fetching a ref's `.root` file
(the atomically-updated current value; the sibling `.log`, `.prev`, and
`.lock` files are local depot state and are not needed remotely):

```
GET <base>/refs/<name>.root   ->  8eb26db6...  (64 hex + newline)
```

The root address is the entry point for the walk and the anchor of the
snapshot. On an update the client keeps the previous root's objects, so
the walk from the new root descends only into changed subtrees.

## Baseline transport: loose objects

The simplest deployment publishes every object as a loose file. No range
support is required.

1. Fetch the ref to get the root address.
2. Maintain a work queue seeded with the root address.
3. For each address in the queue:
   - If `cas_exists(address)` locally, skip it.
   - Otherwise `GET <base>/<xx>/<address>`, verify it (see
     Trust), and write it into the local depot atomically.
   - If the object is a directory (`tree` or `htree`), parse it and
     enqueue every child address.
4. When the queue drains, every object reachable from the root is local.
   Record the ref locally as the final step.

Because directories are fetched before their children, the walk
discovers the reachable set incrementally. Fetches for independent
objects may run concurrently; HTTP/2 or HTTP/3 multiplexing keeps the
per-object request overhead low.

The cost of this transport is one request per missing object. It suits
small or highly incremental updates and hosting that cannot serve byte
ranges.

## Scaled transport: packfile with range requests

When a snapshot is published as a single packfile, the client avoids
per-object requests by reading the pack's index once and then fetching
only the byte ranges of the objects it needs. A large pack is never
downloaded whole.

### Step 1: fetch the index

The index and footer sit at the tail of the pack (see FORMAT.md). The
client reads them with two range requests:

```
GET <base>/packs/<name>.pack   Range: bytes=-64
   -> footer (64 bytes); Content-Range gives the total pack size Z
```

Parse `entry_count` from the footer, compute `index_size = entry_count *
64`, then:

```
GET <base>/packs/<name>.pack   Range: bytes=(Z-64-index_size)-(Z-65)
   -> the full index
```

Verify the footer checksum over `index || footer[0:16]` before trusting
any offset. The client now holds, for every object in the pack, its
address and the absolute offset of its trailer.

### Step 2: derive object extents

Objects are concatenated with no padding: each is its data region
immediately followed by its 64-byte trailer. Sorting the index entries
by `offset` therefore pins every object's exact byte span without
reading any trailer first, even for uncompressed objects whose
`stored_size` is recorded as zero:

```
let O = the trailer offsets, sorted ascending
object at offset O[i] occupies bytes [ end(i-1), O[i] + 64 )
   where end(i-1) = O[i-1] + 64,  and end(-1) = 0
```

The data region is `[end(i-1), O[i])` and the trailer is
`[O[i], O[i]+64)`. This extent is the byte range to request for that
object.

### Step 3: walk and fetch

1. Fetch the ref; seed the queue with the root address.
2. Resolve the reachable set by walking directories. Interior nodes
   (`tree`/`htree`) are small; fetch each missing one by its extent
   (Step 2), verify, store, parse, and enqueue its children.
3. For every reachable object that is missing locally and present in the
   pack index, issue a range request for its extent, verify, and store.
4. Coalesce ranges: contiguous runs of wanted objects become one
   request; scattered wants can be combined with a multi-range request
   (`Range: bytes=a-b,c-d,...`) where the origin supports it, or issued
   as concurrent single-range GETs where it does not (multi-range
   support is uneven across CDNs).
5. Record the ref locally once all reachable objects are present.

### Fallback

If the origin does not advertise `Accept-Ranges: bytes`, the client
falls back to the loose-object transport, or as a last resort downloads
the whole pack and imports it (equivalent to `cas_pack_import`). A client
should prefer loose objects over a whole-pack download when both are
published, since the loose walk still fetches only what is missing.

## Incremental update (A to B)

Updating a cached snapshot A to a newer B uses the same walk from B's
root. Every object shared between A and B is already local and is
skipped by the `cas_exists` test, so the client fetches exactly the
objects unique to B: the changed blobs and the `O(depth)` directory
nodes on their paths to the root. No diff is computed and no manifest of
A is sent; content addressing makes the shared set implicit.

## Progress reporting

Progress is reported against the set of bytes the client must fetch,
which it can size once it knows the reachable set and each object's
length.

- **Packfile transport.** After the index is fetched and directories are
  walked, the client knows the full reachable set and, from the sorted
  offsets, each object's exact byte length. Summing the lengths of the
  reachable-and-missing objects gives an exact byte total, and progress
  is `bytes_fetched / total`. The estimate is exact as soon as the
  interior walk finishes; during that short walk it is a lower bound that
  rises as directories are parsed.
- **Loose transport.** Object lengths are not known in advance. The
  client reports progress by object count (the reachable-and-missing
  count, itself growing during the walk) and refines to bytes using each
  response's `Content-Length` as objects arrive. A `HEAD` per object
  would give byte sizes up front but costs a round trip each and is not
  recommended.

Either way the reachable set is knowable from the tree, as the TODO
anticipated: the root's tree and htree objects enumerate every
descendant address, and the pack index supplies the sizes.

## Trust

Object integrity is self-verifying and does not depend on the transport:

- A `blob` or text `tree`, and a compressed object after decoding, is
  accepted only if `BLAKE2b-256("type len\0" || plaintext)` equals the
  address it was fetched under. A tampered or truncated object is
  rejected.
- An `htree` is addressed by its canonical text form, so it cannot be
  verified by hashing its own bytes; the client validates its internal
  adler32 and, in the packfile transport, relies on the footer checksum
  that covers the index. This mirrors `cas_pack_import`.
- A compressed object is fetched as stored (codec tag plus payload) and
  decoded locally; decoding requires the matching codec to be compiled
  into the client.

The one thing the client must obtain authentically is the ref: it is the
root of the Merkle structure, so whoever controls the ref controls which
tree the client materializes. Fetch refs over TLS from a trusted origin,
or sign them out of band. Everything reachable from a trusted root is
then verified by address.

## Atomicity, resumption, and concurrency

- Each fetched object is written into the local depot atomically
  (temp file plus rename), so a crash never leaves a partial object at a
  valid address.
- The local ref is updated only after every reachable object is present.
  A partial download therefore never exposes a dangling ref; the
  previous snapshot stays intact and mountable.
- Resumption is a re-run of the same walk: objects already fetched pass
  the `cas_exists` test and are skipped, so an interrupted download
  continues from where it stopped with no server-side state.
- Independent object fetches are safe to run in parallel; ordering only
  requires that a directory be fetched before its children are walked.

## Worked example (packfile transport)

A client caches snapshot `main` from `https://cdn.example/depot`.

```
GET /depot/refs/main.root
    -> 8eb26db6ab39...            (root address)

GET /depot/packs/main.pack   Range: bytes=-64
    206; Content-Range: bytes 17240-17303/17304
    -> footer: entry_count = 3

GET /depot/packs/main.pack   Range: bytes=17048-17239
    206  -> index (3 x 64 bytes); footer checksum verifies

    sorted offsets -> extents:
      obj @  71  : data [0,71)      + trailer [71,135)
      obj @ 210  : data [135,210)   + trailer [210,274)
      obj @ ...  : ...

    walk root (an htree at some offset): fetch its extent, verify,
    parse -> children: file.txt (a blob), sub/ (a tree)
    reachable-and-missing = { root, sub, file.txt } ; total = sum(sizes)

GET /depot/packs/main.pack   Range: bytes=0-134,135-273,...
    206 multipart  -> the three object extents; each verified and stored

record ref main -> 8eb26db6ab39...   (only now)
```

## Reference client algorithm

```
fetch_snapshot(base, refname):
    root = GET base/refs/refname.root
    if base advertises byte ranges and a pack is published:
        idx   = fetch_pack_index(base, pack)        # 2 range requests
        verify_footer_checksum(idx)
        exts  = extents_from_sorted_offsets(idx)    # per-address byte span
        want  = walk_reachable(root, idx, exts)     # missing interior + leaves
        total = sum(exts[a].len for a in want)
        for group in coalesce(want, exts):
            body = GET base/pack Range=group.ranges
            for obj in split(body, group):
                verify(obj); store(obj); progress += obj.len
    else:
        queue = [root]
        while queue:
            a = queue.pop()
            if cas_exists(a): continue
            obj = GET base/<xx(a)>/a
            verify(obj); store(obj)
            if is_dir(obj): queue += children(obj)
    record_ref_local(refname, root)                 # last

walk_reachable(root, idx, exts):
    want = set(); queue = [root]
    while queue:
        a = queue.pop()
        if cas_exists(a) or a in want: continue
        if is_dir_by_index(a):          # tree/htree: fetch now, parse, descend
            obj = GET_range(exts[a]); verify(obj); store(obj)
            queue += children(obj)
        want.add(a)
    return want
```

## Reference client

[examples/cas-fetch.c](examples/cas-fetch.c) implements the loose-object
transport described above: it fetches the ref, walks the tree, GETs each
missing object with libcurl, and verifies it with `cas_fsck_object`
before storing it, so a re-run against a newer ref fetches only the
changed objects. It links against the smolvfs library and libcurl.

```sh
make cas-fetch                # add MINIZ=1 for compressed depots
./cas-fetch https://cdn.example/depot main ./cache
```

It fetches whole objects rather than pack byte-ranges, which keeps the
example small while still demonstrating the incremental core. The
packfile transport (range requests, extent coalescing, byte-accurate
progress) is a documented extension left to a fuller client.

## Open questions

- **Multiple packs.** This document assumes one pack per published
  snapshot so the client can range-fetch a single index. Serving many
  packs on static hosting needs a published hash-to-pack index or a
  per-pack index fetch; the local depot deliberately does not support
  multiple packs, so a multi-pack server would target the loose or
  whole-pack import paths.
- **Object-length hint for loose hosting.** Publishing a small manifest
  of address-to-length pairs would give byte-accurate progress without a
  pack. Whether that is worth a non-content-addressed side file is open.
- **Smart-server extension.** A negotiation endpoint that accepts a
  have/want set and returns a tailored pack would cut round trips for
  cold clients, at the cost of server logic. It can be layered on top of
  this model without changing the object formats.
