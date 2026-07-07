/* vfs-snap.c : snapshot VFS state to CAS-Tree and restore */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "vfs-snap.h"
#include "cas-codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_PATH_MAX 4096

/****************************************************************
 * Child collection for vfs_list callback
 ****************************************************************/

struct snap_child {
    char path[SNAP_PATH_MAX];
    enum vfs_type type;
    int mode;
    int owner;
    time_t mtime;
    uint64_t size;
};

struct snap_ctx {
    struct snap_child *children;
    int count;
    int cap;
};

static int
collect_child(const struct vfs_stat *st, void *ctx)
{
    struct snap_ctx *sc = ctx;

    if (sc->count >= sc->cap) {
        int newcap = sc->cap ? sc->cap * 2 : 16;
        struct snap_child *p = realloc(sc->children,
            (size_t)newcap * sizeof(*p));

        if (!p)
            return -1;
        sc->children = p;
        sc->cap = newcap;
    }

    struct snap_child *c = &sc->children[sc->count++];

    snprintf(c->path, sizeof(c->path), "%s", st->path);
    c->type = st->type;
    c->mode = st->mode;
    c->owner = st->owner;
    c->mtime = st->mtime;
    c->size = st->size;
    return 0;
}

static void
snap_ctx_free(struct snap_ctx *sc)
{
    free(sc->children);
    sc->children = NULL;
    sc->count = 0;
    sc->cap = 0;
}

/****************************************************************
 * Path helpers
 ****************************************************************/

static const char *
basename_of(const char *path)
{
    const char *last = strrchr(path, '/');

    return last ? last + 1 : path;
}

/****************************************************************
 * Snapshot (VFS -> CAS)
 ****************************************************************/

static int
snap_dir(struct vfs *fs, const struct vfs_cred *cred,
         struct cas_tree *ct, struct cas *store, int policy, int codec,
         const char *dirpath, char *hash_out)
{
    struct snap_ctx ctx = { NULL, 0, 0 };

    int rc = vfs_list(fs, cred, dirpath, collect_child, &ctx);

    if (rc != VFS_OK) {
        snap_ctx_free(&ctx);
        return CAS_ERR;
    }

    struct cas_tree_dir dir;

    cas_tree_dir_init(&dir);

    for (int i = 0; i < ctx.count; i++) {
        struct snap_child *c = &ctx.children[i];
        struct cas_tree_entry e = {0};

        const char *name = basename_of(c->path);
        size_t namelen = strlen(name);

        if (namelen == 0 || namelen > CAS_TREE_NAME_MAX) {
            cas_tree_dir_free(&dir);
            snap_ctx_free(&ctx);
            return CAS_ERR;
        }
        memcpy(e.name, name, namelen + 1);
        e.uid = c->owner;
        e.gid = 0;
        e.mtime_s = (int64_t)c->mtime;
        e.mtime_ns = 0;

        if (c->type == VFS_FILE) {
            e.mode = CAS_TREE_S_IFREG | (c->mode & 0777);

            const void *data;
            size_t len;

            rc = vfs_read(fs, cred, c->path, &data, &len);
            if (rc != VFS_OK) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&ctx);
                return CAS_ERR;
            }

            int use = cas_codec_policy(policy, codec, data, len);

            rc = cas_put_object_z(store, "blob", use,
                                  data ? data : "", len, e.hash);
            if (rc != CAS_OK) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&ctx);
                return rc;
            }
        } else {
            e.mode = CAS_TREE_S_IFDIR | (c->mode & 0777);

            rc = snap_dir(fs, cred, ct, store, policy, codec,
                          c->path, e.hash);
            if (rc != CAS_OK) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&ctx);
                return rc;
            }
        }

        rc = cas_tree_dir_add(&dir, &e);
        if (rc != CAS_OK) {
            cas_tree_dir_free(&dir);
            snap_ctx_free(&ctx);
            return rc;
        }
    }

    snap_ctx_free(&ctx);

    rc = cas_tree_store(ct, &dir, hash_out);
    cas_tree_dir_free(&dir);
    return rc;
}

int
vfs_snap_store(struct vfs *fs, const struct vfs_cred *cred,
               struct cas_tree *ct, char *hash_out)
{
    return snap_dir(fs, cred, ct, cas_tree_cas(ct),
                    CAS_COMPRESS_NEVER, CAS_CODEC_NONE, "/", hash_out);
}

int
vfs_snap_store_z(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, int policy, int codec,
                 char *hash_out)
{
    return snap_dir(fs, cred, ct, cas_tree_cas(ct),
                    policy, codec, "/", hash_out);
}

/****************************************************************
 * Restore (CAS -> VFS)
 ****************************************************************/

