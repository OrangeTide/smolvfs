# smolvfs on-disk format

This is the normative specification of the byte formats smolvfs reads
and writes: content-addressed objects, their loose and packfile
encodings, directory (tree) objects, and the packfile bundle used to
move objects between depots. It is written so an independent
implementation, for example a server that produces bundles for a
smolvfs client, can interoperate at the byte level.

The object-map format has its own document, [cas-omap.md](cas-omap.md),
and is only referenced here.

PUBLIC DOMAIN (CC0-1.0)

## Status and versioning

Formats carry explicit version markers in their magic values
(`...01...` for v1, `...02...` for v2 trailers, `HTv1` for htree). A
reader identifies a structure by its magic before interpreting any
other field and rejects unknown versions. There is no separate global
format-version number; each structure is versioned independently.

### Incompatible change: entry names must be UTF-8

**Directory entry names are now required to be well-formed UTF-8** (see
"Entry names"). Through smolvfs 0.2.0 a name was any octet string apart
from `/`, `\n`, and NUL, so this narrows what a valid directory may
contain and is not backward compatible.

What breaks:

- A directory object written by an earlier version and containing a name
  that is not valid UTF-8 no longer loads. The failure is a clean
  rejection of the whole object, not a partial read.
- `castool` refuses to add a file whose basename is not valid UTF-8. On
  Linux a filename is an arbitrary byte string, so this is reachable from
  ordinary use rather than only from a crafted depot.

No such depot is known to exist. The change is taken deliberately anyway,
because the rule cannot be relaxed on the read path without giving up the
protection it exists for: an overlong sequence encodes `/` in octets that
are not `0x2F`, so it passes a separator test here and decodes to a
separator in any consumer less strict. A name that is a path separator on
arrival but not on inspection is worth an incompatible change to
foreclose.

There is no conversion tool. A depot holding such a name must be
rewritten by re-importing its content with names the producer has made
valid, which is a decision about what those names should be and not one a
library can make.

The htree layout tightening that landed alongside this is *not* a break:
every htree this implementation has ever written already satisfies it.
Only the name rule is incompatible.

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
3. **Canonically re-encoded** - an `htree` object stores a directory in
   a hash-table layout, but its address is the hash of the *canonical
   `tree` text form* of the same directory, not of the htree bytes. It
   is verified in two steps rather than one; see "Verifying an htree"
   below.

Every encoding is verifiable against the address, and no fourth class
may be added that is not. The distinction between 2 and 3 is that a
compressed object's only read path is decoding, so recovering the
plaintext is a complete check, whereas an htree carries a second read
path (the hash table) that the plaintext does not constrain. Any
encoding adding such a path belongs to class 3 and must be pinned
byte-for-byte.

An internal checksum is never an integrity check. The htree's adler32
and the packfile index checksum detect corruption cheaply; neither
resists forgery, and neither may stand in for verification against the
address.

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
the unit of transport: a *bundle* is a packfile. Layout:

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
(a `blob` for a file, a `tree`/`htree` for a subdirectory).

Two encodings share one address (the `tree` address). The rules below
apply to both, and a reader enforces them rather than assuming them.

### Entry names

A name is between 1 and 255 octets and must not contain `/`, `\n`, or
NUL. It must also be **well-formed UTF-8**, rejecting the three things a
lenient decoder would accept. This last requirement is newer than the
others and is not backward compatible; see "Incompatible change: entry
names must be UTF-8" above.

- an **overlong** sequence, encoding a codepoint in more octets than it
  needs;
- a **surrogate**, U+D800 through U+DFFF;
- anything **past U+10FFFF**.

Overlong forms are the reason this is a hard requirement rather than
advice. The two octets `C0 AF` are not the octet `0x2F`, so they pass a
test for `/`, yet they decode to U+002F in any consumer less strict than
this one. A name that is a path separator on arrival but not on
inspection is a path traversal waiting to happen.

Normalization is **not** performed. NFC would require Unicode tables this
format will not carry, so byte-distinct names are distinct entries even
when they render identically. A producer that needs a name to address the
same on every platform must normalize before storing, which matters most
between systems that disagree by default, macOS decomposing where Linux
does not.

### Entry order

Entries appear in **strictly ascending order by name, compared as
unsigned octets**. For well-formed UTF-8 this is also codepoint order,
because UTF-8 is built so that octet-wise and codepoint-wise comparison
agree; one rule serves both and needs no table. Where one name is a
prefix of another the shorter sorts first.

Strictly ascending has two consequences, and the second is the point:

- The encoding is canonical, so a directory's address depends only on its
  contents and not on insertion order.
- **Duplicate names are impossible.** Two entries sharing a name have no
  canonical order between them, so the address would depend on how a sort
  happened to break the tie. Worse, a duplicate lets a listing and a
  lookup disagree about which child a name has, which is the same class
  of divergence "Verifying an htree" addresses.

A reader that meets an out-of-order or repeated name rejects the object.

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

Each field has exactly one spelling. The mode is octal padded to at least
six digits; the numeric fields carry no sign, no leading zeros, and no
padding; single spaces separate them and a single `\n` ends each line. A
lenient parser would read `0100644` and `+1` as the same values a
canonical line spells `100644` and `1`, producing a second address for
one directory. The bytes are what the address commits to, so such an
object is not a forgery, merely a duplicate that will never dedup and
that moves when it is loaded and stored back. Verification rejects it:
re-serializing a parsed text tree must reproduce it exactly.

