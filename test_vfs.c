/* test_vfs.c : unit tests for the VFS module */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#include "vfs.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static const struct vfs_cred admin = { .uid = 0, .is_admin = 1 };
static const struct vfs_cred user1 = { .uid = 1000 };
static const struct vfs_cred user2 = { .uid = 2000 };

/****************************************************************
 * Path utilities
 ****************************************************************/

static void
test_normalize(void)
{
    char buf[256];

    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/"), VFS_OK);
    ASSERT_STR_EQ(buf, "/");

    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/foo/bar"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo/bar");

    // collapse duplicate slashes
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "//foo///bar//"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo/bar");

    // resolve . and ..
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/foo/./bar"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo/bar");

    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/foo/bar/.."), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo");

    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/foo/bar/../baz"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo/baz");

    // .. past root clamps
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/.."), VFS_OK);
    ASSERT_STR_EQ(buf, "/");

    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/../../../foo"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo");

    // strip trailing slash
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "/foo/"), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo");

    // errors
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), ""), VFS_EBADPATH);
    ASSERT_INT_EQ(vfs_normalize(buf, sizeof(buf), "relative"), VFS_EBADPATH);
}

static void
test_resolve(void)
{
    char buf[256];

    // absolute path ignores cwd
    ASSERT_INT_EQ(vfs_resolve(buf, sizeof(buf), "/abs", "/cwd"), VFS_OK);
    ASSERT_STR_EQ(buf, "/abs");

    // relative path prepends cwd
    ASSERT_INT_EQ(vfs_resolve(buf, sizeof(buf), "rel", "/cwd"), VFS_OK);
    ASSERT_STR_EQ(buf, "/cwd/rel");

    ASSERT_INT_EQ(vfs_resolve(buf, sizeof(buf), "../sibling", "/a/b"),
                  VFS_OK);
    ASSERT_STR_EQ(buf, "/a/sibling");

    // NULL cwd defaults to root
    ASSERT_INT_EQ(vfs_resolve(buf, sizeof(buf), "foo", NULL), VFS_OK);
    ASSERT_STR_EQ(buf, "/foo");
}

/****************************************************************
 * Lifecycle
 ****************************************************************/

static void
test_lifecycle(void)
{
    struct vfs *fs = vfs_new(NULL);
    ASSERT(fs != NULL);

    // root always exists
    struct vfs_stat st;
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);

    vfs_free(fs);
    vfs_free(NULL); // should not crash
}

/****************************************************************
 * Mkdir
 ****************************************************************/

static void
test_mkdir(void)
{
    struct vfs *fs = vfs_new(NULL);

    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/a", 0), VFS_OK);

    struct vfs_stat st;
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/a", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);

    // duplicate without create_parents is an error
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/a", 0), VFS_EEXIST);

    // duplicate with create_parents is OK
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/a", 1), VFS_OK);

    // missing parent without create_parents
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/x/y/z", 0), VFS_ENOTFOUND);

    // create_parents creates ancestors
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/x/y/z", 1), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/x", &st), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/x/y", &st), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/x/y/z", &st), VFS_OK);

    // root mkdir is a no-op
    ASSERT_INT_EQ(vfs_mkdir(fs, &admin, "/", 0), VFS_OK);

    vfs_free(fs);
}

/****************************************************************
 * Write and read
 ****************************************************************/

static void
test_write_read(void)
{
    struct vfs *fs = vfs_new(NULL);

    // write with create_parents
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d/hello.txt",
                            "hello", 5, 1), VFS_OK);

    // read it back
    const void *data;
    size_t len;
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/d/hello.txt", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 5);
    ASSERT(memcmp(data, "hello", 5) == 0);

    // overwrite
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/d/hello.txt",
                            "world!", 6, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/d/hello.txt", &data, &len),
                  VFS_OK);
    ASSERT_INT_EQ((int)len, 6);
    ASSERT(memcmp(data, "world!", 6) == 0);

    // write empty file
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/empty", NULL, 0, 0), VFS_OK);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/empty", &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 0);

    // read a directory fails
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/d", &data, &len), VFS_EISDIR);

    // write to root fails
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/", "x", 1, 0), VFS_EISDIR);

    // write without parent fails
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/no/such/file", "x", 1, 0),
                  VFS_ENOTFOUND);

    vfs_free(fs);
}

/****************************************************************
 * Stat
 ****************************************************************/

