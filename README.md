# smolvfs -- a small virtual file system (VFS) layer

## Introduction

smolvfs is a dependency-free C library that provides an in-memory virtual
filesystem with Unix-style permissions, quotas, and a content-addressable
storage (CAS) backend.  It is designed to be dropped into any C project as a
handful of source files -- no build system integration beyond compiling a few
`.c` files, no external libraries.  The VFS presents a familiar POSIX-like
interface (stat, read, write, mkdir, rename, delete) while the CAS layer
provides persistent, deduplicated blob storage backed by BLAKE2b-256 hashes.

## Overview

The library has three modules that can be used at different levels:

**VFS** (`vfs.h` / `vfs.c`) -- An in-memory filesystem with hierarchical
directories, file data, Unix-style permission bits (owner/group/other
read/write/execute), per-user ownership, umask, and configurable byte quotas.
Paths are normalized Unix-style (`/foo/bar`), with `.` and `..` resolution,
duplicate-slash collapsing, and illegal-character rejection.  The VFS runs
entirely in process memory with no disk I/O, making it suitable for:

- Sandboxed execution environments that need a virtual filesystem.
- Game engines or embedded applications with asset overlays.
- Testing filesystem-dependent code without touching the real filesystem.
- Configuration or state trees that benefit from path-based organization
  with access control.

**CAS-Tree** (`cas-tree.h` / `cas-tree.c`) -- A tree-structured content
layer built on CAS.  Maps directory hierarchies onto Merkle trees of CAS
objects, with named refs, append-only snapshot logs, and atomic commits.
Structural sharing means committing a single-file change only creates
O(depth) new objects regardless of tree size.  Depends on `cas.h`.
Useful for:

- Persistent, versioned, deduplicated directory trees.
- Snapshot and rollback workflows.
- Diff between two snapshots by walking their tree roots.
- Backing the VFS with on-disk persistence.

**CAS Pack** (`cas-pack.h` / `cas-pack.c`) -- Packfile format for CAS.
Rolls many loose objects into a single file with a binary-searchable
index for fast lookup.  Both loose objects and packfile entries use the
same 64-byte trailer structure, sharing hash verification and object
reading code.  The CAS store auto-opens `pack.dat` in the depot
directory at startup and transparently falls back to it for reads.
Depends on `cas.h`.

**CAS** (`cas.h` / `cas.c`) -- A content-addressable store that writes
objects to disk identified by their BLAKE2b-256 digest (32 bytes, 64 hex
characters).  Writes are atomic (mkstemp + rename), reads are mmap-based,
and duplicate writes are automatically deduplicated.  The CAS module
includes its own embedded BLAKE2b-256 implementation (RFC 7693) so it has
zero external dependencies.  Objects may optionally be stored compressed
while keeping their uncompressed address, so pre-compressed data can be
ingested without a client-side compression step (see Compression under
Pack format).  Useful on its own for:

- Deduplicating asset or document storage.
- Caching build artifacts or computed results by content hash.
- Any scenario where immutable, hash-indexed blobs are a good fit.

## Usage

### Adding to your project

Copy the files you need into your source tree:

- **VFS only:** `vfs.h`, `vfs.c`
- **CAS only:** `cas.h`, `cas.c`, `cas-pack.h`, `cas-pack.c`, `cas-codec.h`, `cas-codec.c`
- **CAS + trees:** the CAS files above plus `cas-tree.h`, `cas-tree.c`
- **Everything:** all files

The VFS module is fully independent (pure in-memory, no disk I/O).  The
CAS module depends on CAS-Pack and CAS-Codec.  The CAS-Tree module
depends on CAS.  Using the VFS with a CAS-Tree backend requires all
three.  The CAS-Codec module is a small self-contained compression
codec table; on its own it pulls in no compression library.  To
enable the optional bundled DEFLATE codec, also compile
`cas-codec-miniz.c` with `third_party/miniz.c` and define
`-DCAS_WITH_MINIZ` (the Makefile does this with `make MINIZ=1`).

### Compiling

Add the `.c` files to your build.  The only requirement is a C99 (or later)
compiler and POSIX for the CAS module (it uses `mmap`, `mkstemp`, `rename`).
The VFS module is pure C99 with no POSIX dependency.

```sh
cc -c vfs.c
cc -c cas.c
cc -c cas-pack.c
cc -c cas-tree.c
cc -o myapp myapp.c vfs.o cas-tree.o cas-pack.o cas.o
```

Or as a static library:

```sh
cc -c vfs.c
cc -c cas.c
cc -c cas-pack.c
cc -c cas-tree.c
ar rcs libsmolvfs.a vfs.o cas.o cas-pack.o cas-tree.o
cc -o myapp myapp.c -L. -lsmolvfs
```

### VFS example

```c
#include "vfs.h"
#include <stdio.h>

int
main(void)
{
    struct vfs_opts opts = { .quota = 1024 * 1024 };
    struct vfs *fs = vfs_new(&opts);

    struct vfs_cred cred = { .uid = 1000, .is_admin = 1 };

    /* create directories and write a file */
    vfs_mkdir(fs, &cred, "/etc", 0);
    vfs_write(fs, &cred, "/etc/greeting",
              "hello\n", 6, 0);

    /* read it back */
    const void *data;
    size_t len;
    if (vfs_read(fs, &cred, "/etc/greeting", &data, &len) == VFS_OK)
        printf("%.*s", (int)len, (const char *)data);

    vfs_free(fs);
    return 0;
}
```

### CAS example

```c
#include "cas.h"
#include <stdio.h>

int
main(void)
{
    struct cas *store = cas_new("depot");
    char hash[CAS_HASH_HEX + 1];

    /* store a blob */
    cas_put(store, "hello world\n", 12, hash);
    printf("stored: %s\n", hash);

    /* read it back by hash */
    struct cas_file cf;
    if (cas_open(store, &cf, hash) == CAS_OK) {
        printf("data: %.*s", (int)cf.len, cf.data);
        cas_close(&cf);
    }

    cas_free(store);
    return 0;
}
```

### castool -- CAS-tree CLI

`castool` is a command-line tool for administering and debugging CAS-tree
depots.  It provides import/export, inspection, integrity checking, and
garbage collection.

```
castool [-d depot] <command> [args...]
```

The `-d` flag sets the depot directory (default: `depot`).

**Commands:**

| Command | Usage | Description |
|---------|-------|-------------|
| `refs` | `refs` | List all refs in the depot |
| `log` | `log <ref>` | Show the commit log for a ref |
| `cat` | `cat <hash>` | Print the raw body of any object (blob or tree) |
| `ls` | `ls <ref-or-hash>` | List entries in a tree (single level) |
| `tree` | `tree <ref-or-hash>` | Recursively list a tree with full paths |
| `import` | `import [-z] <ref> <file>...` | Import files into a ref; `-z` compresses text-like files |
| `export` | `export <ref-or-hash> <destdir>` | Export a tree to a directory, preserving permissions |
| `rm` | `rm <ref> <name>...` | Remove named entries from a ref's tree |
| `hash` | `hash [file]` | Compute the blob hash of a file (or stdin) |
| `fsck` | `fsck` | Verify integrity of all reachable trees and blobs |
| `gc` | `gc` | Remove unreachable objects older than one hour |
| `pack` | `pack [-z]` | Pack loose objects into `pack.dat`; `-z` compresses |

Where a command accepts `<ref-or-hash>`, either a ref name or a 64-character
hex hash may be used.

**Reclaiming disk space:** the natural maintenance cycle is `gc` (drop
unreachable objects), then `pack -z` (roll the rest into one compressed
packfile).  `-z` applies the `CAS_COMPRESS_GUESS` policy, compressing
text-like objects into the packfile where that saves space and leaving
binary or already-compressed content untouched, so a run of `gc` +
`pack -z` is the usual way to shrink a depot on disk:

```sh
castool gc
castool pack -z
```

`pack -z` compresses with the bundled DEFLATE codec, so `castool` must
be built with it (`make MINIZ=1`); without a codec compiled in, `-z`
prints a warning and packs uncompressed.  A depot packed with `-z` can
only be read by a build that has the matching decoder; `fsck` from a
build without it reports such objects as `skipped (no codec)` rather
than failing.

