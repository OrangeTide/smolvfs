# smolvfs on-disk format

This is the normative specification of the byte formats smolvfs reads
and writes: content-addressed objects, their loose and packfile
encodings, directory (tree) objects, and the packfile bundle used to
move objects between depots. It is written so an independent
implementation, for example a server that produces bundles for a
smolvfs client, can interoperate at the byte level.

The object-map format has its own document, [cas-omap.md](cas-omap.md),
and is only referenced here.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

## Status and versioning

Formats carry explicit version markers in their magic values
(`...01...` for v1, `...02...` for v2 trailers, `HTv1` for htree). A
reader identifies a structure by its magic before interpreting any
other field and rejects unknown versions. There is no separate global
format-version number; each structure is versioned independently.

## Conventions

- **Byte order.** Every multi-byte integer in a binary structure is
  **little-endian**. This holds for the packfile index and footer and
  for the htree directory format, so a file is byte-identical across
  architectures. The only non-little-endian integers in the system are
  those written as ASCII text (the object header and the text tree).
- **Hash primitive.** BLAKE2b with a 32-byte (256-bit) digest.
- **Hex.** Hashes appear in text form as 64 lowercase hexadecimal
  characters.
- **Alignment.** Packfile structures are 64 bytes each and are laid
  out at 64-byte boundaries; readers may memory-map and cast them.

## Object model and addressing

Every stored value is an *object*: a short ASCII *type* plus a byte
*payload* (the plaintext). Its address is

```
address = BLAKE2b-256( type || " " || decimal(len) || "\0" || plaintext )
```

where `type` is the type string, `len` is the byte length of the
plaintext in ASCII decimal, a single space separates them, and a single
NUL byte follows. For example a 5-byte blob hashes the bytes
`blob 5\0` followed by the 5 payload bytes. The address is the lowercase
hex of the digest.

Type strings are at most 16 bytes. Types used by this specification:

| Type | Meaning |
|------|---------|
| `blob` | opaque file contents |
| `tree` | directory listing, text form (canonical) |
| `htree` | directory listing, hash-table form (see below) |

**The address never depends on how the object is stored.** The same
plaintext has the same address whether it is stored raw, compressed, or
(for directories) as `tree` or `htree`. Three encodings exist:

1. **Raw** - the stored bytes are the plaintext.
2. **Compressed** - the stored bytes are a codec tag plus a compressed
   payload; the address still commits to the plaintext. Signalled by
   the v2 trailer magic.
3. **Re-encoded** - an `htree` object stores a directory in a
   hash-table layout, but its address is the hash of the *canonical
   `tree` text form* of the same directory, not of the htree bytes. An
   `htree` therefore cannot be verified by hashing its own bytes; a
   verifier must reconstruct the text form, or trust an outer integrity
   check (the packfile index checksum and the htree's internal
   adler32).

## Codec tags

A compressed object's payload is prefixed by one printable-ASCII tag
byte identifying the codec. Printable bytes keep objects greppable.

| Tag | Byte | Codec | Status |
|-----|------|-------|--------|
| `%` | 0x25 | none (payload is plaintext) | intrinsic |
| `Z` | 0x5A | zlib/DEFLATE stream | implemented (optional) |
| `S` | 0x53 | zstd frame | reserved |
| `X` | 0x58 | xz/LZMA stream | reserved |
| `4` | 0x34 | LZ4 | reserved |

A reader that meets a tag it has no decoder for treats the object as
unverifiable rather than corrupt. `DEFLATE` is the only non-intrinsic
codec currently implemented, and only when the library is built with
it. Reserved tags must not be emitted by a producer that expects
current readers to decode them.

Compression is address-transparent and only kept when it saves space by
a comfortable margin, so an object smaller compressed than raw may still
be stored raw.

## Object trailer

Both loose objects and packfile entries end their data region with a
fixed 64-byte trailer:

```
offset  size  field
0       8     magic
8       56    header   ("type len\0", NUL-padded to 56)
```

The header holds the same `type " " len "\0"` string used in the hash
input, padded with NUL bytes to fill 56 bytes. The NUL padding is not
part of the hash input. A reader parses the type and length up to the
first NUL.

