# cas-omap: Sparse Object Map

A two-level sparse page table that maps arbitrary 64-bit numeric IDs
to CAS content hashes.  Designed for workloads where objects cluster
in zones (thousands of entries at arbitrary offsets) with large gaps
between them.

## Use case

A game engine stores objects by numeric ID.  Builders allocate IDs in
zones (e.g. starting at #1200000, #1300000) with 1000-5000 objects per
zone.  The object map tracks which content hash each ID resolves to.
Snapshots capture the full mapping at a point in time. Free-ID
allocation finds the next unused slot in a zone without scanning the
entire ID space.

## Interface

### Lifecycle

```c
struct cas_omap *cas_omap_new(struct cas *store);
void             cas_omap_free(struct cas_omap *om);
struct cas *     cas_omap_cas(struct cas_omap *om);
```

Create an empty map backed by a CAS store.  All page data and the
directory itself are stored as CAS objects.

### Snapshot (load / store)

```c
int cas_omap_load(struct cas_omap *om, const char *root_hash);
int cas_omap_store(struct cas_omap *om, char *hash_out);
```

`store` writes all dirty pages to CAS, then writes the directory
object.  Returns the directory's content hash -- this single hash is
the snapshot handle.  `load` replaces in-memory state from a
previously stored snapshot.

Old snapshots remain valid indefinitely.  Pages are immutable CAS
objects; storing a new snapshot only writes pages that changed (COW at
page granularity).

### Lookup

```c
int cas_omap_get(struct cas_omap *om, uint64_t id, char *hash_out);
int cas_omap_exists(struct cas_omap *om, uint64_t id);
```

O(1) lookup: binary-search the directory (small, in RAM), then index
directly into the page's slot array.

### Mutation

```c
int cas_omap_put(struct cas_omap *om, uint64_t id, const char *hash);
int cas_omap_del(struct cas_omap *om, uint64_t id);
void cas_omap_clear(struct cas_omap *om);
```

Pages are created on first write to any slot in their range.  Delete
zeroes the slot; empty pages are omitted from the next `store`.

### Allocation

```c
int cas_omap_alloc(struct cas_omap *om, uint64_t start,
                   uint64_t limit, uint64_t *id_out);
```

Finds the first unmapped ID >= `start`, below `start + limit` (0 =
no upper bound).  Skips full pages using the directory's population
count without loading page data.  Does NOT reserve the ID -- the
caller must `put` afterward.

Typical usage:

```c
uint64_t id;
cas_omap_alloc(om, 1200000, 100000, &id);
cas_omap_put(om, id, some_hash);
```

### Iteration

```c
uint64_t cas_omap_count(struct cas_omap *om);
int      cas_omap_page_count(struct cas_omap *om);

typedef int (*cas_omap_foreach_fn)(uint64_t id, const char *hash,
                                   void *ctx);
int cas_omap_foreach(struct cas_omap *om, cas_omap_foreach_fn fn,
                     void *ctx);
```

`foreach` visits entries in ascending ID order.  Pages are loaded on
demand during iteration.  `count` is O(pages) using the directory's
population counts -- no page loads needed.

## Internal architecture

### Two-level structure

```
Directory (in RAM, stored as CAS "omap" object)
  |
  +-- page 4687 --> [256 hash slots] (CAS "opage" object, 8192 bytes)
  +-- page 4688 --> [256 hash slots]
  +-- page 5078 --> [256 hash slots]
  ...
```

An ID maps to:
- page_num = id / 256
- slot     = id % 256

### Page format (CAS type "opage")

Raw 8192 bytes: 256 consecutive 32-byte slots.  Each slot holds a
binary BLAKE2b-256 hash, or all-zeros for empty.  No header, no
framing -- the page is purely positional data.

### Directory format (CAS type "omap")

Binary, little-endian:

```
Offset  Size  Field
------  ----  -----
0       4     Magic: "OMv1"
4       4     entry_count (uint32 LE)
8       48*N  Directory entries (sorted by page_num)
```

Each directory entry (48 bytes):

```
Offset  Size  Field
------  ----  -----
0       8     page_num (uint64 LE)
8       32    page_hash (binary, the CAS hash of the opage object)
40      2     pop_count (uint16 LE, number of occupied slots)
42      6     reserved (zero)
```

Entries are sorted by page_num for binary search.  Empty pages
(pop_count == 0) are not written.

### Memory layout

The directory array is always resident.  At 48 bytes per entry, 20,000
populated pages (covering ~5M objects) costs under 1 MB of RAM.

Pages are loaded lazily on first access and cached.  A page costs 8 KB.
Typical working sets (a few active zones) keep only tens of pages in
memory.

Dirty pages are tracked with a flag.  `cas_omap_store` writes only
pages whose content changed since last store, then rebuilds the
directory from current state.

### Copy-on-write semantics

Pages and the directory are ordinary CAS objects addressed by content
hash.  Two snapshots that share unchanged pages point to the same CAS
objects -- no data is duplicated.  This makes snapshot storage
proportional to the size of the delta, not the total map size.

### Allocation strategy

`cas_omap_alloc` scans pages starting from the one containing `start`:

1. If no directory entry exists for a page, all 256 slots are free.
   Return the first ID in that page (or `start` if it falls mid-page).

2. If the entry's pop_count == 256, skip the page entirely (no I/O).

3. Otherwise, load the page and scan slots sequentially for a zero.

For the typical zone pattern (sparse pages with many free slots), this
finds a free ID with at most one page load.

### Compaction

Since the game doesn't care about specific ID values, a full compaction
reassigns all objects to dense sequential IDs:

```c
// pseudocode
struct cas_omap *fresh = cas_omap_new(store);
uint64_t new_id = 0;
cas_omap_foreach(old, reassign_callback, fresh);
// where callback does: cas_omap_put(fresh, new_id++, hash);
```

Cross-references in game data must be updated via a separate remapping
pass.

## Error codes

| Code             | Meaning                              |
|------------------|--------------------------------------|
| CAS_OK           | Success                              |
| CAS_ERR          | Generic error (bad input, format)    |
| CAS_ENOTFOUND   | ID not mapped / snapshot not found   |
| CAS_ENOMEM      | Allocation failure                   |
| CAS_EIO         | Storage I/O error                    |
| CAS_ETYPE       | Wrong CAS object type                |
| CAS_OMAP_EFULL  | No free ID in the requested range    |

## Sizing estimates

| Objects | Pages | Directory RAM | Page cache (all loaded) |
|---------|-------|---------------|-------------------------|
| 5,000   | ~20   | 960 B         | 160 KB                  |
| 50,000  | ~200  | 9.6 KB        | 1.6 MB                  |
| 500,000 | ~2000 | 96 KB         | 16 MB                   |
| 5M      | ~20K  | 960 KB        | 160 MB                  |

In practice, only pages accessed during a session are cached.  A build
touching one zone of 5000 objects loads ~20 pages (160 KB).