### Htree (`htree`)

A CDB-inspired hash table giving O(1) name lookup without parsing the
whole directory. The object type is `htree`, but its address is the
address of the equivalent `tree` (the sorted text form above), so a
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
significant to a *reader* looking one up, which goes through the tables.
It is significant everywhere else: records are emitted in the strictly
ascending name order of "Entry order" above, both because the whole
encoding is pinned (see "Verifying an htree") and because scanning them
in order is what makes a duplicate name detectable at all:

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

### Verifying an htree

An htree is accepted only when **both** of these hold:

1. **The entry set is right.** Recover the entries, sort them
   byte-wise by name, serialize the canonical `tree` text form, and
   require `BLAKE2b-256("tree" len\0 || text)` to equal the address the
   object was fetched under.
2. **The encoding is pinned.** Re-derive the htree from those same
   entries and require the result to equal the stored bytes exactly.

Step 1 alone is not sufficient, and the reason is worth stating because
it is not obvious. Entries can be recovered two ways, by scanning the
records region or by following the tables, and only the second is the
path a lookup takes. An object whose tables point at records the record
scan never reaches will pass step 1 while returning different data from
a name lookup, so a directory listing and a lookup would disagree about
the same object. Step 2 covers every byte and forecloses this, whichever
way step 1 recovered the entries.

Because step 2 compares bytes, the layout is **canonical**: given an
entry set, exactly one valid htree encoding exists. Records appear in
sorted-name order, each bucket's table has `2 * bucket_count` slots, and
placement follows the probing rule below. The `HTv1` footer magic selects
which derivation applies, so a future layout revision becomes `HTv2`
rather than invalidating stored objects.

The adler32 is a corruption pre-filter. Checking it first avoids the cost
of the two steps above on a damaged object; passing it means nothing
about authenticity, since a producer of forged bytes computes it too.

A reader selects the parser by object type (`tree` vs `htree`), never by
sniffing content.

### Where these rules are enforced

**On write, for anything from outside.** An object obtained from a peer,
an origin, or a user is fully checked before it is admitted to a depot,
and is not stored at all if it fails. That is the only point at which a
hostile producer is turned away, so it is the point that may not be
skipped.

**On read, not again.** An object already in the depot is trusted. A
reader re-establishing what admission established would be work paid on
every walk for the rest of the object's life, against an adversary who
never got in.

Reads still keep the checks they get for free. Both directory parsers
walk every entry anyway, so both enforce "Entry order" as they go, which
costs one comparison per entry and catches depot damage rather than
forgery. A text-tree lookup validates the lines it passes on the way to
its answer, because it is comparing names regardless and its early exit
is only sound while the order holds. An htree lookup is a single probe
through the tables and checks nothing global, since doing so would cost
exactly what the encoding exists to avoid.

**Periodically, over everything.** fsck re-runs the full checks. This
answers a different question from admission: not whether an object was
sound when it arrived, but whether it still is. Bit rot, a truncated
write, and a tamper below the library are invisible to both of the
first two.

## Object map

The sparse numeric-id-to-hash object map is a separate CAS object type
with its own layout. See [cas-omap.md](cas-omap.md).

## Repository layout

A depot is a directory. These paths are local repository state, not part
of any transported bundle:

```
<depot>/<xx>/<hash>       loose objects (xx = first two hex chars)
<depot>/pack.dat          optional packfile, read transparently
<depot>/refs/<name>.root  a ref: a file holding a 64-hex root address
<depot>/refs/<name>.log   append-only commit log for that ref
<depot>/refs/<name>.prev  previous root, kept for crash recovery
<depot>/refs/<name>.lock  transient lock held during a ref update
```

When present, `pack.dat` is consulted alongside loose objects: a lookup
checks the pack, then the loose store. Refs name root objects (typically
directory trees) so they are reachable and survive garbage collection.
Every entry of a ref's `.log`, not just its current `.root`, marks its
tree as reachable, so committed history is retained until the log entry
is removed.

A ref may be sparse: its log can name objects the depot no longer
stores. The log is prunable. Truncating it to the last N entries (or to
entries newer than a cutoff) discards old snapshots, after which garbage
collection reclaims the objects those pruned snapshots alone reached.
The newest log entry is always retained because it is the ref's live
root. Garbage collection then treats a missing object as a boundary and
stops descending there rather than failing, so a pruned depot is legal
and incomplete rather than corrupt. A missing object is distinct from a
corrupt or wrong-type one, which is still a hard error.

## Bundles and interoperation

A packfile carries its own index and BLAKE2b checksum and stores every
integer little-endian, so it reads identically on any machine, which
makes it usable directly as a transport bundle. A producer packs the
objects reachable from a root and ships the file plus the root's hex
address. A consumer merges the objects into its depot, deduplicated by
address, and records a ref at the shipped root.

A conforming consumer:

- Validates the footer magic and checksum before using the index.
- Re-hashes each self-addressed object (`blob`, `tree`, compressed
  blobs after decoding) against its index hash and rejects a mismatch.
- Stores an `htree` object verbatim at its index address without the
  re-hash check (see Re-encoded, above).
- Skips objects it already holds.

A producer must sort directory entries by name, must compute directory
addresses from the canonical `tree` text form even when shipping the
`htree` encoding, and must write all binary integers little-endian.