Alternatively, `import -z` compresses at ingest time instead of at
pack time.  It applies the `CAS_COMPRESS_GUESS` policy (see the CAS
codec table), compressing text-like files and leaving binary content
raw, so importing a mix of source and media only pays compression on
what benefits.  Both paths store blobs at their plaintext address, so
they interoperate: an object imported with `-z` and later packed is
already compressed and copied through unchanged.

**@file expansion:** any argument starting with `@` is expanded to lines read
from the named file (`@files.txt` reads `files.txt`, one argument per line).
`@-` reads from stdin.  This is useful for large file lists:

```sh
find /data -name '*.csv' > filelist.txt
castool import myref @filelist.txt
```

**Examples:**

```sh
# import files into a ref named "backup"
castool import backup src/cas.c src/cas.h

# list what's in the ref
castool ls backup

# export a snapshot to a directory
castool export backup /tmp/restored

# remove an entry
castool rm backup cas.h

# check integrity and collect garbage
castool fsck
castool gc

# pack loose objects into a single packfile
castool pack

# compute a blob hash without storing
castool hash README.md
echo "hello" | castool hash
```

## Building

### Prerequisites (Debian/Ubuntu)

Only a C compiler and make are required.  On a minimal system:

```sh
sudo apt-get install build-essential
```

That provides `cc` (gcc), `make`, `ar`, and the standard C library headers.
No other packages are needed.

### Build

```sh
make
```

This compiles all source files and produces the `smolvfs` binary (from
`sample_main.c`).  Debug symbols are split into `smolvfs.debug`.

### Other targets

```sh
make clean        # remove object files
make clean-all    # remove objects, binary, and dependency files
make test         # run test.sh (if present)
make run          # build and run smolvfs
```

### Compiler flags

The default Makefile uses:

```
CFLAGS = -Wall -Wextra -g -Og -fno-omit-frame-pointer
```

Override on the command line for a release build:

```sh
make CFLAGS="-O2 -DNDEBUG -Wall -Wextra"
```

## Manual

### VFS error codes

All VFS functions return `int` status codes.  Zero is success; negative
values are errors.

| Code | Value | Meaning |
|------|-------|---------|
| `VFS_OK` | 0 | Success |
| `VFS_ERR` | -1 | Generic error |
| `VFS_ENOTFOUND` | -2 | Path does not exist |
| `VFS_EEXIST` | -3 | Path already exists |
| `VFS_EPERM` | -4 | Permission denied |
| `VFS_EQUOTA` | -5 | Quota exceeded |
| `VFS_ENOTDIR` | -6 | Expected a directory, found a file |
| `VFS_EISDIR` | -7 | Expected a file, found a directory |
| `VFS_ENOTEMPTY` | -8 | Directory is not empty |
| `VFS_EBADPATH` | -9 | Invalid or malformed path |
| `VFS_ENOMEM` | -10 | Memory allocation failed |

```c
const char *
vfs_strerror(int err);
```

Returns a static string describing the error code.

### Node types

```c
enum vfs_type {
    VFS_FILE,
    VFS_DIR,
};
```

### Permission bits

Standard Unix-style octal bits.  Only read and write are enforced by the
library; execute is stored but not checked.

| Constant | Value | Description |
|----------|-------|-------------|
| `VFS_OWNER_R` | 0400 | Owner read |
| `VFS_OWNER_W` | 0200 | Owner write |
| `VFS_OWNER_X` | 0100 | Owner execute (stored, not enforced) |
| `VFS_GROUP_R` | 0040 | Group read |
| `VFS_GROUP_W` | 0020 | Group write |
| `VFS_GROUP_X` | 0010 | Group execute (stored, not enforced) |
| `VFS_OTHER_R` | 0004 | Other read |
| `VFS_OTHER_W` | 0002 | Other write |
| `VFS_OTHER_X` | 0001 | Other execute (stored, not enforced) |
| `VFS_MODE_FILE_DEFAULT` | 0666 | Default mode for new files |
| `VFS_MODE_DIR_DEFAULT` | 0777 | Default mode for new directories |

### Data structures

**`struct vfs_stat`** -- Metadata for a node, filled by `vfs_stat()` and the
`vfs_list()` callback.

| Field | Type | Description |
|-------|------|-------------|
| `path` | `const char *` | Normalized absolute path |
| `type` | `enum vfs_type` | `VFS_FILE` or `VFS_DIR` |
| `mode` | `int` | Permission bits (masked to 0777) |
| `owner` | `int` | Owner UID |
| `size` | `uint64_t` | File size in bytes (0 for directories) |
| `ctime` | `time_t` | Creation time |
| `mtime` | `time_t` | Last modification time |

**`struct vfs`** -- Opaque filesystem handle.  Created with `vfs_new()`,
destroyed with `vfs_free()`.

**`struct vfs_opts`** -- Options passed to `vfs_new()`.

| Field | Type | Description |
|-------|------|-------------|
| `quota` | `uint64_t` | Maximum total bytes for all files; 0 = unlimited |
| `umask` | `int` | Default umask applied to new nodes |

**`struct vfs_cred`** -- Caller identity passed to every permission-checked
operation.

| Field | Type | Description |
|-------|------|-------------|
| `uid` | `int` | User ID stamped on creates and writes |
| `is_admin` | `int` | Nonzero bypasses all permission checks |
| `is_group` | `int` | Nonzero means caller is in the node's group |

### Lifecycle

```c
struct vfs *
vfs_new(const struct vfs_opts *opts);
```

Create a new empty filesystem.  `opts` may be NULL for defaults (no quota,
zero umask).  The root directory `/` is implicit and always exists.  Returns
NULL on allocation failure.

```c
void
vfs_free(struct vfs *fs);
```

Destroy a filesystem and free all nodes and their data.  Safe to call with
NULL.

### Operations

---

```c
int
vfs_stat(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, struct vfs_stat *st);
```

Retrieve metadata for a path.  The path is normalized before lookup.  Root
(`/`) always succeeds.  Requires read permission on the target node.

**Returns:** `VFS_OK` on success.  `VFS_ENOTFOUND` if the path does not
exist.  `VFS_EPERM` if the caller lacks read permission.

---

```c
int
vfs_list(struct vfs *fs, const struct vfs_cred *cred,
         const char *dirpath,
         int (*fn)(const struct vfs_stat *st, void *ctx),
         void *ctx);
```

Iterate over direct children of a directory.  Calls `fn` for each child
with its metadata and the caller-supplied `ctx`.  If `fn` returns nonzero,
iteration stops and that value is returned.  Only direct children are
listed, not descendants.

**Returns:** `VFS_OK` after visiting all children.  `VFS_ENOTFOUND` if the
directory does not exist.  `VFS_ENOTDIR` if the path is not a directory.
`VFS_EPERM` if the caller lacks read permission on the directory.

---

```c
int
vfs_read(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, const void **out, size_t *out_len);
```

Read the contents of a file.  On success, `*out` points to the file data
and `*out_len` is set to the size in bytes.  The returned pointer is
borrowed -- it is valid until the node is modified or the filesystem is
freed.  Do not free it.

**Returns:** `VFS_OK` on success.  `VFS_EISDIR` if the path is a
directory.  `VFS_EPERM` if the caller lacks read permission.

---

```c
int
vfs_write(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, const void *data, size_t len,
          int create_parents);
```

Write (create or overwrite) a file.  The data is copied internally.
If `create_parents` is nonzero, missing ancestor directories are created
automatically with default mode minus umask.  Writing to an existing file
requires write permission; the file's content is replaced entirely.
Quota is checked before allocation -- if the write would exceed the
configured quota, it fails without modifying anything.

**Returns:** `VFS_OK` on success.  `VFS_EISDIR` if the path is a directory.
`VFS_EQUOTA` if the write would exceed the quota.  `VFS_ENOTFOUND` if a
parent directory does not exist and `create_parents` is 0.

---

```c
int
vfs_mkdir(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int create_parents);
```

Create a directory.  If `create_parents` is nonzero, behaves like `mkdir -p`:
missing ancestors are created, and no error is returned if the target
directory already exists.

**Returns:** `VFS_OK` on success.  `VFS_EEXIST` if the path already exists
(when `create_parents` is 0).  `VFS_ENOTFOUND` if a parent does not exist
and `create_parents` is 0.

