/* test_vfs_snap.c : unit tests for VFS snapshot module */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "vfs-snap.h"
#include "cas-codec.h"
#include "cas-pack.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/test_vfs_snap_XXXXXX";

static void
cleanup(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd)) { /* best effort */ }
}

static const struct vfs_cred admin = {
    .uid = 0,
    .is_admin = 1,
};

static struct cas *
make_store(const char *name)
{
    char depot[512];

    snprintf(depot, sizeof(depot), "%s/%s", tmpdir, name);
    return cas_new(depot);
}

/****************************************************************
 * Empty VFS round trip
 ****************************************************************/

static void
test_snap_empty(void)
{
    struct cas *store = make_store("empty");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT(ct != NULL);
    ASSERT(fs != NULL);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);
    ASSERT_INT_EQ((int)strlen(hash), CAS_HASH_HEX);

    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(fs2, &admin, ct, hash), CAS_OK);
    ASSERT(vfs_usage(fs2) == 0);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Single file round trip
 ****************************************************************/

static void
test_snap_single_file(void)
{
    struct cas *store = make_store("single");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/hello.txt",
                  "hello world", 11, 1), VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(fs2, &admin, ct, hash), CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/hello.txt", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 11);
    ASSERT(memcmp(data, "hello world", 11) == 0);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

#ifdef CAS_WITH_MINIZ
/****************************************************************
 * Compressed snapshot (vfs_snap_store_z)
 ****************************************************************/

static void
test_snap_store_z(void)
{
    struct cas *store = make_store("snapz");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    /* a compressible text file */
    char text[2000];
    for (size_t i = 0; i < sizeof(text); i++)
        text[i] = (char)('a' + (i % 16));
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/notes.txt", text,
                  sizeof(text), 1), VFS_OK);

    /* a binary file: half control bytes, so GUESS skips it */
    unsigned char bin[512];
    for (size_t i = 0; i < sizeof(bin); i++)
        bin[i] = (i & 1) ? 0x01 : 0x80;
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/blob.bin", bin,
                  sizeof(bin), 1), VFS_OK);

    char hz[CAS_HASH_HEX + 1], h0[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store_z(fs, &admin, ct, CAS_COMPRESS_GUESS,
                  CAS_CODEC_DEFLATE, hz), CAS_OK);

    /* transparency: the compressed snapshot has the same tree hash
     * as an uncompressed one (blobs live at their plaintext address) */
    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, h0), CAS_OK);
    ASSERT_STR_EQ(hz, h0);

    /* the text blob is stored compressed, the binary blob raw */
    struct cas_file raw;
    unsigned char tr[CAS_PACK_BLOCK];
    char bh[CAS_HASH_HEX + 1];

    cas_hash_object("blob", text, sizeof(text), bh);
    ASSERT_INT_EQ(cas_open_loose_raw(store, &raw, bh, tr), CAS_OK);
    ASSERT(raw.len < sizeof(text));
    cas_close(&raw);

    cas_hash_object("blob", bin, sizeof(bin), bh);
    ASSERT_INT_EQ(cas_open_loose_raw(store, &raw, bh, tr), CAS_OK);
    ASSERT_INT_EQ((int)raw.len, (int)sizeof(bin));
    cas_close(&raw);

    /* restore reproduces both files exactly */
    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(fs2, &admin, ct, hz), CAS_OK);

    const void *d;
    size_t l;

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/notes.txt", &d, &l), VFS_OK);
    ASSERT_INT_EQ((int)l, (int)sizeof(text));
    ASSERT(memcmp(d, text, sizeof(text)) == 0);

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/blob.bin", &d, &l), VFS_OK);
    ASSERT_INT_EQ((int)l, (int)sizeof(bin));
    ASSERT(memcmp(d, bin, sizeof(bin)) == 0);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}
#endif

/****************************************************************
 * Nested tree round trip
 ****************************************************************/