Two magic values distinguish the data-region encoding:

| Magic (8 bytes) | Meaning |
|-----------------|---------|
| `CB 4F 42 4A AA 01 00 00` | v1: data region is the raw plaintext |
| `CB 4F 42 4A AA 02 00 00` | v2: data region is one codec tag byte followed by the codec payload |

For a v2 object the header `len` is still the plaintext length and still
the hash input, so the address is unchanged.

## Loose object file

A loose object is a single file whose name is the object's hex address,
stored under a two-character fan-out directory:

```
<depot>/<first 2 hex chars>/<full 64-hex address>
```

The file contents are the data region followed by the 64-byte trailer:

```
[ data region ] [ trailer (64) ]
```

The data region occupies `[0, filesize - 64)`. To read: stat the file,
read the trailing 64 bytes, verify the magic, parse the header, and take
the data region as everything before the trailer. For a v1 trailer the
data region is the plaintext; for v2 it is `tag || payload`.

## Packfile

A packfile rolls many objects and a search index into one file. It is
the unit of transport: a "bundle" is simply a packfile. Layout:

```
[ obj0 data | obj0 trailer(64) ]
[ obj1 data | obj1 trailer(64) ]
  ...
[ objN data | objN trailer(64) ]
[ index entry 0 (64) | ... | index entry N (64) ]
[ footer (64) ]
```

Each object is encoded exactly as in a loose file (data region plus a
v1 or v2 trailer). The index follows all objects, and the footer is the
last 64 bytes of the file.

### Index entry (64 bytes)

Entries are sorted ascending by `hash` for binary search.

```
offset  size  field         encoding
0       32    hash          32 raw digest bytes
32      8     offset        little-endian u64
40      8     stored_size   little-endian u64
48      16    reserved      zero
```

`offset` is the absolute byte offset of the object's *trailer*. The
object's data region is `[offset - region_size, offset)`, where
`region_size` is `stored_size` if non-zero, otherwise the header `len`.
A zero `stored_size` means "same as the plaintext length" and lets packs
written before compression existed read back unchanged; a compressed
object records its on-disk region length here because that differs from
the plaintext length.

### Footer (64 bytes)

```
offset  size  field         encoding
0       8     magic         CB 50 4B 46 AA 01 00 00
8       8     entry_count   little-endian u64
16      32    checksum      32 raw digest bytes
48      16    reserved      zero
```

`checksum` is `BLAKE2b-256` over the concatenation of all index-entry
bytes followed by the first 16 bytes of the footer (its magic and
`entry_count`). A reader validates the checksum before trusting the
index. Object payload integrity is separately checked per object by
re-hashing against the index `hash`.

### Reading a packfile

1. Map or read the file. Read the last 64 bytes as the footer; verify
   the footer magic.
2. Take `entry_count` (little-endian). The index is the
   `entry_count * 64` bytes immediately before the footer.
3. Verify the footer checksum over `index || footer[0:16]`.
4. To find an object, binary-search the index by hash, then use `offset`
   and `stored_size` to locate its trailer and data region.

## Directory objects

A directory is a list of entries, each with a name, POSIX-style mode,
owner/group ids, modification time, and the address of the child object
(a `blob` for a file, a `tree`/`htree` for a subdirectory). Entries are
**sorted ascending by name** (byte-wise) to make the encoding canonical:
the address of a directory depends only on its contents, not on
insertion order.

Two encodings share one address (the `tree` address):

### Text tree (`tree`)

The canonical form. The payload is one marker byte `%` (0x25) followed
by one line per entry:

```
%06o SP uid SP gid SP mtime_s SP mtime_ns SP hash SP name LF
```

- `%06o` - mode as zero-padded 6-digit octal (includes the type bits,
  e.g. `100644` for a regular file, `040755` for a directory).
- `uid`, `gid` - decimal.
- `mtime_s` - decimal signed 64-bit seconds.
- `mtime_ns` - decimal signed 32-bit nanoseconds.
- `hash` - 64 hex characters, the child address.
- `name` - the entry name (no NUL, no embedded LF).