---

```c
int
vfs_delete(struct vfs *fs, const struct vfs_cred *cred,
           const char *path);
```

Delete a file or an empty directory.  The root directory cannot be deleted.
Requires write permission on the target.

**Returns:** `VFS_OK` on success.  `VFS_ENOTEMPTY` if the directory has
children.  `VFS_EPERM` for root or insufficient permission.

---

```c
int
vfs_delete_recursive(struct vfs *fs, const struct vfs_cred *cred,
                     const char *path);
```

Delete a directory and all of its contents.  For a file, behaves like
`vfs_delete`.  The root directory cannot be deleted.  Requires write
permission on the target.

**Returns:** `VFS_OK` on success.  `VFS_EPERM` for root or insufficient
permission.

---

```c
int
vfs_rename(struct vfs *fs, const struct vfs_cred *cred,
           const char *old_path, const char *new_path);
```

Rename or move a node.  If the destination exists and is an empty directory
or a file, it is replaced.  A directory cannot be moved into its own
subtree.  All descendants of a renamed directory have their paths updated.
Requires write permission on the source.

**Returns:** `VFS_OK` on success.  `VFS_ENOTEMPTY` if the destination is a
non-empty directory.  `VFS_EBADPATH` if the move would create a cycle.

---

```c
int
vfs_copy(struct vfs *fs, const struct vfs_cred *cred,
         const char *src_path, const char *dst_path,
         int create_parents);
```

Copy a file.  The source must be a file (directories are not supported).
The data is duplicated.  Requires read permission on the source; the
destination is created via `vfs_write`.

**Returns:** `VFS_OK` on success.  `VFS_EISDIR` if the source is a
directory.

---

```c
int
vfs_chmod(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int mode);
```

Change the permission bits of a node.  Admin-only -- the caller's
`is_admin` field must be set.

**Returns:** `VFS_OK` on success.  `VFS_EPERM` if the caller is not admin.

### Quota and usage

```c
uint64_t
vfs_usage(struct vfs *fs);
```

Returns the total bytes currently used by all file contents.

```c
uint64_t
vfs_quota(struct vfs *fs);
```

Returns the configured quota in bytes.  0 means unlimited.

### Path utilities

```c
int
vfs_normalize(char *buf, size_t bufsz, const char *path);
```

Normalize a path into `buf`.  Collapses duplicate slashes, resolves `.`
and `..` (clamped at root), and strips trailing slashes.  The input must
start with `/`.

**Returns:** `VFS_OK` on success.  `VFS_EBADPATH` if the path is empty,
does not start with `/`, or does not fit in `buf`.

```c
int
vfs_resolve(char *buf, size_t bufsz,
            const char *path, const char *cwd);
```

Resolve a possibly-relative path against a working directory.  If `path`
starts with `/`, it is normalized directly.  Otherwise it is joined with
`cwd` and then normalized.  If `cwd` is NULL or does not start with `/`,
root is assumed.

**Returns:** `VFS_OK` on success.  `VFS_EBADPATH` on error.

### Pattern matching

```c
int
vfs_fnmatch(const char *pattern, const char *name);
```

Match a filename against a glob pattern.  Case-insensitive.  Supports
`*` (match any sequence), `?` (match one character), `[abc]` (character
class), `[a-z]` (range), and `[!a]` (negated class).  Intended for
matching the filename component only, not full paths.

**Returns:** Nonzero if the name matches, 0 if it does not.

### CAS error codes

| Code | Value | Meaning |
|------|-------|---------|
| `CAS_OK` | 0 | Success |
| `CAS_ERR` | -1 | Generic error (bad hash, path overflow) |
| `CAS_ENOTFOUND` | -2 | Blob not found |
| `CAS_ENOMEM` | -3 | Memory allocation failed |
| `CAS_EIO` | -4 | I/O error (open, write, rename failed) |
| `CAS_ETYPE` | -5 | Object type mismatch (e.g. opening a tree as a blob) |

```c
const char *
cas_strerror(int err);
```

Returns a static string describing the error code.

### CAS data structures

**`struct cas`** -- Opaque store handle.

**`struct cas_file`** -- Handle to an open blob.

| Field | Type | Description |
|-------|------|-------------|
| `data` | `const unsigned char *` | Pointer to mmap'd content (NULL for empty blobs) |
| `len` | `size_t` | Size in bytes |

### CAS lifecycle

```c
struct cas *
cas_new(const char *basedir);
```

Create a new CAS handle rooted at `basedir`.  If `basedir` is NULL,
defaults to `"depot"`.  The directory is created lazily on the first
`cas_put`.  Returns NULL on allocation failure.

```c
void
cas_free(struct cas *store);
```

Destroy the handle and free memory.  Does not delete any on-disk data.
Safe to call with NULL.

```c
const char *
cas_basedir(struct cas *store);
```

Return the base directory path for the store.

### CAS operations

```c
int
cas_put(struct cas *store, const void *data, size_t len,
        char *hash_out);
```

Store a blob and write its hex hash to `hash_out`.  The buffer must be at
least `CAS_HASH_HEX + 1` (65) bytes.  If a blob with the same hash
already exists, the write is skipped (content-addressable deduplication).
Writes are atomic: data goes to a temporary file, then is renamed into
place.

**Returns:** `CAS_OK` on success.  `CAS_ERR` on header formatting
failure or path overflow.  `CAS_EIO` on I/O failure.

```c
int
cas_open(struct cas *store, struct cas_file *cf, const char *hash);
```

Open a blob for reading by its hex hash string.  Checks the packfile
first, then falls back to loose objects.  On success, `cf->data` points
to the content and `cf->len` is the size.  For empty blobs, `cf->data`
is NULL and `cf->len` is 0.  The hash must be exactly 64 hex characters.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the blob does not
exist.  `CAS_ERR` if the hash is malformed.

```c
void
cas_close(struct cas_file *cf);
```

Close a previously opened blob, unmapping its memory.

```c
int
cas_exists(struct cas *store, const char *hash);
```

Check whether an object with the given hex hash exists (in the packfile
or as a loose object).

**Returns:** Nonzero if present, 0 if not.

```c
int
cas_hash(const void *data, size_t len, char *hash_out);
```

Compute the BLAKE2b-256 hash of data without storing it.  `hash_out` must
be at least `CAS_HASH_HEX + 1` (65) bytes.

**Returns:** `CAS_OK` (always succeeds for valid inputs).

```c
int
cas_hash_object(const char *type, const void *data, size_t len,
                char *hash_out);
```

Compute the hash of a typed object (`"type len\0" || data`) without storing
it.  Useful for verifying the expected hash of an object before or after
storage.

**Returns:** `CAS_OK`.

### CAS typed object API

```c
int
cas_put_object(struct cas *store, const char *type,
               const void *data, size_t len, char *hash_out);
```

Store a typed object.  On disk, the data is followed by a 64-byte
trailer containing the header string (see Pack format below).  The hash
covers `"type len\0" || data`.  The `type` string must be at
most `CAS_TYPE_MAX` (16) characters.

**Returns:** `CAS_OK` on success.  `CAS_ERR` on header formatting
failure or path overflow.  `CAS_EIO` on I/O failure.

```c
int
cas_open_object(struct cas *store, struct cas_file *cf,
                const char *hash, char *type_out, size_t type_bufsz);
```

Open any object by hash, regardless of type.  On success, `cf->data` and
`cf->len` contain the body (after the header), and the object type is
copied to `type_out` (if non-NULL).  A compressed object is decoded
transparently, provided its codec is in the compile-time codec table;
if not, this returns `CAS_ETYPE`.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the object does not
exist.  `CAS_ETYPE` if the object's codec is not in the codec table.
`CAS_ERR` if the hash is malformed.

### CAS compression

These entry points create or ingest compressed objects.  See the
Compression section under Pack format for the on-disk format and the
CAS codec table below for supplying codecs.

```c
int
cas_put_object_z(struct cas *store, const char *type, int codec,
                 const void *data, size_t len, char *hash_out);
```

Store a typed object, compressing it with `codec` when that saves
enough space.  The object is hashed over its plaintext, so the address
matches `cas_put_object` whether or not compression wins.  The
compressed form is kept only when it beats the raw size by a comfortable
margin and an encoder for `codec` is in the codec table; otherwise the object is
stored raw.  Incompressible data therefore costs only a compression
attempt.

