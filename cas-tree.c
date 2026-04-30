/* cas-tree.c : tree-structured content layer on CAS */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas-tree.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define REF_PATH_MAX 512

struct cas_tree {
    struct cas *store;
};

/****************************************************************
 * Lifecycle
 ****************************************************************/

struct cas_tree *
cas_tree_new(struct cas *store)
{
    struct cas_tree *ct = calloc(1, sizeof(*ct));

    if (!ct)
        return NULL;
    ct->store = store;
    return ct;
}

void
cas_tree_free(struct cas_tree *ct)
{
    free(ct);
}

struct cas *
cas_tree_cas(struct cas_tree *ct)
{
    return ct->store;
}

/****************************************************************
 * Directory building
 ****************************************************************/

void
cas_tree_dir_init(struct cas_tree_dir *dir)
{
    dir->entries = NULL;
    dir->count = 0;
    dir->_cap = 0;
}

void
cas_tree_dir_free(struct cas_tree_dir *dir)
{
    free(dir->entries);
    dir->entries = NULL;
    dir->count = 0;
    dir->_cap = 0;
}

int
cas_tree_dir_add(struct cas_tree_dir *dir,
                 const struct cas_tree_entry *e)
{
    size_t nlen = strlen(e->name);

    if (nlen == 0 || nlen > CAS_TREE_NAME_MAX)
        return CAS_ERR;
    if (strchr(e->name, '/') || strchr(e->name, '\n'))
        return CAS_ERR;

    if (dir->count >= dir->_cap) {
        int newcap = dir->_cap ? dir->_cap * 2 : 8;
        struct cas_tree_entry *p = realloc(dir->entries,
            (size_t)newcap * sizeof(*p));

        if (!p)
            return CAS_ENOMEM;
        dir->entries = p;
        dir->_cap = newcap;
    }
    dir->entries[dir->count++] = *e;
    return CAS_OK;
}

/****************************************************************
 * Serialization
 ****************************************************************/

static int
entry_cmp(const void *a, const void *b)
{
    const struct cas_tree_entry *ea = a;
    const struct cas_tree_entry *eb = b;

    return strcmp(ea->name, eb->name);
}

int
cas_tree_store(struct cas_tree *ct, struct cas_tree_dir *dir,
               char *hash_out)
{
    if (dir->count > 1)
        qsort(dir->entries, (size_t)dir->count,
              sizeof(*dir->entries), entry_cmp);

    size_t bodylen = 1;

    for (int i = 0; i < dir->count; i++) {
        struct cas_tree_entry *e = &dir->entries[i];
        int n = snprintf(NULL, 0,
            "%06o %d %d %" PRId64 " %" PRId32 " %s %s\n",
            (unsigned)e->mode, e->uid, e->gid,
            e->mtime_s, e->mtime_ns, e->hash, e->name);

        if (n < 0)
            return CAS_ERR;
        bodylen += (size_t)n;
    }

    char *body = malloc(bodylen + 1);

    if (!body)
        return CAS_ENOMEM;

    body[0] = '%';
    size_t pos = 1;

    for (int i = 0; i < dir->count; i++) {
        struct cas_tree_entry *e = &dir->entries[i];
        int n = snprintf(body + pos, bodylen + 1 - pos,
            "%06o %d %d %" PRId64 " %" PRId32 " %s %s\n",
            (unsigned)e->mode, e->uid, e->gid,
            e->mtime_s, e->mtime_ns, e->hash, e->name);

        if (n < 0 || (size_t)n >= bodylen + 1 - pos) {
            free(body);
            return CAS_ERR;
        }
        pos += (size_t)n;
    }

    int rc = cas_put_object(ct->store, "tree", body, bodylen,
                            hash_out);

    free(body);
    return rc;
}

/****************************************************************
 * Deserialization
 ****************************************************************/