static void
test_snap_nested(void)
{
    struct cas *store = make_store("nested");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/README",
                  "top level", 9, 1), VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/src", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/src/main.c",
                  "int main(){}", 12, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/src/util.c",
                  "void help(){}", 13, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/doc", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/doc/api.txt",
                  "API docs", 8, 0), VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(fs2, &admin, ct, hash), CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/README", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 9);
    ASSERT(memcmp(data, "top level", 9) == 0);

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/src/main.c",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 12);

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/src/util.c",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 13);

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/doc/api.txt",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 8);
    ASSERT(memcmp(data, "API docs", 8) == 0);

    struct vfs_stat st;

    ASSERT_INT_EQ(vfs_stat(fs2, &admin, "/src", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);
    ASSERT_INT_EQ(vfs_stat(fs2, &admin, "/doc", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Deterministic hashing
 ****************************************************************/

static void
test_snap_deterministic(void)
{
    struct cas *store = make_store("deterministic");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs1 = vfs_new(NULL);
    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs1, &admin, "/b.txt", "BBB", 3, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs1, &admin, "/a.txt", "AAA", 3, 1),
                  VFS_OK);

    ASSERT_INT_EQ(vfs_write(fs2, &admin, "/a.txt", "AAA", 3, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs2, &admin, "/b.txt", "BBB", 3, 1),
                  VFS_OK);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs1, &admin, ct, h1), CAS_OK);
    ASSERT_INT_EQ(vfs_snap_store(fs2, &admin, ct, h2), CAS_OK);
    ASSERT_STR_EQ(h1, h2);

    vfs_free(fs2);
    vfs_free(fs1);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Commit and checkout
 ****************************************************************/

static void
test_snap_commit_checkout(void)
{
    struct cas *store = make_store("commit");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/v1.txt",
                  "version one", 11, 1), VFS_OK);
    ASSERT_INT_EQ(vfs_snap_commit(fs, &admin, ct, "main",
                  "first version"), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/v2.txt",
                  "version two", 11, 1), VFS_OK);
    ASSERT_INT_EQ(vfs_snap_commit(fs, &admin, ct, "main",
                  "second version"), CAS_OK);

    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_checkout(fs2, &admin, ct, "main"),
                  CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/v1.txt", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 11);
    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/v2.txt", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 11);

    ASSERT_INT_EQ(vfs_snap_checkout(fs2, &admin, ct, "nope"),
                  CAS_ENOTFOUND);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Modify and restore earlier version
 ****************************************************************/

static void
test_snap_versioning(void)
{
    struct cas *store = make_store("versioning");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/config.txt",
                  "old config", 10, 1), VFS_OK);

    char h1[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, h1), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/config.txt",
                  "new config!!", 12, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/extra.txt",
                  "bonus", 5, 1), VFS_OK);

    char h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, h2), CAS_OK);
    ASSERT(strcmp(h1, h2) != 0);

    struct vfs *restored = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(restored, &admin, ct, h1),
                  CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(restored, &admin, "/config.txt",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 10);
    ASSERT(memcmp(data, "old config", 10) == 0);

    ASSERT_INT_EQ(vfs_read(restored, &admin, "/extra.txt",
                  &data, &len), VFS_ENOTFOUND);

    vfs_free(restored);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Empty file round trip
 ****************************************************************/

static void
test_snap_empty_file(void)
{
    struct cas *store = make_store("emptyfile");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/empty",
                  "", 0, 1), VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore(fs2, &admin, ct, hash), CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(fs2, &admin, "/empty", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 0);

    vfs_free(fs2);
    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Structural sharing: identical subtrees share CAS objects
 ****************************************************************/