**Returns:** `CAS_OK` on success.  `CAS_ERR` on a bad type or length.
An I/O error code on write failure.

```c
int
cas_put_precompressed(struct cas *store, const char *type, int codec,
                      const void *payload, size_t payload_len,
                      size_t plaintext_len, const char *hash);
```

Store an already-compressed object at its plaintext hash address
without compressing, hashing, or decompressing.  `codec` is the codec
tag describing `payload`; `plaintext_len` is the uncompressed length,
which is what the header records and what the address derives from.  The
caller supplies the canonical `hash`, which is not recomputed or
verified here.  This is the ingest path for pre-compressed downloads.

**Returns:** `CAS_OK` on success (or if the object already exists).
`CAS_ERR` on a bad hash or oversized header.  `CAS_EIO` on I/O failure.

```c
int
cas_open_loose_raw(struct cas *store, struct cas_file *cf,
                   const char *hash, void *trailer_out);
```

Low-level: open a loose object's raw on-disk data region without
decoding it.  `cf->data` points to the stored region (codec framing
intact) and `cf->len` is the region length, not the plaintext length.
The 64-byte trailer is copied to `trailer_out`.  Used by the packer to
roll objects up without inflating compressed ones.  Close with
`cas_close`.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if not a loose
object.  `CAS_ERR` on a bad hash or malformed trailer.  `CAS_EIO` on
I/O failure.

### CAS codec table

Compression codecs are fixed at compile time (`cas-codec.h`).  There is
no runtime registry and no global mutable state: the codec set is a
`static const` table, so it is thread-safe by construction and needs no
initialization step.  Only `CAS_CODEC_NONE` (`%`, raw) is intrinsic.
The bundled DEFLATE codec is compiled in with `-DCAS_WITH_MINIZ`.

```c
int cas_codec_supported(int codec);    /* decoder available? */
int cas_codec_can_encode(int codec);   /* encoder available? */
```

Report whether the codec table has a decoder or encoder for a tag.
`CAS_CODEC_NONE` is always available.

```c
int cas_codec_policy(int policy, int codec, const void *data, size_t len);
```

Decide whether to attempt compression for a blob, returning
`CAS_CODEC_NONE` (store raw) or `codec` (attempt it, subject to the
`cas_put_object_z` size threshold).  The library keeps this policy
minimal and leaves the real authority with the caller, who picks
`policy`:

- `CAS_COMPRESS_NEVER` -- never compress.
- `CAS_COMPRESS_ALWAYS` -- always attempt.
- `CAS_COMPRESS_GUESS` -- attempt only if the data looks like text.

`GUESS` is a data-only heuristic (no filename, no format list): it
examines the fraction of non-text control bytes in the first
`CAS_TEXT_SNIFF` (512) bytes and treats data above
`CAS_TEXT_MAX_NONTEXT_PCT` (10%) as binary.  Text (source, JSON, XML,
CSV, logs) compresses; already-compressed binary (images, audio,
video, archives) is skipped without a wasted attempt.  Both bounds are
compile-time overridable.  Callers use it as:

```c
int use = cas_codec_policy(policy, CAS_CODEC_DEFLATE, data, len);
cas_put_object_z(store, "blob", use, data, len, hash);
```

An application that wants its own codec (for example its own zlib
instead of the bundled miniz) declares its functions and defines
`CAS_CODEC_USER` when compiling `cas-codec.c`.  Each `X` entry becomes
one table row:

```c
int myz_decode(const void *in, size_t inlen, void *out, size_t outlen);
int myz_encode(const void *in, size_t inlen, void *out, size_t *outlen);

#define CAS_CODEC_USER(X) \
    X(CAS_CODEC_DEFLATE, myz_decode, myz_encode, "myzlib")
```

The bundled DEFLATE stream is zlib-wrapped, so a `Z` codec supplied
this way interoperates with the bundled one and with a standard zlib.

### CAS binary hash helpers

```c
void
cas_digest(const void *data, size_t len, unsigned char *out);
```

Compute a raw BLAKE2b-256 digest (32 bytes) without storing.

```c
void
cas_digest_object(const char *type, const void *data, size_t len,
                  unsigned char *out);
```

Compute the binary digest of a typed object (`"type len\0" || data`).

```c
void
cas_hex_encode(const unsigned char *bin, size_t len, char *out);
```

Encode binary bytes to a hex string.  `out` must be at least `len * 2 + 1`
bytes.

```c
int
cas_hex_decode(const char *hex, size_t hexlen,
               unsigned char *out, size_t outsz);
```

Decode a hex string to binary.

**Returns:** `CAS_OK` on success.  `CAS_ERR` if the hex string is
malformed or the output buffer is too small.

```c
int
cas_valid_hash(const char *hash);
```

Validate a hex hash string (64 hex characters).

**Returns:** Nonzero if valid, 0 if not.

### CAS Pack API

```c
int
cas_pack_create(struct cas *store, const char *path);
```

Create a packfile from all loose objects in the store.  If the store has
no loose objects, returns `CAS_OK` without creating a file.

**Returns:** `CAS_OK` on success.

```c
int
cas_pack_create_z(struct cas *store, const char *path, int policy,
                  int codec);
```

Like `cas_pack_create`, but compresses objects with `codec` (a tag from
`cas-codec.h`) under `policy` (a `CAS_COMPRESS_*` mode) into the
packfile where that saves space.  A raw object is compressed only if
the policy selects it, it beats its stored size by a comfortable
margin, and an encoder for `codec` is compiled in; already-compressed
objects are copied unchanged.  Object addresses are unaffected.  Pass
`CAS_COMPRESS_NEVER` for no compression.  This backs `castool pack -z`
(which uses `CAS_COMPRESS_GUESS`) and the `gc` + `pack -z` disk-reclaim
cycle.

**Returns:** `CAS_OK` on success.

```c
struct cas_pack *
cas_pack_open(const char *path);
```

Open a packfile for reading.  Returns NULL on failure (missing file,
corrupt footer, bad checksum).

```c
void
cas_pack_close(struct cas_pack *pack);
```

Close a packfile handle and unmap its memory.  Safe to call with NULL.

```c
int
cas_pack_lookup(struct cas_pack *pack, struct cas_file *cf,
                const char *hash, char *type_out, size_t type_bufsz);
```

Look up an object by hex hash.  On success, `cf->data` and `cf->len`
are set.  The returned `cas_file` has `_map = NULL` so `cas_close()` is
a no-op -- the data lives in the packfile's mmap.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the hash is not
in the packfile.

```c
int
cas_pack_exists(struct cas_pack *pack, const char *hash);
```

Check whether an object exists in the packfile.

**Returns:** Nonzero if present, 0 if not.

```c
uint64_t
cas_pack_count(struct cas_pack *pack);
```

Return the number of objects in the packfile.

```c
int
cas_pack_foreach(struct cas_pack *pack, cas_pack_foreach_fn fn,
                 void *ctx);
```

Iterate over all objects in the packfile.  Calls `fn` with each object's
hex hash.

```c
int
cas_pack_fsck(struct cas_pack *pack, cas_fsck_fn fn, void *ctx);
```

Check integrity of all objects in the packfile by rehashing each object's
data and comparing against the stored hash.  Uses the same `cas_fsck_fn`
callback as `cas_fsck`.

**Returns:** `CAS_OK` if all objects passed, `CAS_ERR` if any failed.

### CAS iteration, integrity, and removal

```c
int
cas_foreach(struct cas *store, cas_foreach_fn fn, void *ctx);
```

Iterate over all objects in the store.  Calls `fn` with each object's hex
hash.  If `fn` returns nonzero, iteration stops.

```c
int
cas_fsck(struct cas *store, cas_fsck_fn fn, void *ctx);
```

Check integrity of all objects by decoding (if compressed) and rehashing
each file, comparing to its filename.  Calls `fn` for each object with a
status code (`CAS_FSCK_OK`, `CAS_FSCK_CORRUPT`, `CAS_FSCK_BADNAME`,
`CAS_FSCK_IOERR`, `CAS_FSCK_NOCODEC`).  `CAS_FSCK_NOCODEC` means the
object is compressed with a codec that is not compiled in, so it could
not be verified; it is reported but not counted as a failure.

