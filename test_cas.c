/* test_cas.c : unit tests for the CAS module */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/test_cas_XXXXXX";

static void
cleanup(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd)) { /* best effort */ }
}

/****************************************************************
 * Hash
 ****************************************************************/

static void
test_hash_known_vector(void)
{
    char hash[CAS_HASH_HEX + 1];

    // RFC 7693 appendix A: BLAKE2b-256("abc")
    cas_hash("abc", 3, hash);
    ASSERT_STR_EQ(hash,
        "bddd813c634239723171ef3fee98579b"
        "94964e3bb1cb3e427262c8c068d52319");
}

static void
test_hash_empty(void)
{
    char hash[CAS_HASH_HEX + 1];

    // BLAKE2b-256 of empty input
    cas_hash("", 0, hash);
    ASSERT_INT_EQ((int)strlen(hash), CAS_HASH_HEX);

    // same input produces the same hash
    char hash2[CAS_HASH_HEX + 1];
    cas_hash("", 0, hash2);
    ASSERT_STR_EQ(hash, hash2);
}

static void
test_hash_differs(void)
{
    char h1[CAS_HASH_HEX + 1];
    char h2[CAS_HASH_HEX + 1];

    cas_hash("hello", 5, h1);
    cas_hash("world", 5, h2);
    ASSERT(strcmp(h1, h2) != 0);
}

/****************************************************************
 * Put, open, close
 ****************************************************************/

static void
test_put_open(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/depot", tmpdir);

    struct cas *store = cas_new(depot);
    ASSERT(store != NULL);

    char hash[CAS_HASH_HEX + 1];
    ASSERT_INT_EQ(cas_put(store, "hello world", 11, hash), CAS_OK);
    ASSERT_INT_EQ((int)strlen(hash), CAS_HASH_HEX);

    // verify by opening
    struct cas_file cf;
    ASSERT_INT_EQ(cas_open(store, &cf, hash), CAS_OK);
    ASSERT_INT_EQ((int)cf.len, 11);
    ASSERT(memcmp(cf.data, "hello world", 11) == 0);
    cas_close(&cf);

    // hash matches typed-object computation
    char expected[CAS_HASH_HEX + 1];
    cas_hash_object("blob", "hello world", 11, expected);
    ASSERT_STR_EQ(hash, expected);

    cas_free(store);
}

/****************************************************************
 * Deduplication
 ****************************************************************/

static void
test_dedup(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/dedup", tmpdir);

    struct cas *store = cas_new(depot);
    char h1[CAS_HASH_HEX + 1];
    char h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "same", 4, h1), CAS_OK);
    ASSERT_INT_EQ(cas_put(store, "same", 4, h2), CAS_OK);
    ASSERT_STR_EQ(h1, h2);

    cas_free(store);
}

/****************************************************************
 * Exists
 ****************************************************************/

static void
test_exists(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/exists", tmpdir);

    struct cas *store = cas_new(depot);
    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "data", 4, hash), CAS_OK);
    ASSERT(cas_exists(store, hash));

    // nonexistent hash
    char fake[CAS_HASH_HEX + 1];
    memset(fake, '0', CAS_HASH_HEX);
    fake[CAS_HASH_HEX] = '\0';
    ASSERT(!cas_exists(store, fake));

    // invalid hash string
    ASSERT(!cas_exists(store, "not-a-hash"));
    ASSERT(!cas_exists(store, ""));

    cas_free(store);
}

/****************************************************************
 * Empty blob
 ****************************************************************/

static void
test_empty_blob(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/empty", tmpdir);

    struct cas *store = cas_new(depot);
    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "", 0, hash), CAS_OK);
    ASSERT(cas_exists(store, hash));

    struct cas_file cf;
    ASSERT_INT_EQ(cas_open(store, &cf, hash), CAS_OK);
    ASSERT_INT_EQ((int)cf.len, 0);
    ASSERT(cf.data == NULL);
    cas_close(&cf);

    cas_free(store);
}

/****************************************************************
 * Open nonexistent
 ****************************************************************/

static void
test_open_missing(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/missing", tmpdir);

    struct cas *store = cas_new(depot);
    struct cas_file cf;

    char fake[CAS_HASH_HEX + 1];
    memset(fake, 'a', CAS_HASH_HEX);
    fake[CAS_HASH_HEX] = '\0';
    ASSERT_INT_EQ(cas_open(store, &cf, fake), CAS_ENOTFOUND);

    // malformed hash
    ASSERT_INT_EQ(cas_open(store, &cf, "bad"), CAS_ERR);

    cas_free(store);
}

/****************************************************************
 * Error strings
 ****************************************************************/

static void
test_cas_strerror(void)
{
    ASSERT(strlen(cas_strerror(CAS_OK)) > 0);
    ASSERT(strlen(cas_strerror(CAS_EIO)) > 0);
    ASSERT(strlen(cas_strerror(-999)) > 0);
}

/****************************************************************
 * Typed object API
 ****************************************************************/