static void
test_snap_sharing(void)
{
    struct cas *store = make_store("sharing");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs1 = vfs_new(NULL);
    struct vfs *fs2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_mkdir(fs1, &admin, "/a", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs1, &admin, "/a/x.txt", "X", 1, 0),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(fs1, &admin, "/b", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs1, &admin, "/b/y.txt", "Y", 1, 0),
                  VFS_OK);

    ASSERT_INT_EQ(vfs_mkdir(fs2, &admin, "/a", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs2, &admin, "/a/x.txt", "X", 1, 0),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(fs2, &admin, "/b", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs2, &admin, "/b/y.txt",
                  "different", 9, 0), VFS_OK);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs1, &admin, ct, h1), CAS_OK);
    ASSERT_INT_EQ(vfs_snap_store(fs2, &admin, ct, h2), CAS_OK);

    /* Root trees differ (different /b content) */
    ASSERT(strcmp(h1, h2) != 0);

    /* But /a subtrees should produce the same hash */
    struct cas_tree_dir d1, d2;

    ASSERT_INT_EQ(cas_tree_load(ct, h1, &d1), CAS_OK);
    ASSERT_INT_EQ(cas_tree_load(ct, h2, &d2), CAS_OK);

    ASSERT_STR_EQ(d1.entries[0].name, "a");
    ASSERT_STR_EQ(d2.entries[0].name, "a");
    ASSERT_STR_EQ(d1.entries[0].hash, d2.entries[0].hash);

    ASSERT_STR_EQ(d1.entries[1].name, "b");
    ASSERT_STR_EQ(d2.entries[1].name, "b");
    ASSERT(strcmp(d1.entries[1].hash, d2.entries[1].hash) != 0);

    cas_tree_dir_free(&d1);
    cas_tree_dir_free(&d2);
    vfs_free(fs2);
    vfs_free(fs1);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: matching content
 ****************************************************************/

static void
test_checkfile_match(void)
{
    struct cas *store = make_store("cf_match");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "AAA", 3, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash, "/a.txt"),
                  CAS_OK);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: modified content
 ****************************************************************/

static void
test_checkfile_modified(void)
{
    struct cas *store = make_store("cf_mod");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "AAA", 3, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "BBB", 3, 0),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash, "/a.txt"),
                  CAS_ERR);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: path not in snapshot
 ****************************************************************/

static void
test_checkfile_not_in_snap(void)
{
    struct cas *store = make_store("cf_nosnap");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/b.txt", "B", 1, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash, "/b.txt"),
                  CAS_ENOTFOUND);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: path not in VFS
 ****************************************************************/

static void
test_checkfile_not_in_vfs(void)
{
    struct cas *store = make_store("cf_novfs");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);
    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/a.txt"), VFS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash, "/a.txt"),
                  VFS_ENOTFOUND);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: VFS path is a directory
 ****************************************************************/

static void
test_checkfile_directory(void)
{
    struct cas *store = make_store("cf_dir");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/d", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d/f.txt", "F", 1, 0),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash, "/d"),
                  VFS_EISDIR);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * checkfile: nested path
 ****************************************************************/

static void
test_checkfile_nested(void)
{
    struct cas *store = make_store("cf_nested");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/src", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/src/main.c",
                  "int main(){}", 12, 0), VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash,
                  "/src/main.c"), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/src/main.c",
                  "int main(){return 1;}", 21, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_snap_checkfile(fs, &admin, ct, hash,
                  "/src/main.c"), CAS_ERR);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck helpers
 ****************************************************************/

struct fsck_record {
    char path[4096];
    int status;
};

struct fsck_log {
    struct fsck_record records[64];
    int count;
};

static int
fsck_collect(const char *path, int status, void *ctx)
{
    struct fsck_log *log = ctx;

    if (log->count < 64) {
        snprintf(log->records[log->count].path,
                 sizeof(log->records[0].path), "%s", path);
        log->records[log->count].status = status;
        log->count++;
    }
    return 0;
}

static int
fsck_find(struct fsck_log *log, const char *path)
{
    for (int i = 0; i < log->count; i++) {
        if (strcmp(log->records[i].path, path) == 0)
            return log->records[i].status;
    }
    return -999;
}

/****************************************************************
 * fsck: clean state
 ****************************************************************/