**Returns:** `CAS_OK` if all objects passed (skips do not fail the run),
`CAS_ERR` if any object was corrupt or unreadable.

```c
int
cas_fsck_object(struct cas *store, const char *hash);
```

Check integrity of a single object.

**Returns:** `CAS_FSCK_OK` if valid, `CAS_FSCK_CORRUPT` if the rehash
does not match, `CAS_FSCK_BADNAME` if the hash string is invalid,
`CAS_FSCK_IOERR` if the object cannot be read.

```c
int
cas_remove(struct cas *store, const char *hash);
```

Delete a single object by hash.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the object does not
exist.

```c
int
cas_object_mtime(struct cas *store, const char *hash, time_t *mtime_out);
```

Get the modification time of a loose object.  Used by GC to implement
the grace period.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the object does not
exist as a loose file.

### CAS constants

| Constant | Value | Description |
|----------|-------|-------------|
| `CAS_HASH_LEN` | 32 | BLAKE2b-256 digest size in bytes |
| `CAS_HASH_HEX` | 64 | Hex-encoded digest length (not counting NUL) |
| `CAS_TYPE_MAX` | 16 | Maximum length of object type string (legacy) |
| `CAS_PACK_BLOCK` | 64 | On-disk block size for trailers, index entries, footer |
| `CAS_PACK_HEADER_LEN` | 56 | Usable header space in a trailer (type: 8 + len: 18) |

## Design

### Architecture

```
+---------------+       +---------------+
|  vfs.h        |       |  castool.c    |     CLI admin/debug tool
|  vfs.c        |       +---------------+
+---------------+              |
       |                       v
       v  (optional)    +---------------+
       +--------------->|  cas-tree.h   |     directory trees, refs,
                        |  cas-tree.c   |     snapshots, commits
                        +---------------+
                               |
                               v
                        +---------------+     +---------------+
                        |  cas.h        |---->|  cas-pack.h   |
                        |  cas.c        |     |  cas-pack.c   |
                        +---------------+     +---------------+
                        cas - content-        packfile format
                        addressable storage
                             |          (on-disk structures)
                             v
                        +---------------+     +--------------------+
                        |  cas-codec.h  |     |  cas-codec-miniz.c |
                        |  cas-codec.c  |<----|  third_party/miniz |
                        +---------------+     +--------------------+
                        codec table           optional DEFLATE
                        (only NONE built in)  (-DCAS_WITH_MINIZ)
```

The VFS provides an in-memory filesystem API (usable standalone).  `castool`
is a standalone CLI that operates directly on CAS-Tree depots.

Each module exposes an opaque handle created with a `_new` function and
destroyed with `_free`.  All operations take the handle as the first
argument.  There is no global state.

Each layer is usable independently: CAS on its own for raw blob storage,
CAS-Tree on CAS for persistent directory structures, VFS on its own for
an in-memory filesystem, or VFS backed by CAS-Tree for persistent
versioned storage.

### VFS internals

The VFS stores all nodes in a flat dynamic array (`struct vfs_node[]`)
inside the opaque `struct vfs`.  Each node holds its full normalized
absolute path as a heap-allocated string, plus type, permissions, owner,
timestamps, and a `malloc`'d copy of the file data.

```c
struct vfs_node {
    char           *path;   /* normalized absolute path */
    enum vfs_type   type;   /* VFS_FILE or VFS_DIR */
    int             mode;   /* permission bits (0777) */
    int             owner;  /* owner UID */
    uint64_t        size;   /* file size in bytes */
    time_t          ctime;  /* creation time */
    time_t          mtime;  /* last modification time */
    void           *data;   /* file contents (malloc'd) */
};

struct vfs {
    struct vfs_node *nodes;  /* dynamic array */
    int              count;  /* number of live nodes */
    int              cap;    /* allocated capacity */
    uint64_t         usage;  /* total bytes in files */
    uint64_t         quota;  /* max bytes, 0 = unlimited */
    int              umask;  /* default umask */
};
```

**Node lookup** is a linear scan comparing normalized path strings.  This
is intentional -- the target use cases involve hundreds to low thousands
of nodes, where a linear scan over contiguous memory is fast and avoids
the complexity of a hash table or tree index.

**Deletion** uses swap-removal: the deleted node is overwritten with the
last node in the array and the count is decremented.  This is O(1) but
means node order is not stable across deletions.

**The root directory** (`/`) is implicit.  It is never stored as a node;
`vfs_stat` and `vfs_list` special-case it.  This avoids edge cases
around deleting or modifying root.

**Path validation** rejects control characters (bytes < 0x20) and the
characters `:/\*?"<>|` in filename components.  This prevents path
traversal and keeps paths portable across platforms.

**Permission model:** Each operation that checks access calls an internal
`check_perm()` function that tests owner, group, and other bits in that
order.  Admin callers (`is_admin = 1`) bypass all checks.  The
permission argument to `check_perm` is shifted based on the caller's
relationship to the node (owner bits at position 6, group at 3, other at
0).

**Quota accounting:** `fs->usage` tracks total file bytes.  It is
updated on every write and delete.  `vfs_write` checks the quota before
allocating new data, accounting for the size difference when overwriting
an existing file.

### CAS internals

#### Object format

Every CAS object -- whether a blob or a tree -- has a header string:

```
"<type> <length>\0"
```

The hash covers `"type length\0" || data`.  For example, storing the
string "hello world" produces:

```
hash("blob 11\0hello world")
```

On disk, this header is stored as a 64-byte trailer appended after the
data (see Pack format below).  The trailer contains the header string
nul-padded to a fixed size, preceded by a magic value.  Readers
`stat()` the file and `pread()` the last 64 bytes to extract type and
length.  The object is self-describing: `type` identifies what kind of
object it is, and `length` provides an integrity check independent of
the hash.

Object types:

| Type   | Description |
|--------|-------------|
| `blob` | Raw file content |
| `tree` | Directory listing (see Tree layer below) |

#### Depot layout

The CAS currently uses a two-level directory layout under the configured
base directory (one object per file):

```
depot/
  bd/
    bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319
  a1/
    a1b2c3d4...
```

The first two hex characters of the hash form the subdirectory name.  This
limits the number of entries per directory to 256 subdirectories at the
top level and distributes blobs roughly evenly across them.

Loose objects can be rolled into a single packfile (`pack.dat`) using
`castool pack` or `cas_pack_create()`.  On startup, `cas_new()` opens
`pack.dat` if it exists and uses it as a read-through fallback for
lookups.  See Pack format below for the on-disk layout.

#### Hashing

Uses an embedded BLAKE2b-256 implementation (RFC 7693).  The
implementation is self-contained in `cas.c` with no external dependencies.
It consists of the standard BLAKE2b compression function, the sigma
permutation table, and the IV constants.  The digest is 32 bytes, encoded
as 64 lowercase hex characters.

#### Atomic writes

`cas_put` writes objects atomically:

1. Compute the BLAKE2b-256 hash over `"blob <len>\0" || data`.
2. Check if the blob already exists (loose file via `access(path, F_OK)`
   or in a pack via `cas_pack_exists`).  If so, return immediately
   (deduplication).
3. Create the base directory and hash-prefix subdirectory if needed.
4. Write the header and data to a temporary file via `mkstemp` in the
   target subdirectory.
5. `rename` the temporary file to the final path.  Rename is atomic
   on POSIX filesystems, so readers never see a partial write.
6. If the rename fails because another writer won the race, the
   temporary file is cleaned up and success is returned (the blob
   exists either way).

#### Reading

`cas_open` uses `mmap(PROT_READ, MAP_PRIVATE)` for zero-copy access to
blob contents.  The file descriptor is closed immediately after mapping.
`cas_close` calls `munmap`.  Empty blobs (zero-length files) are handled
as a special case with `data = NULL`.

#### Hash validation

All hash strings passed to `cas_open` and `cas_exists` are validated for
exact length (64 characters) and hex content before being used in path
construction.  This prevents directory-traversal attacks through crafted
hash strings.

#### Code organization

Both `vfs.c` and `cas.c` define their private structures directly in the
`.c` file, exposing only the opaque forward declarations through the
public headers.