static void
test_object_api(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/object", tmpdir);

    struct cas *store = cas_new(depot);
    ASSERT(store != NULL);

    // store a "tree" object
    char hash[CAS_HASH_HEX + 1];
    ASSERT_INT_EQ(cas_put_object(store, "tree", "body", 4, hash),
                  CAS_OK);

    // open as typed object, verify type
    struct cas_file cf;
    char type[CAS_TYPE_MAX + 1];
    ASSERT_INT_EQ(cas_open_object(store, &cf, hash, type,
                                  sizeof(type)), CAS_OK);
    ASSERT_STR_EQ(type, "tree");
    ASSERT_INT_EQ((int)cf.len, 4);
    ASSERT(memcmp(cf.data, "body", 4) == 0);
    cas_close(&cf);

    // cas_open rejects non-blob objects
    ASSERT_INT_EQ(cas_open(store, &cf, hash), CAS_ETYPE);

    // cas_hash_object matches stored hash
    char expected[CAS_HASH_HEX + 1];
    cas_hash_object("tree", "body", 4, expected);
    ASSERT_STR_EQ(hash, expected);

    // cas_hash_object differs from raw cas_hash
    char raw[CAS_HASH_HEX + 1];
    cas_hash("body", 4, raw);
    ASSERT(strcmp(hash, raw) != 0);

    cas_free(store);
}

/****************************************************************
 * Fsck
 ****************************************************************/

struct fsck_result {
    int ok;
    int corrupt;
    int total;
};

static int
fsck_counter(const char *hash, int status, void *ctx)
{
    (void)hash;
    struct fsck_result *r = ctx;

    r->total++;
    if (status == CAS_FSCK_OK)
        r->ok++;
    else if (status == CAS_FSCK_CORRUPT)
        r->corrupt++;
    return 0;
}

static void
test_fsck_clean(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/fsck_clean", tmpdir);

    struct cas *store = cas_new(depot);
    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "aaa", 3, h1), CAS_OK);
    ASSERT_INT_EQ(cas_put(store, "bbb", 3, h2), CAS_OK);

    struct fsck_result r = {0};

    ASSERT_INT_EQ(cas_fsck(store, fsck_counter, &r), CAS_OK);
    ASSERT_INT_EQ(r.total, 2);
    ASSERT_INT_EQ(r.ok, 2);
    ASSERT_INT_EQ(r.corrupt, 0);

    cas_free(store);
}

static void
test_fsck_corrupt(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/fsck_corrupt", tmpdir);

    struct cas *store = cas_new(depot);
    char hash[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "good data", 9, hash), CAS_OK);

    /* corrupt the file by appending a byte */
    char path[512];
    snprintf(path, sizeof(path), "%s/%.2s/%s", depot, hash, hash);
    FILE *fp = fopen(path, "a");
    ASSERT(fp != NULL);
    fputc('X', fp);
    fclose(fp);

    ASSERT_INT_EQ(cas_fsck_object(store, hash), CAS_FSCK_CORRUPT);

    struct fsck_result r = {0};

    ASSERT_INT_EQ(cas_fsck(store, fsck_counter, &r), CAS_ERR);
    ASSERT_INT_EQ(r.corrupt, 1);

    cas_free(store);
}

static void
test_fsck_empty_store(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/fsck_empty", tmpdir);

    struct cas *store = cas_new(depot);
    struct fsck_result r = {0};

    ASSERT_INT_EQ(cas_fsck(store, fsck_counter, &r), CAS_OK);
    ASSERT_INT_EQ(r.total, 0);

    cas_free(store);
}

/****************************************************************
 * Foreach and remove
 ****************************************************************/

struct count_ctx {
    int count;
};

static int
count_visitor(const char *hash, void *ctx)
{
    (void)hash;
    struct count_ctx *c = ctx;

    c->count++;
    return 0;
}

static void
test_foreach(void)
{
    char depot[512];
    snprintf(depot, sizeof(depot), "%s/foreach", tmpdir);

    struct cas *store = cas_new(depot);
    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];
    char h3[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_put(store, "one", 3, h1), CAS_OK);
    ASSERT_INT_EQ(cas_put(store, "two", 3, h2), CAS_OK);
    ASSERT_INT_EQ(cas_put(store, "three", 5, h3), CAS_OK);

    struct count_ctx cc = {0};

    cas_foreach(store, count_visitor, &cc);
    ASSERT_INT_EQ(cc.count, 3);

    ASSERT_INT_EQ(cas_remove(store, h2), CAS_OK);
    ASSERT(!cas_exists(store, h2));

    cc.count = 0;
    cas_foreach(store, count_visitor, &cc);
    ASSERT_INT_EQ(cc.count, 2);

    /* remove nonexistent */
    ASSERT_INT_EQ(cas_remove(store, h2), CAS_ENOTFOUND);

    cas_free(store);
}

/****************************************************************
 * Lifecycle
 ****************************************************************/

static void
test_cas_lifecycle(void)
{
    struct cas *store = cas_new(NULL); // defaults to "depot"
    ASSERT(store != NULL);
    cas_free(store);
    cas_free(NULL); // should not crash
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

    fprintf(stderr, "--- cas tests ---\n");

    RUN(test_hash_known_vector);
    RUN(test_hash_empty);
    RUN(test_hash_differs);
    RUN(test_put_open);
    RUN(test_dedup);
    RUN(test_exists);
    RUN(test_empty_blob);
    RUN(test_open_missing);
    RUN(test_cas_strerror);
    RUN(test_object_api);
    RUN(test_fsck_clean);
    RUN(test_fsck_corrupt);
    RUN(test_fsck_empty_store);
    RUN(test_foreach);
    RUN(test_cas_lifecycle);

    TEST_REPORT();
}
