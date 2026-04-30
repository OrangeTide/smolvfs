/* cas-pack.h : packfile format for content-addressable store */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#ifndef CAS_PACK_H
#define CAS_PACK_H

#include "cas.h"
#include <stddef.h>
#include <stdint.h>

/****************************************************************
 * On-disk format
 ****************************************************************
 *
 * All structures are 64-byte aligned for clean packing into
 * system pages.
 *
 * Loose object (new format):
 *
 *   [ data bytes ] [ trailer: 64 bytes ]
 *
 * The trailer contains a magic value followed by a nul-padded
 * header string.  The header is "type len\0" matching the hash
 * input format.  Reading: stat() the file, pread() the last 64
 * bytes, verify magic, parse header up to first '\0'.  Data
 * occupies [0, filesize - 64).
 *
 * Hash input: BLAKE2b-256("type len\0" || data).  The nul
 * padding in the trailer is not part of the hash input.
 *
 * Packfile layout:
 *
 *   [ obj0_data | obj0_trailer(64) |
 *     obj1_data | obj1_trailer(64) |
 *     ...
 *     objN_data | objN_trailer(64) |
 *     index_entry_0(64) | ... | index_entry_N(64) |
 *     footer(64) ]
 *
 * The index is sorted by hash for binary search.  Each index
 * entry points to the object's trailer offset within the file.
 * From the trailer, the reader extracts the data length and
 * computes the data region as [offset - len, offset).
 *
 * The footer checksum covers index || footer_without_checksum.
 * Object data integrity is verified per-object via BLAKE2b
 * against the hash stored in the index.
 */

/****************************************************************
 * Constants
 ****************************************************************/

#define CAS_PACK_BLOCK     64

#define CAS_PACK_TRAILER_MAGIC   { 0xCB, 0x4F, 0x42, 0x4A, 0xAA, 0x01, 0x00, 0x00 }
#define CAS_PACK_FOOTER_MAGIC_V1 { 0xCB, 0x50, 0x4B, 0x46, 0xAA, 0x01, 0x00, 0x00 }

#define CAS_PACK_MAGIC_LEN  8
#define CAS_PACK_HEADER_LEN 56  /* 64 - 8 magic */

/****************************************************************
 * On-disk structures (64 bytes each)
 ****************************************************************/

/** Object trailer -- appended after data in both loose objects
 *  and packfile entries.
 *
 *  magic[8] + header[56 nul-padded]
 *  header contains "type len\0" padded with nul bytes to 56.
 *  Max type: 8 chars.  Max len: 18 digits (~4.6 EB).
 */
struct cas_pack_trailer {
	unsigned char magic[CAS_PACK_MAGIC_LEN];
	char header[CAS_PACK_HEADER_LEN];
};

/** Packfile index entry -- one per object, sorted by hash.
 *
 *  hash[32] + offset[8] + reserved[24]
 *  offset points to the start of the object's trailer.
 */
struct cas_pack_index_entry {
	unsigned char hash[CAS_HASH_LEN];
	uint64_t offset;
	unsigned char reserved[24];
};

/** Packfile footer -- last 64 bytes of the file.
 *
 *  magic[8] + entry_count[8] + checksum[32] + reserved[16]
 *  checksum covers: all index entries || footer without checksum
 *  (the first 16 bytes of the footer: magic + entry_count).
 */
struct cas_pack_footer {
	unsigned char magic[CAS_PACK_MAGIC_LEN];
	uint64_t entry_count;
	unsigned char checksum[CAS_HASH_LEN];
	unsigned char reserved[16];
};

/****************************************************************
 * Compile-time layout checks
 ****************************************************************/

_Static_assert(sizeof(struct cas_pack_trailer) == CAS_PACK_BLOCK,
               "trailer must be 64 bytes");
_Static_assert(sizeof(struct cas_pack_index_entry) == CAS_PACK_BLOCK,
               "index entry must be 64 bytes");
_Static_assert(sizeof(struct cas_pack_footer) == CAS_PACK_BLOCK,
               "footer must be 64 bytes");

/****************************************************************
 * API
 ****************************************************************/

/** Opaque packfile handle. */
struct cas_pack;

/** Open a packfile for reading. Returns NULL on failure. */
struct cas_pack *
cas_pack_open(const char *path);

/** Close a packfile handle. */
void
cas_pack_close(struct cas_pack *pack);

/** Create a packfile from all loose objects in the store.
 *  Returns CAS_OK on success.  If the store is empty, returns
 *  CAS_OK without creating a file.
 */
int
cas_pack_create(struct cas *store, const char *path);

/** Look up an object in a packfile by hex hash.
 *  On success, cf->data and cf->len are set.  cf->_map is NULL;
 *  cas_close() is a no-op for packfile-backed objects.
 */
int
cas_pack_lookup(struct cas_pack *pack, struct cas_file *cf,
                const char *hash, char *type_out, size_t type_bufsz);

/** Check whether an object exists in a packfile.
 *  Returns nonzero if present.
 */
int
cas_pack_exists(struct cas_pack *pack, const char *hash);

/** Return the number of objects in a packfile. */
uint64_t
cas_pack_count(struct cas_pack *pack);

/** Callback for cas_pack_foreach.  Return 0 to continue. */
typedef int (*cas_pack_foreach_fn)(const char *hash, void *ctx);

/** Iterate over all objects in a packfile. */
int
cas_pack_foreach(struct cas_pack *pack, cas_pack_foreach_fn fn,
                 void *ctx);

/** Check integrity of all objects in a packfile.
 *  Uses cas_fsck_fn from cas.h for per-object reporting.
 */
int
cas_pack_fsck(struct cas_pack *pack, cas_fsck_fn fn, void *ctx);

#endif /* CAS_PACK_H */