static void
test_fsck_clean(void)
{
    struct cas *store = make_store("fsck_clean");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/d", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d/b.txt", "B", 1, 0),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_OK);
    ASSERT_INT_EQ(log.count, 0);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck: modified file
 ****************************************************************/

static void
test_fsck_modified(void)
{
    struct cas *store = make_store("fsck_mod");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/b.txt", "B", 1, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/b.txt", "X", 1, 0),
                  VFS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_ERR);
    ASSERT_INT_EQ(log.count, 1);
    ASSERT_INT_EQ(fsck_find(&log, "/b.txt"), VFS_SNAP_FSCK_MODIFIED);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck: added file
 ****************************************************************/

static void
test_fsck_added(void)
{
    struct cas *store = make_store("fsck_add");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/new.txt", "N", 1, 1),
                  VFS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_ERR);
    ASSERT_INT_EQ(log.count, 1);
    ASSERT_INT_EQ(fsck_find(&log, "/new.txt"), VFS_SNAP_FSCK_ADDED);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck: missing file
 ****************************************************************/

static void
test_fsck_missing(void)
{
    struct cas *store = make_store("fsck_miss");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a.txt", "A", 1, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/b.txt", "B", 1, 1),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/a.txt"), VFS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_ERR);
    ASSERT_INT_EQ(log.count, 1);
    ASSERT_INT_EQ(fsck_find(&log, "/a.txt"), VFS_SNAP_FSCK_MISSING);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck: empty VFS and empty snapshot
 ****************************************************************/

static void
test_fsck_empty(void)
{
    struct cas *store = make_store("fsck_empty");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_OK);
    ASSERT_INT_EQ(log.count, 0);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * fsck: type mismatch (dir in snapshot, file in VFS)
 ****************************************************************/

static void
test_fsck_type_mismatch(void)
{
    struct cas *store = make_store("fsck_type");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/d", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d/x.txt", "X", 1, 0),
                  VFS_OK);

    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(fs, &admin, ct, hash), CAS_OK);

    ASSERT_INT_EQ(vfs_delete_recursive(fs, &admin, "/d"), VFS_OK);
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d", "file now", 8, 1),
                  VFS_OK);

    struct fsck_log log = { .count = 0 };

    ASSERT_INT_EQ(vfs_snap_fsck(fs, &admin, ct, hash,
                  fsck_collect, &log), CAS_ERR);
    ASSERT_INT_EQ(log.count, 1);
    ASSERT_INT_EQ(fsck_find(&log, "/d"), VFS_SNAP_FSCK_TYPE);

    vfs_free(fs);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * restore_at: mount a bundle under a directory prefix
 ****************************************************************/