static int
parse_entry(const char *line, size_t linelen,
            struct cas_tree_entry *e)
{
    unsigned int mode;
    long long mtime_s;
    int mtime_ns;
    int n = 0;

    if (sscanf(line, "%o %d %d %lld %d %n",
               &mode, &e->uid, &e->gid,
               &mtime_s, &mtime_ns, &n) < 5)
        return CAS_ERR;

    e->mode = (int)mode;
    e->mtime_s = (int64_t)mtime_s;
    e->mtime_ns = (int32_t)mtime_ns;

    if ((size_t)n + CAS_HASH_HEX + 2 > linelen)
        return CAS_ERR;

    memcpy(e->hash, line + n, CAS_HASH_HEX);
    e->hash[CAS_HASH_HEX] = '\0';

    if (line[n + CAS_HASH_HEX] != ' ')
        return CAS_ERR;

    const char *name = line + n + CAS_HASH_HEX + 1;
    size_t namelen = linelen - (size_t)(n + CAS_HASH_HEX + 1);

    if (namelen == 0 || namelen > CAS_TREE_NAME_MAX)
        return CAS_ERR;

    memcpy(e->name, name, namelen);
    e->name[namelen] = '\0';
    return CAS_OK;
}

int
cas_tree_load(struct cas_tree *ct, const char *hash,
              struct cas_tree_dir *dir)
{
    struct cas_file cf;
    char type[CAS_TYPE_MAX + 1];

    int rc = cas_open_object(ct->store, &cf, hash, type,
                             sizeof(type));

    if (rc != CAS_OK)
        return rc;

    if (strcmp(type, "tree") != 0) {
        cas_close(&cf);
        return CAS_ETYPE;
    }

    cas_tree_dir_init(dir);

    const char *body = (const char *)cf.data;

    if (cf.len < 1 || body[0] != '%') {
        cas_close(&cf);
        return CAS_ERR;
    }

    const char *p = body + 1;
    const char *end = body + cf.len;

    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));

        if (!nl) {
            cas_tree_dir_free(dir);
            cas_close(&cf);
            return CAS_ERR;
        }

        size_t linelen = (size_t)(nl - p);
        struct cas_tree_entry e;

        if (parse_entry(p, linelen, &e) != CAS_OK) {
            cas_tree_dir_free(dir);
            cas_close(&cf);
            return CAS_ERR;
        }

        if (cas_tree_dir_add(dir, &e) != CAS_OK) {
            cas_tree_dir_free(dir);
            cas_close(&cf);
            return CAS_ERR;
        }

        p = nl + 1;
    }

    cas_close(&cf);
    return CAS_OK;
}

/****************************************************************
 * Ref helpers
 ****************************************************************/