static void
test_stat(void)
{
    struct vfs *fs = vfs_new(NULL);
    struct vfs_stat st;

    vfs_mkdir(fs, &user1, "/mydir", 0);
    vfs_write(fs, &user1, "/mydir/f", "data", 4, 0);

    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/mydir", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_DIR);
    ASSERT_INT_EQ(st.owner, 1000);

    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/mydir/f", &st), VFS_OK);
    ASSERT_INT_EQ(st.type, VFS_FILE);
    ASSERT_INT_EQ((int)st.size, 4);
    ASSERT_INT_EQ(st.owner, 1000);
    ASSERT(st.ctime > 0);
    ASSERT(st.mtime > 0);

    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/nope", &st), VFS_ENOTFOUND);

    vfs_free(fs);
}

/****************************************************************
 * List
 ****************************************************************/

struct list_ctx {
    int count;
    char names[16][256];
};

static int
list_cb(const struct vfs_stat *st, void *ctx)
{
    struct list_ctx *lc = ctx;

    if (lc->count < 16) {
        const char *slash = strrchr(st->path, '/');
        const char *name = slash ? slash + 1 : st->path;

        snprintf(lc->names[lc->count], sizeof(lc->names[0]),
                 "%s", name);
    }
    lc->count++;
    return 0;
}

static int
has_name(struct list_ctx *lc, const char *name)
{
    for (int i = 0; i < lc->count; i++) {
        if (strcmp(lc->names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void
test_list(void)
{
    struct vfs *fs = vfs_new(NULL);

    vfs_mkdir(fs, &admin, "/a", 0);
    vfs_write(fs, &admin, "/a/x", "1", 1, 0);
    vfs_write(fs, &admin, "/a/y", "2", 1, 0);
    vfs_mkdir(fs, &admin, "/a/sub", 0);
    vfs_write(fs, &admin, "/a/sub/deep", "3", 1, 0);

    // list /a -- should get x, y, sub but not sub/deep
    struct list_ctx lc = {0};
    ASSERT_INT_EQ(vfs_list(fs, &admin, "/a", list_cb, &lc), VFS_OK);
    ASSERT_INT_EQ(lc.count, 3);
    ASSERT(has_name(&lc, "x"));
    ASSERT(has_name(&lc, "y"));
    ASSERT(has_name(&lc, "sub"));

    // list root
    memset(&lc, 0, sizeof(lc));
    ASSERT_INT_EQ(vfs_list(fs, &admin, "/", list_cb, &lc), VFS_OK);
    ASSERT_INT_EQ(lc.count, 1);
    ASSERT(has_name(&lc, "a"));

    // list a file fails
    ASSERT_INT_EQ(vfs_list(fs, &admin, "/a/x", list_cb, &lc),
                  VFS_ENOTDIR);

    // list nonexistent
    ASSERT_INT_EQ(vfs_list(fs, &admin, "/nope", list_cb, &lc),
                  VFS_ENOTFOUND);

    vfs_free(fs);
}

/****************************************************************
 * Delete
 ****************************************************************/

static void
test_delete(void)
{
    struct vfs *fs = vfs_new(NULL);
    struct vfs_stat st;

    vfs_write(fs, &admin, "/f", "data", 4, 0);

    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/f"), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/f", &st), VFS_ENOTFOUND);

    // delete empty dir
    vfs_mkdir(fs, &admin, "/d", 0);
    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/d"), VFS_OK);

    // delete non-empty dir fails
    vfs_mkdir(fs, &admin, "/d2", 0);
    vfs_write(fs, &admin, "/d2/f", "x", 1, 0);
    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/d2"), VFS_ENOTEMPTY);

    // cannot delete root
    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/"), VFS_EPERM);

    // delete nonexistent
    ASSERT_INT_EQ(vfs_delete(fs, &admin, "/nope"), VFS_ENOTFOUND);

    vfs_free(fs);
}

static void
test_delete_recursive(void)
{
    struct vfs *fs = vfs_new(NULL);
    struct vfs_stat st;

    vfs_mkdir(fs, &admin, "/a/b/c", 1);
    vfs_write(fs, &admin, "/a/b/c/f", "data", 4, 0);
    vfs_write(fs, &admin, "/a/b/g", "data", 4, 0);

    ASSERT_INT_EQ(vfs_delete_recursive(fs, &admin, "/a"), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/a", &st), VFS_ENOTFOUND);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/a/b", &st), VFS_ENOTFOUND);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/a/b/c/f", &st), VFS_ENOTFOUND);

    // usage should be back to zero
    ASSERT(vfs_usage(fs) == 0);

    vfs_free(fs);
}

/****************************************************************
 * Rename
 ****************************************************************/

static void
test_rename(void)
{
    struct vfs *fs = vfs_new(NULL);
    struct vfs_stat st;
    const void *data;
    size_t len;

    vfs_write(fs, &admin, "/a", "hello", 5, 0);

    ASSERT_INT_EQ(vfs_rename(fs, &admin, "/a", "/b"), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/a", &st), VFS_ENOTFOUND);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/b", &data, &len), VFS_OK);
    ASSERT(memcmp(data, "hello", 5) == 0);

    // rename directory with children
    vfs_mkdir(fs, &admin, "/d", 0);
    vfs_write(fs, &admin, "/d/f", "x", 1, 0);
    ASSERT_INT_EQ(vfs_rename(fs, &admin, "/d", "/e"), VFS_OK);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/e/f", &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 1);

    // cannot move into own subtree
    vfs_mkdir(fs, &admin, "/p/q", 1);
    ASSERT_INT_EQ(vfs_rename(fs, &admin, "/p", "/p/q/r"), VFS_EBADPATH);

    // same path is a no-op
    ASSERT_INT_EQ(vfs_rename(fs, &admin, "/b", "/b"), VFS_OK);

    vfs_free(fs);
}