static int
restore_dir(struct vfs *fs, const struct vfs_cred *cred,
            struct cas_tree *ct, struct cas *store,
            const char *dirpath, const char *tree_hash)
{
    struct cas_tree_dir dir;

    int rc = cas_tree_load(ct, tree_hash, &dir);

    if (rc != CAS_OK)
        return rc;

    for (int i = 0; i < dir.count; i++) {
        struct cas_tree_entry *e = &dir.entries[i];
        char path[SNAP_PATH_MAX];

        if (strcmp(dirpath, "/") == 0)
            snprintf(path, sizeof(path), "/%s", e->name);
        else
            snprintf(path, sizeof(path), "%s/%s",
                     dirpath, e->name);

        int type = e->mode & CAS_TREE_S_IFMT;

        if (type == CAS_TREE_S_IFREG) {
            struct cas_file cf;

            rc = cas_open(store, &cf, e->hash);
            if (rc != CAS_OK) {
                cas_tree_dir_free(&dir);
                return rc;
            }

            rc = vfs_write(fs, cred, path,
                           cf.data ? (const void *)cf.data : "",
                           cf.len, 1);
            cas_close(&cf);
            if (rc != VFS_OK) {
                cas_tree_dir_free(&dir);
                return CAS_ERR;
            }
        } else if (type == CAS_TREE_S_IFDIR) {
            rc = vfs_mkdir(fs, cred, path, 1);
            if (rc != VFS_OK) {
                cas_tree_dir_free(&dir);
                return CAS_ERR;
            }

            rc = restore_dir(fs, cred, ct, store, path, e->hash);
            if (rc != CAS_OK) {
                cas_tree_dir_free(&dir);
                return rc;
            }
        }
    }

    cas_tree_dir_free(&dir);
    return CAS_OK;
}

int
vfs_snap_restore(struct vfs *fs, const struct vfs_cred *cred,
                 struct cas_tree *ct, const char *root_hash)
{
    return restore_dir(fs, cred, ct, cas_tree_cas(ct), "/",
                       root_hash);
}

/****************************************************************
 * Ref convenience
 ****************************************************************/

int
vfs_snap_commit(struct vfs *fs, const struct vfs_cred *cred,
                struct cas_tree *ct, const char *ref,
                const char *comment)
{
    char hash[CAS_HASH_HEX + 1];

    int rc = vfs_snap_store(fs, cred, ct, hash);

    if (rc != CAS_OK)
        return rc;
    return cas_tree_ref_commit(ct, ref, hash, comment);
}

int
vfs_snap_commit_z(struct vfs *fs, const struct vfs_cred *cred,
                  struct cas_tree *ct, const char *ref,
                  const char *comment, int policy, int codec)
{
    char hash[CAS_HASH_HEX + 1];

    int rc = vfs_snap_store_z(fs, cred, ct, policy, codec, hash);

    if (rc != CAS_OK)
        return rc;
    return cas_tree_ref_commit(ct, ref, hash, comment);
}

int
vfs_snap_checkout(struct vfs *fs, const struct vfs_cred *cred,
                  struct cas_tree *ct, const char *ref)
{
    char hash[CAS_HASH_HEX + 1];

    int rc = cas_tree_ref_read(ct, ref, hash);

    if (rc != CAS_OK)
        return rc;
    return vfs_snap_restore(fs, cred, ct, hash);
}

/****************************************************************
 * Tree path resolution
 ****************************************************************/

/** Walk a CAS tree to find the entry for a given VFS path.
 *
 *  Splits path on '/' and descends through tree objects.
 *  Returns CAS_OK and fills entry_out on success.
 */
static int
tree_resolve(struct cas_tree *ct, const char *root_hash,
             const char *path, struct cas_tree_entry *entry_out)
{
    char buf[SNAP_PATH_MAX];

    snprintf(buf, sizeof(buf), "%s", path);

    char *cur = buf;

    if (*cur == '/')
        cur++;
    if (*cur == '\0')
        return CAS_ENOTFOUND;

    char curhash[CAS_HASH_HEX + 1];

    memcpy(curhash, root_hash, CAS_HASH_HEX + 1);

    for (;;) {
        char *slash = strchr(cur, '/');

        if (slash)
            *slash = '\0';

        struct cas_tree_dir dir;
        int rc = cas_tree_load(ct, curhash, &dir);

        if (rc != CAS_OK)
            return rc;

        int found = 0;

        for (int i = 0; i < dir.count; i++) {
            if (strcmp(dir.entries[i].name, cur) == 0) {
                *entry_out = dir.entries[i];
                found = 1;
                break;
            }
        }
        cas_tree_dir_free(&dir);

        if (!found)
            return CAS_ENOTFOUND;

        if (!slash)
            return CAS_OK;

        if ((entry_out->mode & CAS_TREE_S_IFMT) != CAS_TREE_S_IFDIR)
            return CAS_ETYPE;
        memcpy(curhash, entry_out->hash, CAS_HASH_HEX + 1);
        cur = slash + 1;
    }
}

/****************************************************************
 * Integrity checking: VFS vs snapshot
 ****************************************************************/