static int
valid_ref_name(const char *name)
{
    size_t len = strlen(name);

    if (len == 0 || len > 255)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

static int
valid_hex_hash(const char *hash)
{
    if (strlen(hash) != CAS_HASH_HEX)
        return 0;
    for (int i = 0; i < CAS_HASH_HEX; i++) {
        char c = hash[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static int
fsync_dir(const char *dirpath)
{
    int fd = open(dirpath, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
        return -1;
    fsync(fd);
    close(fd);
    return 0;
}

static int
ref_path(struct cas *store, const char *name, const char *ext,
         char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "%s/refs/%s%s",
                     cas_basedir(store), name, ext);

    if (n < 0 || (size_t)n >= bufsz)
        return CAS_ERR;
    return CAS_OK;
}

static int
ensure_refs_dir(struct cas *store)
{
    char path[REF_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/refs",
                     cas_basedir(store));

    if (n < 0 || (size_t)n >= sizeof(path))
        return CAS_ERR;
    mkdir(cas_basedir(store), 0755);
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return CAS_EIO;
    return CAS_OK;
}

/****************************************************************
 * Refs and log
 ****************************************************************/

int
cas_tree_ref_read(struct cas_tree *ct, const char *name,
                  char *hash_out)
{
    if (!valid_ref_name(name))
        return CAS_ERR;

    char path[REF_PATH_MAX];

    if (ref_path(ct->store, name, ".root", path, sizeof(path))
        != CAS_OK)
        return CAS_ERR;

    FILE *fp = fopen(path, "r");

    if (!fp)
        return CAS_ENOTFOUND;

    char line[CAS_HASH_HEX + 2];

    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return CAS_EIO;
    }
    fclose(fp);

    size_t len = strlen(line);

    if (len > 0 && line[len - 1] == '\n')
        line[--len] = '\0';
    if (len != CAS_HASH_HEX)
        return CAS_ERR;

    memcpy(hash_out, line, CAS_HASH_HEX + 1);
    return CAS_OK;
}

int
cas_tree_ref_commit(struct cas_tree *ct, const char *name,
                    const char *root_hash, const char *comment)
{
    if (!valid_ref_name(name) || !valid_hex_hash(root_hash))
        return CAS_ERR;

    if (ensure_refs_dir(ct->store) != CAS_OK)
        return CAS_EIO;

    char lockpath[REF_PATH_MAX];
    char logpath[REF_PATH_MAX];
    char rootpath[REF_PATH_MAX];
    char prevpath[REF_PATH_MAX];
    char roottmp[REF_PATH_MAX];

    if (ref_path(ct->store, name, ".lock", lockpath,
                 sizeof(lockpath)) != CAS_OK ||
        ref_path(ct->store, name, ".log", logpath,
                 sizeof(logpath)) != CAS_OK ||
        ref_path(ct->store, name, ".root", rootpath,
                 sizeof(rootpath)) != CAS_OK ||
        ref_path(ct->store, name, ".prev", prevpath,
                 sizeof(prevpath)) != CAS_OK)
        return CAS_ERR;

    if (snprintf(roottmp, sizeof(roottmp), "%s.tmp", rootpath) >=
        (int)sizeof(roottmp))
        return CAS_ERR;

    int lockfd = open(lockpath, O_CREAT | O_WRONLY, 0644);

    if (lockfd < 0)
        return CAS_EIO;
    if (flock(lockfd, LOCK_EX) != 0) {
        close(lockfd);
        return CAS_EIO;
    }

    unlink(roottmp);

    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    FILE *logfp = fopen(logpath, "a");

    if (!logfp) {
        close(lockfd);
        return CAS_EIO;
    }
    if (fprintf(logfp, "%s %" PRId64 " %" PRId32 " %s\n",
                root_hash, (int64_t)ts.tv_sec, (int32_t)ts.tv_nsec,
                comment ? comment : "") < 0 ||
        fflush(logfp) != 0) {
        fclose(logfp);
        close(lockfd);
        return CAS_EIO;
    }
    fsync(fileno(logfp));
    fclose(logfp);

    if (access(rootpath, F_OK) == 0) {
        char prevtmp[REF_PATH_MAX];

        if (snprintf(prevtmp, sizeof(prevtmp), "%s.tmp",
                     prevpath) < (int)sizeof(prevtmp)) {
            unlink(prevtmp);
            if (link(rootpath, prevtmp) == 0)
                rename(prevtmp, prevpath);
        }
    }

    FILE *rootfp = fopen(roottmp, "w");

    if (!rootfp) {
        close(lockfd);
        return CAS_EIO;
    }
    if (fprintf(rootfp, "%s\n", root_hash) < 0 ||
        fflush(rootfp) != 0) {
        fclose(rootfp);
        unlink(roottmp);
        close(lockfd);
        return CAS_EIO;
    }
    fsync(fileno(rootfp));
    fclose(rootfp);

    if (rename(roottmp, rootpath) != 0) {
        unlink(roottmp);
        close(lockfd);
        return CAS_EIO;
    }

    char refsdir[REF_PATH_MAX];

    snprintf(refsdir, sizeof(refsdir), "%s/refs",
             cas_basedir(ct->store));
    fsync_dir(refsdir);

    close(lockfd);
    return CAS_OK;
}

int
cas_tree_log_read(struct cas_tree *ct, const char *name,
                  cas_tree_log_fn fn, void *ctx)
{
    if (!valid_ref_name(name))
        return CAS_ERR;

    char path[REF_PATH_MAX];

    if (ref_path(ct->store, name, ".log", path, sizeof(path))
        != CAS_OK)
        return CAS_ERR;

    FILE *fp = fopen(path, "r");

    if (!fp)
        return CAS_ENOTFOUND;

    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] != '\n') {
            if (feof(fp))
                break;
            fclose(fp);
            return CAS_ERR;
        }

        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        if (len < CAS_HASH_HEX + 1) {
            fclose(fp);
            return CAS_ERR;
        }

        char hash[CAS_HASH_HEX + 1];

        memcpy(hash, line, CAS_HASH_HEX);
        hash[CAS_HASH_HEX] = '\0';

        if (line[CAS_HASH_HEX] != ' ') {
            fclose(fp);
            return CAS_ERR;
        }

        long long time_s;
        int time_ns;
        int n = 0;

        if (sscanf(line + CAS_HASH_HEX + 1, "%lld %d %n",
                   &time_s, &time_ns, &n) < 2) {
            fclose(fp);
            return CAS_ERR;
        }

        const char *comment = line + CAS_HASH_HEX + 1 + n;

        if (fn(hash, (int64_t)time_s, (int32_t)time_ns,
               comment, ctx) != 0) {
            fclose(fp);
            return CAS_OK;
        }
    }

    fclose(fp);
    return CAS_OK;
}