### CAS-Tree API

#### CAS-Tree lifecycle

```c
struct cas_tree *
cas_tree_new(struct cas *store);
```

Create a CAS-Tree handle backed by the given CAS store.

```c
struct cas *
cas_tree_cas(struct cas_tree *ct);
```

Return the underlying CAS store handle.

```c
void
cas_tree_free(struct cas_tree *ct);
```

Destroy a CAS-Tree handle.  Does not free the underlying CAS store.

#### Directory building

```c
void
cas_tree_dir_init(struct cas_tree_dir *dir);

void
cas_tree_dir_free(struct cas_tree_dir *dir);

int
cas_tree_dir_add(struct cas_tree_dir *dir,
                 const struct cas_tree_entry *e);
```

Initialize, free, or add entries to an in-memory directory listing.
`cas_tree_dir_add` validates the entry name (no slashes, no newlines,
max 255 characters) and grows the internal array as needed.

#### Tree store and load

```c
int
cas_tree_store(struct cas_tree *ct, struct cas_tree_dir *dir,
               char *hash_out);
```

Serialize a directory as a "tree" object and store it in CAS.  Entries
are sorted by name for deterministic hashing.

```c
int
cas_tree_load(struct cas_tree *ct, const char *hash,
              struct cas_tree_dir *dir);
```

Load a "tree" object from CAS into a directory listing.  The caller
must call `cas_tree_dir_free` when done.

#### Refs and log

```c
int
cas_tree_ref_read(struct cas_tree *ct, const char *name,
                  char *hash_out);
```

Read the current root hash for a named ref.

**Returns:** `CAS_OK` on success.  `CAS_ENOTFOUND` if the ref does not
exist.

```c
int
cas_tree_ref_commit(struct cas_tree *ct, const char *name,
                    const char *root_hash, const char *comment);
```

Atomically commit a new root hash to a named ref.  Appends to the log,
preserves the previous root in `.prev`, and updates `.root` via
temp+rename.  Serialized via `flock()`.  Input validation rejects
invalid ref names and non-hex hashes.

```c
int
cas_tree_log_read(struct cas_tree *ct, const char *name,
                  cas_tree_log_fn fn, void *ctx);
```

Read the commit log for a ref.  Calls `fn` for each entry with the root
hash, timestamp, and comment.  Tolerates crash-truncated final lines.

```c
int
cas_tree_ref_foreach(struct cas_tree *ct, cas_tree_ref_fn fn,
                     void *ctx);
```

Iterate over all refs in the store.

#### CAS-Tree fsck

```c
int
cas_tree_fsck(struct cas_tree *ct, cas_tree_fsck_fn fn, void *ctx);
```

Walk all refs, verify reachable tree structure and blobs.  Calls `fn`
for each issue with a status code (`CAS_TREE_FSCK_OK`, `_MISSING`,
`_CORRUPT`, `_BAD_TREE`, `_NOCODEC`).  `_NOCODEC` marks a compressed
object whose codec is not compiled in: it is reported as a skip, not
counted as an error, and its subtree is left unchecked.

**Returns:** `CAS_OK` if everything is clean (skips do not fail the
run), `CAS_ERR` on any issue.

```c
int
cas_tree_fsck_root(struct cas_tree *ct, const char *root_hash,
                   cas_tree_fsck_fn fn, void *ctx);
```

Fsck a single tree root by hash, recursively.

#### CAS-Tree garbage collection

```c
int
cas_tree_gc(struct cas_tree *ct, time_t grace, cas_tree_gc_fn fn,
            void *ctx, int *removed);
```

Remove unreachable objects older than `grace` seconds.  The mark phase
walks all refs and all log entries (preserving historical snapshots).
The sweep phase checks each unreachable object's mtime and only deletes
it if the object is at least `grace` seconds old.  Pass 0 to skip the
grace period and delete all unreachable objects immediately.

Sets `*removed` to the count of deleted objects (if non-NULL).  Calls
`fn` for each removed hash.

### VFS-Snap API

The snapshot module (`vfs-snap.h` / `vfs-snap.c`) ties VFS and CAS-Tree
together, providing persistence and integrity checking for in-memory
filesystem state.

#### Snapshot and restore

```c
int
vfs_snap_store(struct vfs *fs, const struct vfs_cred *cred,
               struct cas_tree *ct, char *hash_out);
```

Recursively store the entire VFS state as CAS tree objects.  Returns the
root tree hash in `hash_out`.

```c
int
vfs_snap_store_z(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, int policy, int codec,
                 char *hash_out);
```

Like `vfs_snap_store`, but compresses file blobs according to `policy`
(a `CAS_COMPRESS_*` mode) with `codec`.  Because blobs are stored at
their plaintext address, the tree hashes are identical to an
uncompressed snapshot, and unchanged files are not recompressed on a
repeat snapshot (they already exist).  Restoring needs the matching
decoder compiled in.  There is a matching `vfs_snap_commit_z` that
snapshots with compression and commits to a ref.

```c
int
vfs_snap_restore(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, const char *root_hash);
```

Recursively restore a CAS tree into VFS, creating files and directories
under root.

```c
int
vfs_snap_restore_at(struct vfs *fs, const struct vfs_cred *cred,
                    struct cas_tree *ct, const char *base_path,
                    const char *root_hash);
```

Like `vfs_snap_restore`, but lands the tree under an absolute directory
prefix instead of at the root.  This mounts a snapshot as a
subdirectory, for example installing a downloaded module at
`/modules/coolmod` while the rest of the filesystem is untouched.  The
prefix and any missing parents are created, so an empty tree still
yields the directory; passing `"/"` is identical to `vfs_snap_restore`.
Because the target VFS shares one CAS, mounting several bundles dedups
identical objects across them for free.

#### Ref convenience

```c
int
vfs_snap_commit(struct vfs *fs, const struct vfs_cred *cred,
                struct cas_tree *ct, const char *ref,
                const char *comment);
```

Snapshot VFS and commit to a named ref in one call.

```c
int
vfs_snap_checkout(struct vfs *fs, const struct vfs_cred *cred,
                  struct cas_tree *ct, const char *ref);
```

Read a ref and restore into VFS.

```c
int
vfs_snap_checkout_at(struct vfs *fs, const struct vfs_cred *cred,
                     struct cas_tree *ct, const char *base_path,
                     const char *ref);
```

Read a ref and restore into VFS under an absolute directory prefix (see
`vfs_snap_restore_at`).

#### Integrity checking

```c
int
vfs_snap_checkfile(struct vfs *fs, const struct vfs_cred *cred,
                   struct cas_tree *ct, const char *root_hash,
                   const char *path);
```

Check a single VFS file against a snapshot tree.  Reads the file content,
computes its blob hash, and compares with the tree entry hash.

**Returns:** `CAS_OK` if the content matches.  `VFS_ENOTFOUND` if the
path does not exist in VFS.  `CAS_ENOTFOUND` if the path does not exist
in the snapshot.  `VFS_EISDIR` if the VFS path is a directory.
`CAS_ETYPE` if the snapshot entry is a directory but VFS has a file.
`CAS_ERR` if the content has been modified.

```c
int
vfs_snap_fsck(struct vfs *fs, const struct vfs_cred *cred,
              struct cas_tree *ct, const char *root_hash,
              vfs_snap_fsck_fn fn, void *ctx);
```

Compare live VFS state against a snapshot tree.  Walks both the snapshot
tree entries and VFS children at each directory level, reporting
differences via callback.  The callback receives each path and a status
code:

| Status | Meaning |
|--------|---------|
| `VFS_SNAP_FSCK_OK` | File matches snapshot |
| `VFS_SNAP_FSCK_MODIFIED` | File content differs from snapshot |
| `VFS_SNAP_FSCK_ADDED` | File exists in VFS but not in snapshot |
| `VFS_SNAP_FSCK_MISSING` | File exists in snapshot but not in VFS |
| `VFS_SNAP_FSCK_TYPE` | Type mismatch (file vs. directory) |

If the callback returns nonzero, traversal stops.

**Returns:** `CAS_OK` if everything matches, `CAS_ERR` if any
differences were found.

### CAS-Tree internals

A tree object represents a single directory.  Its body is an ASCII text
format listing one entry per line.