/****************************************************************
 * Copy
 ****************************************************************/

static void
test_copy(void)
{
    struct vfs *fs = vfs_new(NULL);
    const void *data;
    size_t len;

    vfs_write(fs, &admin, "/src", "payload", 7, 0);

    ASSERT_INT_EQ(vfs_copy(fs, &admin, "/src", "/dst", 0), VFS_OK);

    // both exist and have the same content
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/src", &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 7);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/dst", &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 7);
    ASSERT(memcmp(data, "payload", 7) == 0);

    // modifying one does not affect the other
    vfs_write(fs, &admin, "/src", "new", 3, 0);
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/dst", &data, &len), VFS_OK);
    ASSERT_INT_EQ((int)len, 7);

    // copy a directory fails
    vfs_mkdir(fs, &admin, "/dir", 0);
    ASSERT_INT_EQ(vfs_copy(fs, &admin, "/dir", "/dir2", 0), VFS_EISDIR);

    vfs_free(fs);
}

/****************************************************************
 * Chmod
 ****************************************************************/

static void
test_chmod(void)
{
    struct vfs *fs = vfs_new(NULL);
    struct vfs_stat st;

    vfs_write(fs, &admin, "/f", "x", 1, 0);

    ASSERT_INT_EQ(vfs_chmod(fs, &admin, "/f", 0444), VFS_OK);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/f", &st), VFS_OK);
    ASSERT_INT_EQ(st.mode, 0444);

    // non-admin cannot chmod
    ASSERT_INT_EQ(vfs_chmod(fs, &user1, "/f", 0666), VFS_EPERM);

    vfs_free(fs);
}

/****************************************************************
 * Permissions
 ****************************************************************/

static void
test_permissions(void)
{
    struct vfs *fs = vfs_new(NULL);
    const void *data;
    size_t len;

    // user1 creates a file
    vfs_write(fs, &user1, "/secret", "data", 4, 0);

    // default mode is 0666, so user2 can read (other-read)
    ASSERT_INT_EQ(vfs_read(fs, &user2, "/secret", &data, &len), VFS_OK);

    // restrict to owner-only
    vfs_chmod(fs, &admin, "/secret", 0600);

    // user2 cannot read
    ASSERT_INT_EQ(vfs_read(fs, &user2, "/secret", &data, &len),
                  VFS_EPERM);

    // owner can still read
    struct vfs_cred owner = { .uid = 1000 };
    ASSERT_INT_EQ(vfs_read(fs, &owner, "/secret", &data, &len), VFS_OK);

    // user2 cannot write
    ASSERT_INT_EQ(vfs_write(fs, &user2, "/secret", "x", 1, 0),
                  VFS_EPERM);

    // admin bypasses everything
    ASSERT_INT_EQ(vfs_read(fs, &admin, "/secret", &data, &len), VFS_OK);

    // group access
    vfs_chmod(fs, &admin, "/secret", 0060);
    struct vfs_cred group_member = { .uid = 3000, .is_group = 1 };
    ASSERT_INT_EQ(vfs_read(fs, &group_member, "/secret", &data, &len),
                  VFS_OK);

    vfs_free(fs);
}

/****************************************************************
 * Quota
 ****************************************************************/