/****************************************************************
 * Ref iteration
 ****************************************************************/

int
cas_tree_ref_foreach(struct cas_tree *ct, cas_tree_ref_fn fn,
                     void *ctx)
{
    char refdir[REF_PATH_MAX];
    int n = snprintf(refdir, sizeof(refdir), "%s/refs",
                     cas_basedir(ct->store));

    if (n < 0 || (size_t)n >= sizeof(refdir))
        return CAS_ERR;

    DIR *dp = opendir(refdir);

    if (!dp)
        return CAS_OK;

    struct dirent *ent;

    while ((ent = readdir(dp)) != NULL) {
        size_t len = strlen(ent->d_name);
        const char *suffix = ".root";
        size_t slen = strlen(suffix);

        if (len <= slen)
            continue;
        if (strcmp(ent->d_name + len - slen, suffix) != 0)
            continue;

        char name[256];

        if (len - slen >= sizeof(name))
            continue;
        memcpy(name, ent->d_name, len - slen);
        name[len - slen] = '\0';

        if (fn(name, ctx) != 0) {
            closedir(dp);
            return CAS_OK;
        }
    }

    closedir(dp);
    return CAS_OK;
}

/****************************************************************
 * Fsck
 ****************************************************************/

static int
fsck_tree(struct cas_tree *ct, const char *path,
          const char *tree_hash, cas_tree_fsck_fn fn, void *ctx,
          int *errors)
{
    int status = cas_fsck_object(cas_tree_cas(ct), tree_hash);

    if (status == CAS_FSCK_IOERR) {
        (*errors)++;
        if (fn && fn(path, tree_hash, CAS_TREE_FSCK_MISSING, ctx))
            return 1;
        return 0;
    }
    if (status != CAS_FSCK_OK) {
        (*errors)++;
        if (fn && fn(path, tree_hash, CAS_TREE_FSCK_CORRUPT, ctx))
            return 1;
        return 0;
    }

    struct cas_tree_dir dir;
    int rc = cas_tree_load(ct, tree_hash, &dir);

    if (rc != CAS_OK) {
        (*errors)++;
        if (fn && fn(path, tree_hash, CAS_TREE_FSCK_BAD_TREE, ctx))
            return 1;
        return 0;
    }

    for (int i = 0; i < dir.count; i++) {
        struct cas_tree_entry *e = &dir.entries[i];
        char childpath[4096];

        if (strcmp(path, "/") == 0)
            snprintf(childpath, sizeof(childpath), "/%s", e->name);
        else
            snprintf(childpath, sizeof(childpath), "%s/%s",
                     path, e->name);

        int type = e->mode & CAS_TREE_S_IFMT;

        if (type == CAS_TREE_S_IFDIR) {
            if (fsck_tree(ct, childpath, e->hash, fn, ctx, errors)) {
                cas_tree_dir_free(&dir);
                return 1;
            }
        } else {
            status = cas_fsck_object(cas_tree_cas(ct), e->hash);
            if (status == CAS_FSCK_IOERR) {
                (*errors)++;
                if (fn && fn(childpath, e->hash,
                             CAS_TREE_FSCK_MISSING, ctx)) {
                    cas_tree_dir_free(&dir);
                    return 1;
                }
            } else if (status != CAS_FSCK_OK) {
                (*errors)++;
                if (fn && fn(childpath, e->hash,
                             CAS_TREE_FSCK_CORRUPT, ctx)) {
                    cas_tree_dir_free(&dir);
                    return 1;
                }
            }
        }
    }

    cas_tree_dir_free(&dir);
    return 0;
}

