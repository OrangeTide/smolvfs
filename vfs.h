/* vfs.h : in-memory virtual filesystem with Unix-style permissions and quotas */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/****************************************************************
 * Error codes
 ****************************************************************/

enum {
	VFS_OK            =  0,
	VFS_ERR           = -1,
	VFS_ENOTFOUND     = -2,
	VFS_EEXIST        = -3,
	VFS_EPERM         = -4,
	VFS_EQUOTA        = -5,
	VFS_ENOTDIR       = -6,
	VFS_EISDIR        = -7,
	VFS_ENOTEMPTY     = -8,
	VFS_EBADPATH      = -9,
	VFS_ENOMEM        = -10,
};

/** Return a human-readable string for a VFS error code. */
const char *
vfs_strerror(int err);

/****************************************************************
 * Node types and permission bits
 ****************************************************************/

enum vfs_type {
	VFS_FILE,
	VFS_DIR,
};

/* Unix-style permission bits (octal).  Only r/w are enforced;
 * x is stored but never checked by the library. */
enum {
	VFS_OWNER_R = 0400,
	VFS_OWNER_W = 0200,
	VFS_OWNER_X = 0100,
	VFS_GROUP_R = 0040,
	VFS_GROUP_W = 0020,
	VFS_GROUP_X = 0010,
	VFS_OTHER_R = 0004,
	VFS_OTHER_W = 0002,
	VFS_OTHER_X = 0001,
};

#define VFS_MODE_FILE_DEFAULT  0666
#define VFS_MODE_DIR_DEFAULT   0777

/****************************************************************
 * Data structures
 ****************************************************************/

/** Metadata returned by vfs_stat(). */
struct vfs_stat {
	const char     *path;
	enum vfs_type   type;
	int             mode;       /* permission bits (masked to 0777) */
	int             owner;      /* owner id */
	uint64_t        size;       /* bytes (files only) */
	time_t          ctime;
	time_t          mtime;
};

/** Opaque filesystem handle. */
struct vfs;

/** Options for creating a new filesystem. */
struct vfs_opts {
	uint64_t        quota;      /* max bytes, 0 = unlimited */
	int             umask;      /* default umask for new nodes */
};

/****************************************************************
 * Lifecycle
 ****************************************************************/

/** Create a new empty filesystem.  Returns NULL on alloc failure. */
struct vfs *
vfs_new(const struct vfs_opts *opts);

/** Destroy a filesystem and free all memory. */
void
vfs_free(struct vfs *fs);

/****************************************************************
 * Identity context
 ****************************************************************/

/** Caller identity passed to permission-checked operations.
 *
 * Set is_owner to nonzero if the caller owns the node being
 * accessed.  is_group means the caller belongs to the node's
 * group.  is_admin bypasses all permission checks.
 */
struct vfs_cred {
	int             uid;        /* user id stamped on creates/writes */
	int             is_admin;   /* bypass all permission checks */
	int             is_group;   /* member of node's group */
};

/****************************************************************
 * Operations
 ****************************************************************/

/** Stat a path.  Fills *st on success. */
int
vfs_stat(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, struct vfs_stat *st);

/** List direct children of a directory.
 *
 * Calls fn() for each child.  If fn() returns nonzero, iteration
 * stops and that value is returned.  Returns VFS_OK when all
 * children have been visited.
 */
int
vfs_list(struct vfs *fs, const struct vfs_cred *cred,
         const char *dirpath,
         int (*fn)(const struct vfs_stat *st, void *ctx),
         void *ctx);

/** Read file contents.  Sets *out and *out_len on success.
 *
 * The returned pointer is valid until the node is modified or
 * the filesystem is freed.  Do not free it.
 */
int
vfs_read(struct vfs *fs, const struct vfs_cred *cred,
         const char *path, const void **out, size_t *out_len);

/** Write (create or overwrite) a file.
 *
 * If create_parents is nonzero, missing ancestor directories are
 * created automatically with default mode minus umask.
 */
int
vfs_write(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, const void *data, size_t len,
          int create_parents);

/** Create a directory.
 *
 * If create_parents is nonzero, behaves like mkdir -p: creates
 * ancestors as needed and does not error if the directory exists.
 */
int
vfs_mkdir(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int create_parents);

/** Delete a file or empty directory. */
int
vfs_delete(struct vfs *fs, const struct vfs_cred *cred,
           const char *path);

/** Delete a directory and all of its contents recursively. */
int
vfs_delete_recursive(struct vfs *fs, const struct vfs_cred *cred,
                     const char *path);

/** Rename or move a node within the filesystem. */
int
vfs_rename(struct vfs *fs, const struct vfs_cred *cred,
           const char *old_path, const char *new_path);

/** Copy a file.  Directories are not supported (returns VFS_EISDIR). */
int
vfs_copy(struct vfs *fs, const struct vfs_cred *cred,
         const char *src_path, const char *dst_path,
         int create_parents);

/** Change permission bits (admin only). */
int
vfs_chmod(struct vfs *fs, const struct vfs_cred *cred,
          const char *path, int mode);

/****************************************************************
 * Quota and usage
 ****************************************************************/

/** Return total bytes used by all files. */
uint64_t
vfs_usage(struct vfs *fs);

/** Return configured quota (0 = unlimited). */
uint64_t
vfs_quota(struct vfs *fs);

/****************************************************************
 * Path utilities
 ****************************************************************/

/** Normalize a path in-place.  Collapses duplicate slashes,
 *  resolves . and .. (clamped at root), strips trailing slash.
 *  Returns VFS_EBADPATH if the path is empty or doesn't start
 *  with '/'.
 */
int
vfs_normalize(char *buf, size_t bufsz, const char *path);

/** Resolve a possibly-relative path against a cwd.
 *  Result written to buf.  Returns VFS_EBADPATH on error.
 */
int
vfs_resolve(char *buf, size_t bufsz,
            const char *path, const char *cwd);

/****************************************************************
 * Pattern matching
 ****************************************************************/

/** Match a filename against a glob pattern (case-insensitive).
 *
 * Supports *, ?, [abc], [a-z], [!a].  Matches the filename
 * component only, not full paths.  Returns nonzero on match.
 */
int
vfs_fnmatch(const char *pattern, const char *name);

#endif /* VFS_H */