#### On-disk format

```
tree <body-length>\0<body>
```

The body begins with a format byte:

| Byte | Meaning |
|------|---------|
| `%`  | Uncompressed plain ASCII |

Future compression schemes (BWT, LZ-family, etc.) will use a different
sigil.  Any tool can branch on the first byte to select the decoder.

#### Entry layout

After the `%` format byte, each entry is a newline-terminated line:

```
<mode> <uid> <gid> <mtime_s> <mtime_ns> <hexhash> <name>\n
```

| Field      | Format          | Description |
|------------|-----------------|-------------|
| `mode`     | octal           | Unix-style type + permissions (e.g. `100644` file, `040755` directory) |
| `uid`      | decimal         | Owner user ID |
| `gid`      | decimal         | Owner group ID |
| `mtime_s`  | decimal         | Last modification time, seconds since epoch |
| `mtime_ns` | decimal         | Nanosecond component of mtime |
| `hexhash`  | 64 hex chars    | BLAKE2b-256 hash of the referenced object |
| `name`     | UTF-8 bytes     | Entry name (single path component, no `/`) |

Fields are separated by single spaces.  Entries are sorted by name for
deterministic hashing -- the same directory contents always produce the
same tree hash.

A tree entry whose mode has the directory type bits (`040xxx`) points
to another tree object.  All other entries point to blob objects.

#### Example

A directory containing two files and a subdirectory:

```
tree 273\0%040755 1000 1000 1714400000 0 e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6 lib
100644 1000 1000 1714400000 0 bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319 hello.txt
100755 1000 1000 1714400000 0 a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2 run.sh
```

(Above shown with `\0` and `\n` made visible.  The header and body are
contiguous bytes with no extra framing.)

#### Design rationale

**ASCII text over binary encoding:** the format code reduces to
`snprintf` / `sscanf`, the data is inspectable with standard tools, and
future compression (applied to the body after the format byte) can
exploit the ASCII structure effectively.  Binary encodings like LEB128
save ~10 bytes per entry but add encode/decode complexity for modest gain.

**mtime is optional:** the tree entry format includes mtime fields, but
`0 0` means "not tracked at CAS level."  The VFS maintains its own
atime, ctime, and mtime in memory and only persists mtime to tree
entries when the caller opts in.  This keeps tree content focused on
structure by default while supporting filesystem-accurate timestamps
for applications that need them.

**Size omitted:** the object's stored header (`"blob <len>\0"`) already
encodes the content length, so duplicating it in the tree entry would be
redundant.

**Mode encodes type:** following Unix `st_mode` conventions, the high
bits of the mode field distinguish files from directories.  No separate
type field is needed.

### Snapshots and refs

#### Ref files

Each named ref is represented by three files under `depot/refs/`:

| File                    | Contents | Update pattern |
|-------------------------|----------|----------------|
| `refs/<name>.root`      | Single line: `<hexhash>\n` -- the current writable root tree | Atomic replace (temp + rename) |
| `refs/<name>.log`       | Append-only commit log, one entry per line | Append + fsync |
| `refs/<name>.prev`      | Copy of the previous `.root` for crash recovery | Atomic replace (link + rename) |

There is no branching or merging -- snapshots are read-only, and only
the root pointed to by `.root` is writable.

**Log entry format:**

```
<hexhash> <time_sec> <time_ns> <comment>
```

Each line records the root tree hash at commit time, a timestamp as
separate second and nanosecond integers, and a comment string (empty
string if none).  Entries are ordered oldest-first, most recent last.

#### Commit workflow

1. The VFS accumulates writes against the in-memory tree backed by the
   current root.
2. On commit, dirty trees are materialized bottom-up: new tree objects
   are written for each modified directory along the path to the root.
   Sibling subtrees are shared by hash (structural sharing), so only
   O(depth) new objects are created per change regardless of tree size.
3. The new root hash is appended to `.log` and fsynced.
4. The current `.root` is preserved to `.prev` (see Atomicity below).
5. `.root` is updated atomically to the new root hash.

The log entry is written before the root is updated.  If the process
crashes between steps 3 and 5, the log records the intended commit but
`.root` still points to the previous state -- consistent, and the
dangling log entry references valid objects that GC will not collect
while the entry remains.

Changes are batched -- the VFS does not propagate tree updates on every
individual write.  Instead, dirty nodes are tracked in memory and only
flushed to CAS on explicit commit.  Writing 100 files in the same
directory produces one new tree object for that directory, not 100
intermediates.

#### Snapshot access

All snapshots in the log are read-only.  The VFS can open any snapshot
by its root hash for read access, and can hold multiple snapshots open
simultaneously (e.g. for diff or migration).  Only the `.root` head
accepts writes.

Old snapshots can be reaped by removing entries from the log.  On the
next GC pass, any objects not reachable from any remaining root (across
all ref files in the depot) are reclaimed.

#### Atomicity and locking

Ref updates are serialized via `flock()` on `depot/refs/<name>.lock`.
The commit sequence:

1. Acquire `flock(LOCK_EX)` on the lock file.
2. Append new entry to `.log`, `fsync`.
3. `link(".root", ".prev.tmp")` then `rename(".prev.tmp", ".prev")`
   -- atomically preserves the old root for recovery.
4. Write new root hash to a temp file, `rename(temp, ".root")`
   -- atomically updates the current root.
5. Release the lock.

Crash safety at each step:

| Crash point   | State |
|---------------|-------|
| After step 2  | Log has entry, root unchanged.  Consistent -- commit did not complete. |
| After step 3  | `.prev` updated, root unchanged.  Consistent. |
| After step 4  | Fully committed.  `.prev` holds the prior root. |

Recovery: if `.root` is damaged or missing, the previous root is
available in `.prev`.  The log provides a full history of committed
roots to fall back further if needed.

Readers do not need the lock -- they read `.root` (atomic rename
guarantees a consistent snapshot) and walk the immutable object graph.

### Concurrency

#### Design principles

Thread safety is a first-class requirement.  The VFS and CAS layers are
designed for safe concurrent access from multiple threads within a single
process.

**Restriction:** the depot must reside on a local filesystem.  `flock()`
semantics and `rename()` atomicity are not guaranteed over NFS, FUSE, or
other network/virtual filesystems.  Running a depot on network storage
is unsupported.

#### CAS concurrency

- **Reads** are fully concurrent.  `cas_open` uses `mmap(MAP_PRIVATE)`
  which is independent per caller.  No locking required.
- **Writes** are nearly lock-free.  `cas_put` writes to a temporary
  file and renames into place -- if two threads write the same blob
  concurrently, both succeed and the content is identical (content-
  addressed deduplication).
- **Ref updates** are serialized via `flock()` on a per-ref lock file.
  Only the commit path contends for this lock.
- **Garbage collection** uses a grace period to run safely alongside
  concurrent writers.  `cas_tree_gc` takes a `grace` parameter
  (seconds); unreachable objects younger than the grace period are
  kept.  A writer that creates a new object and commits a ref pointing
  to it is safe as long as the window between `cas_put` and
  `cas_tree_ref_commit` is shorter than the grace period -- which is
  always true in practice.  A grace period of 3600 (one hour) is a
  reasonable default.  Pass 0 only when you have exclusive access to
  the store.

#### VFS concurrency

The VFS is currently single-threaded.  All operations on a `struct vfs`
handle must be serialized by the caller.  A future version may add
internal locking, but for now the API is not thread-safe.

### Backup and recovery

The depot is almost entirely immutable, content-addressed files, making
it naturally friendly to standard backup tools.

#### rsync

Ideal for incremental local or remote backups.  Objects never change
after creation, so rsync only transfers new files on each run.

If the depot is active during the rsync, a concurrent commit could cause
rsync to copy a new `.root` while missing some newly written objects.
Two mitigations:

1. **Quiesce the depot** before running rsync (simplest).
2. **Two-pass rsync:** sync objects first (`depot/[0-9a-f][0-9a-f]/`),
   then refs (`depot/refs/`).  Since objects are always written before
   the ref is updated, the second pass captures a consistent state.

#### tar

Works for point-in-time snapshots.  For a guaranteed-consistent archive
when the depot is active, take a filesystem snapshot first (LVM, btrfs
subvolume snapshot, ZFS snapshot) and tar that.

