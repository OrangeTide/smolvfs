/* vfs-snap.h : snapshot VFS state to CAS-Tree and restore */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#ifndef VFS_SNAP_H
#define VFS_SNAP_H

#include "cas-tree.h"
#include "vfs.h"

/****************************************************************
 * Snapshot: VFS -> CAS
 ****************************************************************/

/** Recursively store VFS state as CAS tree objects.
 *  Returns the root tree hash in hash_out.
 */
int
vfs_snap_store(struct vfs *fs, const struct vfs_cred *cred,
               struct cas_tree *ct, char *hash_out);

/** Like vfs_snap_store, but compresses file blobs per policy (a
 *  CAS_COMPRESS_* mode from cas-codec.h) with codec.  File contents
 *  are stored at their plaintext address regardless, so the tree
 *  hashes are identical to an uncompressed snapshot.  Reading the
 *  snapshot back needs the matching decoder compiled in.
 */
int
vfs_snap_store_z(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, int policy, int codec,
                 char *hash_out);

/****************************************************************
 * Restore: CAS -> VFS
 ****************************************************************/

/** Recursively restore a CAS tree into VFS.
 *  Writes files and creates directories under root.
 */
int
vfs_snap_restore(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, const char *root_hash);

/** Restore a CAS tree into VFS under an absolute directory prefix.
 *  The tree's entries land under base_path (for example mounting a
 *  downloaded module at "/modules/coolmod"); the prefix and any
 *  missing parents are created, so an empty tree still yields the
 *  directory.  Passing "/" is equivalent to vfs_snap_restore.
 *  base_path must be absolute.
 */
int
vfs_snap_restore_at(struct vfs *fs, const struct vfs_cred *cred,
                    struct cas_tree *ct, const char *base_path,
                    const char *root_hash);

/****************************************************************
 * Ref convenience
 ****************************************************************/

/** Snapshot VFS and commit to a named ref. */
int
vfs_snap_commit(struct vfs *fs, const struct vfs_cred *cred,
                struct cas_tree *ct, const char *ref,
                const char *comment);

/** Snapshot with compression (see vfs_snap_store_z) and commit to a
 *  named ref. */
int
vfs_snap_commit_z(struct vfs *fs, const struct vfs_cred *cred,
                  struct cas_tree *ct, const char *ref,
                  const char *comment, int policy, int codec);

/** Read a ref and restore into VFS. */
int
vfs_snap_checkout(struct vfs *fs, const struct vfs_cred *cred,
                  struct cas_tree *ct, const char *ref);

/** Read a ref and restore into VFS under an absolute directory
 *  prefix (see vfs_snap_restore_at). */
int
vfs_snap_checkout_at(struct vfs *fs, const struct vfs_cred *cred,
                     struct cas_tree *ct, const char *base_path,
                     const char *ref);

/****************************************************************
 * Integrity checking: VFS vs snapshot
 ****************************************************************/

/** Check a single VFS file against a snapshot tree.
 *
 * Returns CAS_OK if the file content matches the snapshot.
 * VFS_ENOTFOUND if the path does not exist in VFS.
 * CAS_ENOTFOUND if the path does not exist in the snapshot.
 * VFS_EISDIR if the VFS path is a directory.
 * CAS_ETYPE if the snapshot entry is a directory but VFS has a file.
 * CAS_ERR if the content has been modified.
 */
int
vfs_snap_checkfile(struct vfs *fs, const struct vfs_cred *cred,
                   struct cas_tree *ct, const char *root_hash,
                   const char *path);

/** Fsck status codes for vfs_snap_fsck callback. */
enum {
	VFS_SNAP_FSCK_OK,
	VFS_SNAP_FSCK_MODIFIED,
	VFS_SNAP_FSCK_ADDED,
	VFS_SNAP_FSCK_MISSING,
	VFS_SNAP_FSCK_TYPE,
};

/** Callback for vfs_snap_fsck.
 *
 * Called for each path with a status code.
 * Return 0 to continue, nonzero to stop.
 */
typedef int (*vfs_snap_fsck_fn)(const char *path, int status,
                                void *ctx);

/** Compare live VFS state against a snapshot tree.
 *
 * Reports added, missing, modified, and type-changed paths via
 * callback. Returns CAS_OK if everything matches, CAS_ERR if any
 * differences found.
 */
int
vfs_snap_fsck(struct vfs *fs, const struct vfs_cred *cred,
              struct cas_tree *ct, const char *root_hash,
              vfs_snap_fsck_fn fn, void *ctx);

#endif /* VFS_SNAP_H */