int
vfs_snap_checkfile(struct vfs *fs, const struct vfs_cred *cred,
                   struct cas_tree *ct, const char *root_hash,
                   const char *path)
{
    struct vfs_stat st;
    int rc = vfs_stat(fs, cred, path, &st);

    if (rc != VFS_OK)
        return rc;
    if (st.type == VFS_DIR)
        return VFS_EISDIR;

    struct cas_tree_entry entry;

    rc = tree_resolve(ct, root_hash, path, &entry);
    if (rc != CAS_OK)
        return rc;
    if ((entry.mode & CAS_TREE_S_IFMT) == CAS_TREE_S_IFDIR)
        return CAS_ETYPE;

    const void *data;
    size_t len;

    rc = vfs_read(fs, cred, path, &data, &len);
    if (rc != VFS_OK)
        return CAS_ERR;

    char hash[CAS_HASH_HEX + 1];

    cas_hash_object("blob", data ? data : "", len, hash);

    if (strcmp(hash, entry.hash) != 0)
        return CAS_ERR;
    return CAS_OK;
}

static int
fsck_dir(struct vfs *fs, const struct vfs_cred *cred,
         struct cas_tree *ct, const char *dirpath,
         const char *tree_hash, vfs_snap_fsck_fn fn, void *ctx,
         int *errors)
{
    struct cas_tree_dir dir;
    int rc = cas_tree_load(ct, tree_hash, &dir);

    if (rc != CAS_OK)
        return 0;

    struct snap_ctx sc = { NULL, 0, 0 };

    rc = vfs_list(fs, cred, dirpath, collect_child, &sc);
    if (rc != VFS_OK) {
        cas_tree_dir_free(&dir);
        snap_ctx_free(&sc);
        return 0;
    }

    for (int i = 0; i < dir.count; i++) {
        struct cas_tree_entry *e = &dir.entries[i];
        char path[SNAP_PATH_MAX];

        if (strcmp(dirpath, "/") == 0)
            snprintf(path, sizeof(path), "/%s", e->name);
        else
            snprintf(path, sizeof(path), "%s/%s",
                     dirpath, e->name);

        int type = e->mode & CAS_TREE_S_IFMT;

        struct snap_child *match = NULL;

        for (int j = 0; j < sc.count; j++) {
            if (strcmp(basename_of(sc.children[j].path),
                       e->name) == 0) {
                match = &sc.children[j];
                break;
            }
        }

        if (!match) {
            (*errors)++;
            if (fn && fn(path, VFS_SNAP_FSCK_MISSING, ctx)) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&sc);
                return 1;
            }
            continue;
        }

        if ((type == CAS_TREE_S_IFDIR) != (match->type == VFS_DIR)) {
            (*errors)++;
            if (fn && fn(path, VFS_SNAP_FSCK_TYPE, ctx)) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&sc);
                return 1;
            }
            continue;
        }

        if (type == CAS_TREE_S_IFDIR) {
            if (fsck_dir(fs, cred, ct, path, e->hash,
                         fn, ctx, errors)) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&sc);
                return 1;
            }
        } else {
            const void *data;
            size_t len;
            int vrc = vfs_read(fs, cred, path, &data, &len);

            if (vrc != VFS_OK) {
                (*errors)++;
                if (fn && fn(path, VFS_SNAP_FSCK_MODIFIED, ctx)) {
                    cas_tree_dir_free(&dir);
                    snap_ctx_free(&sc);
                    return 1;
                }
                continue;
            }

            char hash[CAS_HASH_HEX + 1];

            cas_hash_object("blob", data ? data : "",
                            len, hash);

            if (strcmp(hash, e->hash) != 0) {
                (*errors)++;
                if (fn && fn(path, VFS_SNAP_FSCK_MODIFIED,
                             ctx)) {
                    cas_tree_dir_free(&dir);
                    snap_ctx_free(&sc);
                    return 1;
                }
            }
        }
    }

    for (int j = 0; j < sc.count; j++) {
        const char *name = basename_of(sc.children[j].path);
        int found = 0;

        for (int i = 0; i < dir.count; i++) {
            if (strcmp(dir.entries[i].name, name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            char path[SNAP_PATH_MAX];

            if (strcmp(dirpath, "/") == 0)
                snprintf(path, sizeof(path), "/%s", name);
            else
                snprintf(path, sizeof(path), "%s/%s",
                         dirpath, name);
            (*errors)++;
            if (fn && fn(path, VFS_SNAP_FSCK_ADDED, ctx)) {
                cas_tree_dir_free(&dir);
                snap_ctx_free(&sc);
                return 1;
            }
        }
    }

    cas_tree_dir_free(&dir);
    snap_ctx_free(&sc);
    return 0;
}

int
vfs_snap_fsck(struct vfs *fs, const struct vfs_cred *cred,
              struct cas_tree *ct, const char *root_hash,
              vfs_snap_fsck_fn fn, void *ctx)
{
    int errors = 0;

    fsck_dir(fs, cred, ct, "/", root_hash, fn, ctx, &errors);
    return errors ? CAS_ERR : CAS_OK;
}