int
cas_tree_fsck_root(struct cas_tree *ct, const char *root_hash,
                   cas_tree_fsck_fn fn, void *ctx)
{
    int errors = 0;

    fsck_tree(ct, "/", root_hash, fn, ctx, &errors);
    return errors ? CAS_ERR : CAS_OK;
}

struct fsck_ref_ctx {
    struct cas_tree *ct;
    cas_tree_fsck_fn fn;
    void *ctx;
    int errors;
};

static int
fsck_ref(const char *name, void *ctx)
{
    struct fsck_ref_ctx *fc = ctx;
    char hash[CAS_HASH_HEX + 1];

    if (cas_tree_ref_read(fc->ct, name, hash) != CAS_OK)
        return 0;

    int errors = 0;

    fsck_tree(fc->ct, "/", hash, fc->fn, fc->ctx, &errors);
    fc->errors += errors;
    return 0;
}

int
cas_tree_fsck(struct cas_tree *ct, cas_tree_fsck_fn fn, void *ctx)
{
    struct fsck_ref_ctx fc = {
        .ct = ct,
        .fn = fn,
        .ctx = ctx,
        .errors = 0,
    };

    cas_tree_ref_foreach(ct, fsck_ref, &fc);
    return fc.errors ? CAS_ERR : CAS_OK;
}

/****************************************************************
 * Garbage collection
 ****************************************************************/

struct hash_set {
    char (*hashes)[CAS_HASH_HEX + 1];
    int count;
    int cap;
};

static void
hash_set_init(struct hash_set *hs)
{
    hs->hashes = NULL;
    hs->count = 0;
    hs->cap = 0;
}

static void
hash_set_free(struct hash_set *hs)
{
    free(hs->hashes);
    hs->hashes = NULL;
    hs->count = 0;
    hs->cap = 0;
}

static int
hash_set_contains(struct hash_set *hs, const char *hash)
{
    for (int i = 0; i < hs->count; i++)
        if (strcmp(hs->hashes[i], hash) == 0)
            return 1;
    return 0;
}

static int
hash_set_add(struct hash_set *hs, const char *hash)
{
    if (hash_set_contains(hs, hash))
        return CAS_OK;
    if (hs->count >= hs->cap) {
        int newcap = hs->cap ? hs->cap * 2 : 64;
        void *p = realloc(hs->hashes,
            (size_t)newcap * sizeof(*hs->hashes));

        if (!p)
            return CAS_ENOMEM;
        hs->hashes = p;
        hs->cap = newcap;
    }
    memcpy(hs->hashes[hs->count++], hash, CAS_HASH_HEX + 1);
    return CAS_OK;
}