#### restic / borg

Block-level deduplicating backup tools are a natural fit.  They take
atomic point-in-time snapshots and their own deduplication stacks well
with CAS -- similar blobs are stored efficiently on the backup side too.
No special handling needed.

#### Crash and corruption recovery

The commit ordering (objects -> log -> root) means a partial or
interrupted backup always lands in a recoverable state:

| Scenario | State | Recovery |
|----------|-------|----------|
| `.root` missing or corrupt | Objects and log intact | Read `.prev` or last entry in `.log` to find the most recent valid root |
| `.log` truncated | `.root` and objects intact | Current state is valid; log history is partial |
| Orphan objects (no root references them) | Harmless | Next GC pass reclaims them |

These are the same states produced by a crash during a normal commit, so
the existing fsck and GC paths handle all backup recovery scenarios.

#### Export / import

The `castool` CLI provides file-level import and export:

- `castool import <ref> <file>...` reads files from the local filesystem
  into a CAS-tree ref, preserving permissions and metadata.
- `castool export <ref-or-hash> <destdir>` writes a tree snapshot to a
  directory, recreating the stored permissions.

For depot-to-depot transfer, rsync remains the recommended approach (see
rsync section above).  A streaming pack format for tool-independent
depot-to-depot transfer is a possible future addition.

### Pack format

Defined in `cas-pack.h`.  All on-disk structures are 64-byte blocks,
packing cleanly into power-of-2 system pages.

#### Object trailer (64 bytes)

Both loose objects and packfile entries use the same trailer format,
appended after the data:

```
[ data bytes ] [ magic(8) | header(56 nul-padded) ]
```

The header field contains `"type len\0"` (the same string used as hash
input), padded with nul bytes to 56.  Max type name: 8 characters.  Max
length field: 18 decimal digits (~4.6 EB).

To read a loose object: `stat()` the file, `pread()` the last 64 bytes,
verify magic, parse header up to the first `'\0'`.  Data occupies
`[0, filesize - 64)`.

Hash input is `BLAKE2b-256("type len\0" || data)` -- the nul padding in
the trailer is not part of the hash.  Hashing type before data prevents
type-confusion attacks regardless of hash algorithm properties.

#### Packfile layout

```
[ obj0_data | obj0_trailer(64) |
  obj1_data | obj1_trailer(64) |
  ...
  objN_data | objN_trailer(64) |
  index_entry_0(64) | ... | index_entry_N(64) |
  footer(64) ]
```

Objects are written with new data going to loose files first (atomic
via mkstemp + rename), then GC or an explicit pack command rolls loose
objects into a packfile and removes the originals.

#### Index entry (64 bytes)

```
hash[32] + offset[8] + reserved[24]
```

Entries are sorted by hash for binary search.  The offset points to
the object's 64-byte trailer within the packfile.  To read:

1. Binary search the index by hash.
2. `pread(fd, trailer, 64, offset)` -- parse the trailer for type and
   data length.
3. Data is at `[offset - len, offset)`.

The 8-byte offset supports packfiles up to the native filesystem's
maximum file size.  No artificial cap is imposed.

#### Footer (64 bytes)

```
magic[8] + entry_count[8] + checksum[32] + reserved[16]
```

The checksum is BLAKE2b-256 over the concatenation of all index
entries and the first 16 bytes of the footer (magic + entry_count).
Object data is not included -- each object's integrity is already
covered by its own hash in the index.

#### Fsck levels

- **Quick:** read the footer, verify the index checksum.  Confirms
  the index and footer are intact.
- **Full:** also verify each object's data against its hash in the
  index by reading the trailer and rehashing `"type len\0" || data`.

#### Lookup order

1. Check packfile index for hash -- if found, offset into packfile.
2. Fall back to loose object -- `stat()`, read trailer, data region.
3. Either way the caller gets a `cas_file` with `data`, `len`, type.

#### Compression

Objects may be stored compressed while keeping the same address.
The hash always covers the uncompressed content (`"type len\0" ||
plaintext`), so compression is purely a property of the on-disk
encoding, never of the object's identity.  Two encodings of the
same content dedup to one address; the first writer wins.

An object's trailer magic selects the data-region encoding:

- **v1** (`CAS_PACK_TRAILER_MAGIC`): the data region is the raw
  plaintext.  This is the default and is byte-identical to earlier
  versions, so existing depots read back unchanged.
- **v2** (`CAS_PACK_TRAILER_MAGIC_V2`): the data region is a
  one-byte codec tag followed by the codec payload.  The trailer
  `len` is still the uncompressed length and still the hash input.

The codec tag is printable ASCII, mirroring the tree body sigil:
`%` none (raw), `Z` DEFLATE, `S` zstd, `X` xz, `4` LZ4.  In a
packfile the on-disk region length is recorded in the index
entry's `stored_size` field (a zero means "same as the plaintext
length", so pre-compression packs are unaffected).

Codecs are fixed at compile time, and only `%` (none) is intrinsic.
Any other codec is compiled into a `static const` table (see the
CAS codec table below), so the core has no compression dependency
and no runtime registry.  An optional bundled DEFLATE codec (miniz)
is compiled in with `-DCAS_WITH_MINIZ`.  Reading an object whose
codec is not in the table returns `CAS_ETYPE` rather than corrupt
data.

Because a compressed object's plaintext length comes from its
trailer (which the address does not cover on an untrusted read),
decoding caps how far an object may inflate before allocating:
`CAS_CODEC_MAX_PLAINTEXT` (default 1 GiB) and `CAS_CODEC_MAX_RATIO`
(default 4096:1), both compile-time overridable.  An object that
claims to exceed either bound is rejected rather than triggering a
large allocation.  The caps apply only to genuinely compressed
codecs; raw and `%` objects are already bounded by their stored
size.

There are three ways to create compressed objects:

- `cas_put_object_z` compresses on write, keeping the compressed
  form only when it beats the raw size by a comfortable margin.
  Already-compressed or incompressible data (PDFs, images,
  archives) costs only a compression attempt and is stored raw.
- `cas_put_precompressed` ingests an already-compressed payload at
  its plaintext address without compressing, hashing, or
  decompressing.  This is ideal for pre-compressed downloads: the
  blob stays small over the wire and on disk, and the client does
  no compression work.  The supplied hash is trusted, not
  recomputed; verify untrusted sources lazily via a later read or
  `fsck`, which decodes and rehashes.
- The producer of a packfile or download may use any codec offline
  (even an expensive `zstd --ultra` or `xz -9`), provided a
  matching decoder is compiled in on the reading side.

### Future investigations

#### Block-level deduplication (content-defined chunking)

For large files where only a small region changes, the current design
stores the entire new blob -- duplicating most of the content.
Content-defined chunking (CDC) with a rolling hash (e.g. Buzhash,
Gear hash) would split files into variable-size blocks at
content-determined boundaries, each stored as its own CAS blob.

A new object type `file` would hold an ordered list of chunk hashes:

```
file <len>\0<chunk-hash-1>\n<chunk-hash-2>\n...
```

The tree entry points to the `file` object; the `file` object points to
chunks.  This forms a Merkle DAG: changing one byte in a large file only
produces a new chunk at the edit site, a new `file` object with an
updated chunk list, and new tree objects up to the root -- everything
else is shared.

Considerations:

- **Chunk size selection** affects dedup ratio vs. object count.  Typical
  target is 4--64 KiB average chunk size.
- **Rolling hash choice** determines split-point stability across edits
  and CPU cost.
- **Random-access reads** require an index or sequential scan of the
  chunk list to locate a byte offset.
- **Interaction with pack format:** chunks are small individual objects,
  so packing becomes important earlier.

This is deferred until demand arrives.  The current architecture
accommodates it: the `%` format byte, typed object headers, and Merkle
tree structure all extend naturally to support chunk-level objects.

## License

Licensed under either of

- BSD 2-Clause Plus Patent License
  ([LICENSE-BSD-2-CLAUSE-PATENT](LICENSE-BSD-2-CLAUSE-PATENT))
- MIT license ([LICENSE-MIT](LICENSE-MIT) or
  http://opensource.org/licenses/MIT)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you shall be dual licensed as
above, without any additional terms or conditions.
