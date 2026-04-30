/* cas.h : content-addressable store using BLAKE2b hashing */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#ifndef CAS_H
#define CAS_H

#include <stddef.h>
#include <time.h>

/****************************************************************
 * Constants
 ****************************************************************/

#define CAS_HASH_LEN 32  // BLAKE2b-256 digest bytes
#define CAS_HASH_HEX 64  // hex-encoded digest length
#define CAS_TYPE_MAX 16   // max length of object type string

enum {
    CAS_OK        =  0,
    CAS_ERR       = -1,
    CAS_ENOTFOUND = -2,
    CAS_ENOMEM    = -3,
    CAS_EIO       = -4,
    CAS_ETYPE     = -5,
};

/** Return a human-readable string for a CAS error code. */
const char *
cas_strerror(int err);

/****************************************************************
 * Data structures
 ****************************************************************/

/** Opaque store handle. */
struct cas;

/** Handle to an open object (mmap'd for reading). */
struct cas_file {
    const unsigned char *data;
    size_t len;
    void *_map;
    size_t _maplen;
};

/****************************************************************
 * Lifecycle
 ****************************************************************/

/** Create a new CAS rooted at basedir (e.g. "depot").
 *  Returns NULL on alloc failure.
 */
struct cas *
cas_new(const char *basedir);

/** Destroy a CAS handle and free memory. */
void
cas_free(struct cas *store);

/** Return the base directory path. */
const char *
cas_basedir(struct cas *store);

/****************************************************************
 * Operations
 ****************************************************************/

/** Open a blob by hex hash for reading (mmap).
 *  On success cf->data and cf->len are set.
 *  Returns CAS_ETYPE if the stored object is not a blob.
 */
int
cas_open(struct cas *store, struct cas_file *cf, const char *hash);

/** Open a typed object by hex hash for reading (mmap).
 *  If type_out is non-NULL, the object type is copied there.
 */
int
cas_open_object(struct cas *store, struct cas_file *cf,
                const char *hash, char *type_out, size_t type_bufsz);

/** Close a previously opened object. */
void
cas_close(struct cas_file *cf);

/** Store data as a blob and return its hash as hex.
 *  hash_out must be at least CAS_HASH_HEX + 1 bytes.
 */
int
cas_put(struct cas *store, const void *data, size_t len,
        char *hash_out);

/** Store a typed object and return its hash as hex.
 *  The hash covers "type len\0" || data.
 */
int
cas_put_object(struct cas *store, const char *type,
               const void *data, size_t len, char *hash_out);

/** Check whether an object with the given hex hash exists.
 *  Returns nonzero if present, 0 if not.
 */
int
cas_exists(struct cas *store, const char *hash);

/** Compute the raw BLAKE2b-256 hash of data without storing. */
int
cas_hash(const void *data, size_t len, char *hash_out);

/** Compute the hash of a typed object without storing.
 *  Hashes "type len\0" || data.
 */
int
cas_hash_object(const char *type, const void *data, size_t len,
                char *hash_out);

/****************************************************************
 * Iteration
 ****************************************************************/

/** Callback for cas_foreach.  Called with each object hash.
 *  Return 0 to continue, nonzero to stop.
 */
typedef int (*cas_foreach_fn)(const char *hash, void *ctx);

/** Iterate over all objects in the store. */
int
cas_foreach(struct cas *store, cas_foreach_fn fn, void *ctx);

/****************************************************************
 * Fsck
 ****************************************************************/

/** Result codes for cas_fsck_fn callback. */
enum {
    CAS_FSCK_OK,
    CAS_FSCK_CORRUPT,
    CAS_FSCK_BADNAME,
    CAS_FSCK_IOERR,
};

/** Callback for cas_fsck.  Called for each object checked.
 *  status is one of CAS_FSCK_OK/CORRUPT/BADNAME/IOERR.
 *  Return 0 to continue, nonzero to stop.
 */
typedef int (*cas_fsck_fn)(const char *hash, int status, void *ctx);

/** Check integrity of all objects: rehash and compare.
 *  Returns CAS_OK if all objects passed, CAS_ERR if any failed.
 */
int
cas_fsck(struct cas *store, cas_fsck_fn fn, void *ctx);

/** Check integrity of a single object by hash. */
int
cas_fsck_object(struct cas *store, const char *hash);

/** Delete a single object by hash. */
int
cas_remove(struct cas *store, const char *hash);

/** Get the modification time of a loose object.
 *  Returns CAS_OK on success, CAS_ENOTFOUND if missing.
 */
int
cas_object_mtime(struct cas *store, const char *hash, time_t *mtime_out);

/****************************************************************
 * Binary hash helpers
 ****************************************************************/

/** Compute binary BLAKE2b-256 digest. */
void
cas_digest(const void *data, size_t len, unsigned char *out);

/** Compute binary BLAKE2b-256 digest of a typed object. */
void
cas_digest_object(const char *type, const void *data, size_t len,
                  unsigned char *out);

/** Encode binary to hex string. out must be at least len*2+1 bytes. */
void
cas_hex_encode(const unsigned char *bin, size_t len, char *out);

/** Decode hex string to binary. Returns 0 on success. */
int
cas_hex_decode(const char *hex, size_t hexlen,
               unsigned char *out, size_t outsz);

/** Validate a hex hash string. Returns nonzero if valid. */
int
cas_valid_hash(const char *hash);

#endif /* CAS_H */