static int
mark_tree(struct cas_tree *ct, struct hash_set *reachable,
          const char *tree_hash)
{
    if (hash_set_contains(reachable, tree_hash))
        return CAS_OK;
    if (hash_set_add(reachable, tree_hash) != CAS_OK)
        return CAS_ENOMEM;

    struct cas_tree_dir dir;
    int rc = cas_tree_load(ct, tree_hash, &dir);

    if (rc != CAS_OK)
        return rc;

    for (int i = 0; i < dir.count; i++) {
        struct cas_tree_entry *e = &dir.entries[i];
        int type = e->mode & CAS_TREE_S_IFMT;

        if (type == CAS_TREE_S_IFDIR) {
            rc = mark_tree(ct, reachable, e->hash);
            if (rc != CAS_OK) {
                cas_tree_dir_free(&dir);
                return rc;
            }
        } else {
            if (hash_set_add(reachable, e->hash) != CAS_OK) {
                cas_tree_dir_free(&dir);
                return CAS_ENOMEM;
            }
        }
    }

    cas_tree_dir_free(&dir);
    return CAS_OK;
}

struct mark_ref_ctx {
    struct cas_tree *ct;
    struct hash_set *reachable;
    int rc;
};

static int
mark_log_entry(const char *hash, int64_t time_s, int32_t time_ns,
               const char *comment, void *ctx)
{
    (void)time_s;
    (void)time_ns;
    (void)comment;
    struct mark_ref_ctx *mc = ctx;

    int rc = mark_tree(mc->ct, mc->reachable, hash);

    if (rc != CAS_OK) {
        mc->rc = rc;
        return 1;
    }
    return 0;
}

static int
mark_ref(const char *name, void *ctx)
{
    struct mark_ref_ctx *mc = ctx;

    cas_tree_log_read(mc->ct, name, mark_log_entry, mc);
    return mc->rc != CAS_OK ? 1 : 0;
}

struct sweep_ctx {
    struct cas *store;
    struct hash_set *reachable;
    cas_tree_gc_fn fn;
    void *ctx;
    time_t cutoff;
    int removed;
};

static int
sweep_visitor(const char *hash, void *ctx)
{
    struct sweep_ctx *sc = ctx;

    if (hash_set_contains(sc->reachable, hash))
        return 0;

    if (sc->cutoff > 0) {
        time_t mtime;

        if (cas_object_mtime(sc->store, hash, &mtime) == CAS_OK &&
            mtime >= sc->cutoff)
            return 0;
    }

    if (cas_remove(sc->store, hash) == CAS_OK) {
        sc->removed++;
        if (sc->fn && sc->fn(hash, sc->ctx) != 0)
            return 1;
    }
    return 0;
}

int
cas_tree_gc(struct cas_tree *ct, time_t grace, cas_tree_gc_fn fn,
            void *ctx, int *removed)
{
    struct hash_set reachable;

    hash_set_init(&reachable);

    struct mark_ref_ctx mc = {
        .ct = ct,
        .reachable = &reachable,
        .rc = CAS_OK,
    };

    cas_tree_ref_foreach(ct, mark_ref, &mc);
    if (mc.rc != CAS_OK) {
        hash_set_free(&reachable);
        return mc.rc;
    }

    struct sweep_ctx sc = {
        .store = cas_tree_cas(ct),
        .reachable = &reachable,
        .fn = fn,
        .ctx = ctx,
        .cutoff = grace > 0 ? time(NULL) - grace : 0,
        .removed = 0,
    };

    cas_foreach(cas_tree_cas(ct), sweep_visitor, &sc);

    if (removed)
        *removed = sc.removed;

    hash_set_free(&reachable);
    return CAS_OK;
}