static void
test_snap_restore_at(void)
{
    struct cas *store = make_store("restore_at");
    struct cas_tree *ct = cas_tree_new(store);

    /* build a module bundle: a file and a subdir with a file */
    struct vfs *mod = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(mod, &admin, "/data.txt", "MODULE", 6, 1),
                  VFS_OK);
    ASSERT_INT_EQ(vfs_mkdir(mod, &admin, "/sub", 0), VFS_OK);
    ASSERT_INT_EQ(vfs_write(mod, &admin, "/sub/inner.bin", "XYZ", 3, 0),
                  VFS_OK);

    char modhash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(mod, &admin, ct, modhash), CAS_OK);
    ASSERT_INT_EQ(cas_tree_ref_commit(ct, "coolmod", modhash, "v1"),
                  CAS_OK);

    /* target VFS with unrelated pre-existing content */
    struct vfs *game = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_write(game, &admin, "/game.cfg", "root", 4, 1),
                  VFS_OK);

    /* mount the module under a not-yet-existing prefix */
    ASSERT_INT_EQ(vfs_snap_restore_at(game, &admin, ct,
                  "/modules/coolmod", modhash), CAS_OK);

    const void *data;
    size_t len;

    ASSERT_INT_EQ(vfs_read(game, &admin, "/modules/coolmod/data.txt",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 6);
    ASSERT(memcmp(data, "MODULE", 6) == 0);
    ASSERT_INT_EQ(vfs_read(game, &admin,
                  "/modules/coolmod/sub/inner.bin", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 3);

    /* pre-existing content untouched */
    ASSERT_INT_EQ(vfs_read(game, &admin, "/game.cfg", &data, &len),
                  VFS_OK);

    /* the mount point is a directory */
    struct vfs_stat st;

    ASSERT_INT_EQ(vfs_stat(game, &admin, "/modules/coolmod", &st),
                  VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);

    /* checkout_at resolves a ref and mounts under a second prefix;
     * the shared store dedups the objects behind both mounts */
    ASSERT_INT_EQ(vfs_snap_checkout_at(game, &admin, ct,
                  "/modules/copy", "coolmod"), CAS_OK);
    ASSERT_INT_EQ(vfs_read(game, &admin, "/modules/copy/data.txt",
                  &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 6);

    /* a trailing slash on the prefix is tolerated */
    ASSERT_INT_EQ(vfs_snap_restore_at(game, &admin, ct,
                  "/modules/trail/", modhash), CAS_OK);
    ASSERT_INT_EQ(vfs_read(game, &admin, "/modules/trail/data.txt",
                  &data, &len), VFS_OK);

    /* a non-absolute prefix is rejected */
    ASSERT_INT_EQ(vfs_snap_restore_at(game, &admin, ct,
                  "rel/path", modhash), CAS_ERR);

    /* base "/" behaves like plain restore */
    struct vfs *g2 = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore_at(g2, &admin, ct, "/", modhash),
                  CAS_OK);
    ASSERT_INT_EQ(vfs_read(g2, &admin, "/data.txt", &data, &len),
                  VFS_OK);

    /* an unknown ref propagates the not-found error */
    ASSERT_INT_EQ(vfs_snap_checkout_at(game, &admin, ct,
                  "/modules/nope", "missing"), CAS_ENOTFOUND);

    vfs_free(g2);
    vfs_free(game);
    vfs_free(mod);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * restore_at: an empty bundle still creates the mount point
 ****************************************************************/

static void
test_snap_restore_at_empty(void)
{
    struct cas *store = make_store("restore_at_empty");
    struct cas_tree *ct = cas_tree_new(store);
    struct vfs *empty = vfs_new(NULL);

    char h[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(vfs_snap_store(empty, &admin, ct, h), CAS_OK);

    struct vfs *game = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_snap_restore_at(game, &admin, ct, "/mods/void", h),
                  CAS_OK);

    struct vfs_stat st;

    ASSERT_INT_EQ(vfs_stat(game, &admin, "/mods/void", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);

    vfs_free(game);
    vfs_free(empty);
    cas_tree_free(ct);
    cas_free(store);
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(void)
{
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    atexit(cleanup);

    fprintf(stderr, "--- vfs-snap tests ---\n");

    RUN(test_snap_empty);
    RUN(test_snap_single_file);
    RUN(test_snap_nested);
#ifdef CAS_WITH_MINIZ
    RUN(test_snap_store_z);
#endif
    RUN(test_snap_deterministic);
    RUN(test_snap_commit_checkout);
    RUN(test_snap_restore_at);
    RUN(test_snap_restore_at_empty);
    RUN(test_snap_versioning);
    RUN(test_snap_empty_file);
    RUN(test_snap_sharing);
    RUN(test_checkfile_match);
    RUN(test_checkfile_modified);
    RUN(test_checkfile_not_in_snap);
    RUN(test_checkfile_not_in_vfs);
    RUN(test_checkfile_directory);
    RUN(test_checkfile_nested);
    RUN(test_fsck_clean);
    RUN(test_fsck_modified);
    RUN(test_fsck_added);
    RUN(test_fsck_missing);
    RUN(test_fsck_empty);
    RUN(test_fsck_type_mismatch);

    TEST_REPORT();
}