static void
test_quota(void)
{
    struct vfs_opts opts = { .quota = 100 };
    struct vfs *fs = vfs_new(&opts);

    ASSERT(vfs_quota(fs) == 100);
    ASSERT(vfs_usage(fs) == 0);

    // write 50 bytes
    char buf[50];
    memset(buf, 'A', sizeof(buf));
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a", buf, 50, 0), VFS_OK);
    ASSERT(vfs_usage(fs) == 50);

    // write 50 more is fine
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/b", buf, 50, 0), VFS_OK);
    ASSERT(vfs_usage(fs) == 100);

    // write 1 more byte exceeds quota
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/c", "x", 1, 0), VFS_EQUOTA);

    // overwriting with smaller data frees space
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/a", "small", 5, 0), VFS_OK);
    ASSERT(vfs_usage(fs) == 55);

    // now there's room
    ASSERT_INT_EQ(vfs_write(fs, &admin, "/c", buf, 45, 0), VFS_OK);
    ASSERT(vfs_usage(fs) == 100);

    // delete frees quota
    vfs_delete(fs, &admin, "/c");
    ASSERT(vfs_usage(fs) == 55);

    vfs_free(fs);
}

/****************************************************************
 * Umask
 ****************************************************************/

static void
test_umask(void)
{
    struct vfs_opts opts = { .umask = 0022 };
    struct vfs *fs = vfs_new(&opts);
    struct vfs_stat st;

    vfs_write(fs, &admin, "/f", "x", 1, 0);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/f", &st), VFS_OK);
    ASSERT_INT_EQ(st.mode, 0644); // 0666 & ~0022

    vfs_mkdir(fs, &admin, "/d", 0);
    ASSERT_INT_EQ(vfs_stat(fs, &admin, "/d", &st), VFS_OK);
    ASSERT_INT_EQ(st.mode, 0755); // 0777 & ~0022

    vfs_free(fs);
}

/****************************************************************
 * fnmatch
 ****************************************************************/

static void
test_fnmatch(void)
{
    // wildcard
    ASSERT(vfs_fnmatch("*.txt", "readme.txt"));
    ASSERT(vfs_fnmatch("*.txt", "README.TXT")); // case-insensitive
    ASSERT(!vfs_fnmatch("*.txt", "readme.md"));

    // question mark
    ASSERT(vfs_fnmatch("file?.c", "file1.c"));
    ASSERT(!vfs_fnmatch("file?.c", "file10.c"));

    // character class
    ASSERT(vfs_fnmatch("[abc]", "a"));
    ASSERT(vfs_fnmatch("[abc]", "B")); // case-insensitive
    ASSERT(!vfs_fnmatch("[abc]", "d"));

    // range
    ASSERT(vfs_fnmatch("[a-z]", "m"));
    ASSERT(!vfs_fnmatch("[a-c]", "d"));

    // negated class
    ASSERT(vfs_fnmatch("[!0-9]", "a"));
    ASSERT(!vfs_fnmatch("[!0-9]", "5"));

    // exact match
    ASSERT(vfs_fnmatch("hello", "hello"));
    ASSERT(!vfs_fnmatch("hello", "world"));

    // empty pattern vs empty name
    ASSERT(vfs_fnmatch("", ""));
    ASSERT(!vfs_fnmatch("", "x"));
    ASSERT(!vfs_fnmatch("x", ""));

    // star matches everything
    ASSERT(vfs_fnmatch("*", "anything"));
    ASSERT(vfs_fnmatch("*", ""));
}

/****************************************************************
 * Error strings
 ****************************************************************/

static void
test_strerror(void)
{
    ASSERT(strlen(vfs_strerror(VFS_OK)) > 0);
    ASSERT(strlen(vfs_strerror(VFS_ENOTFOUND)) > 0);
    ASSERT(strlen(vfs_strerror(VFS_EPERM)) > 0);
    ASSERT(strlen(vfs_strerror(-999)) > 0); // unknown
}

/****************************************************************
 * Main
 ****************************************************************/

int
main(void)
{
    fprintf(stderr, "--- vfs tests ---\n");

    RUN(test_normalize);
    RUN(test_resolve);
    RUN(test_lifecycle);
    RUN(test_mkdir);
    RUN(test_write_read);
    RUN(test_stat);
    RUN(test_list);
    RUN(test_delete);
    RUN(test_delete_recursive);
    RUN(test_rename);
    RUN(test_copy);
    RUN(test_chmod);
    RUN(test_permissions);
    RUN(test_quota);
    RUN(test_umask);
    RUN(test_fnmatch);
    RUN(test_strerror);

    TEST_REPORT();
}