The object type is `tree` and its address is the hash of `tree len\0`
followed by this payload.

### Htree (`htree`)

A CDB-inspired hash table giving O(1) name lookup without parsing the
whole directory. The object type is `htree`, but **its address is the
address of the equivalent `tree`** (the sorted text form above), so a
producer computes the text serialization to derive the address and may
then store either encoding at it. All integers are little-endian.

```
[ header:  256 buckets x 8 bytes = 2048 bytes ]
[ records: variable ]
[ tables:  variable ]
[ footer:  8 bytes ]
```

**Header.** 256 bucket slots, each 8 bytes:

```
u32 table_offset   absolute offset of this bucket's slot table
u32 nslots         number of slots in this bucket's table
```

**Records.** One per entry, concatenated. A reader locates records
through the tables by offset, so their physical order is not
significant (this implementation emits them in the sorted-name order
used for the canonical address, but a consumer must not rely on it):

```
u32 keylen                    length of the name
u32 datalen                   always 56
key[keylen]                   the name bytes (no NUL)
data[56]:
  u32 mode
  u32 uid
  u32 gid
  u64 mtime_s
  u32 mtime_ns
  hash[32]                    32 raw digest bytes of the child
```

**Tables.** For each bucket `b` in `0..255`, `nslots[b]` slots of 8
bytes, where `nslots[b]` is twice the number of entries in that bucket:

```
u32 slot_hash                 the entry's full djb hash
u32 rec_offset                absolute offset of the entry's record
```

An empty slot has `rec_offset == 0`. An entry with name `k` is placed in
bucket `djb(k) % 256`; within that bucket its home slot is
`(djb(k) / 256) % nslots`, with linear probing (increment modulo
`nslots`) on collision. Lookup mirrors this: probe from the home slot,
comparing `slot_hash` then the record's stored name, until a match or an
empty slot.

**Footer.** 8 bytes:

```
u32 adler32                   Adler-32 checksum over all bytes before
                              the footer (header + records + tables)
"HTv1"                        4-byte magic, the last bytes of the object
```

**djb hash.** `h = 5381; for each byte c: h = (h * 33) ^ c` in unsigned
32-bit arithmetic.

**Adler-32.** The standard checksum: `a = 1, b = 0`; for each byte,
`a = (a + byte) % 65521`, `b = (b + a) % 65521`; result `(b << 16) | a`.

A reader distinguishes the two directory encodings by the object type
(`tree` vs `htree`), not by content sniffing; the trailing `HTv1` magic
and the adler32 confirm an htree's integrity.

## Object map

The sparse numeric-id-to-hash object map is a separate CAS object type
with its own layout. See [cas-omap.md](cas-omap.md).

## Repository layout

A depot is a directory. These paths are local repository state, not part
of any transported bundle:

```
<depot>/<xx>/<hash>      loose objects (xx = first two hex chars)
<depot>/pack.dat         optional packfile, read transparently
<depot>/refs/<name>      a ref: a file holding a 64-hex root address
<depot>/refs/<name>.log  append-only commit log for that ref
```

When present, `pack.dat` is consulted alongside loose objects: a lookup
checks the pack, then the loose store. Refs name root objects (typically
directory trees) so they are reachable and survive garbage collection.

## Bundles and interoperation

A packfile is self-contained and portable (little-endian, self-checksummed),
so it doubles as a transport bundle. A producer packs the objects
reachable from a root and ships the file plus the root's hex address. A
consumer merges the objects into its depot, deduplicated by address, and
records a ref at the shipped root.

A conforming consumer:

- Validates the footer magic and checksum before using the index.
- Re-hashes each self-addressed object (`blob`, `tree`, compressed
  blobs after decoding) against its index hash and rejects a mismatch.
- Stores an `htree` object verbatim at its index address without the
  re-hash check, relying on the index checksum and the htree's adler32,
  because the htree address commits to the text form rather than to the
  htree bytes.
- Skips objects it already holds.

A producer must sort directory entries by name, must compute directory
addresses from the canonical `tree` text form even when shipping the
`htree` encoding, and must write all binary integers little-endian.
