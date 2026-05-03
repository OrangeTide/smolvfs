/* test_cas_omap.c : unit tests for the cas-omap module */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#define _POSIX_C_SOURCE 200809L

#include "cas-omap.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/test_cas_omap_XXXXXX";

static void
cleanup(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd)) { /* best effort */ }
}

static struct cas *
make_store(const char *name)
{
    char depot[512];

    snprintf(depot, sizeof(depot), "%s/%s", tmpdir, name);
    return cas_new(depot);
}

static void
put_dummy(struct cas *store, const char *data, char *hash_out)
{
    cas_put(store, data, strlen(data), hash_out);
}

/****************************************************************
 * Basic put/get
 ****************************************************************/

static void
test_put_get(void)
{
    struct cas *store = make_store("putget");
    struct cas_omap *om = cas_omap_new(store);

    ASSERT(om != NULL);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    put_dummy(store, "hello", h1);
    put_dummy(store, "world", h2);

    ASSERT_INT_EQ(cas_omap_put(om, 1200000, h1), CAS_OK);
    ASSERT_INT_EQ(cas_omap_put(om, 1200001, h2), CAS_OK);

    char out[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_get(om, 1200000, out), CAS_OK);
    ASSERT_STR_EQ(out, h1);
    ASSERT_INT_EQ(cas_omap_get(om, 1200001, out), CAS_OK);
    ASSERT_STR_EQ(out, h2);
    ASSERT_INT_EQ(cas_omap_get(om, 999, out), CAS_ENOTFOUND);

    ASSERT(cas_omap_exists(om, 1200000));
    ASSERT(!cas_omap_exists(om, 999));

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Delete
 ****************************************************************/

static void
test_delete(void)
{
    struct cas *store = make_store("del");
    struct cas_omap *om = cas_omap_new(store);

    char h[CAS_HASH_HEX + 1];

    put_dummy(store, "data", h);

    ASSERT_INT_EQ(cas_omap_put(om, 42, h), CAS_OK);
    ASSERT_INT_EQ(cas_omap_del(om, 42), CAS_OK);
    ASSERT_INT_EQ(cas_omap_del(om, 42), CAS_ENOTFOUND);
    ASSERT(!cas_omap_exists(om, 42));

    uint64_t count = cas_omap_count(om);

    ASSERT_INT_EQ((int)count, 0);

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Snapshot store/load round-trip
 ****************************************************************/

static void
test_snapshot(void)
{
    struct cas *store = make_store("snap");
    struct cas_omap *om = cas_omap_new(store);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];
    char h3[CAS_HASH_HEX + 1];

    put_dummy(store, "alpha", h1);
    put_dummy(store, "beta", h2);
    put_dummy(store, "gamma", h3);

    cas_omap_put(om, 5000, h1);
    cas_omap_put(om, 5001, h2);
    cas_omap_put(om, 1300000, h3);

    char root[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_store(om, root), CAS_OK);

    struct cas_omap *om2 = cas_omap_new(store);

    ASSERT_INT_EQ(cas_omap_load(om2, root), CAS_OK);

    char out[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_get(om2, 5000, out), CAS_OK);
    ASSERT_STR_EQ(out, h1);
    ASSERT_INT_EQ(cas_omap_get(om2, 5001, out), CAS_OK);
    ASSERT_STR_EQ(out, h2);
    ASSERT_INT_EQ(cas_omap_get(om2, 1300000, out), CAS_OK);
    ASSERT_STR_EQ(out, h3);

    uint64_t count = cas_omap_count(om2);

    ASSERT_INT_EQ((int)count, 3);

    cas_omap_free(om2);
    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Allocation
 ****************************************************************/

static void
test_alloc(void)
{
    struct cas *store = make_store("alloc");
    struct cas_omap *om = cas_omap_new(store);

    char h[CAS_HASH_HEX + 1];

    put_dummy(store, "obj", h);

    cas_omap_put(om, 1200000, h);
    cas_omap_put(om, 1200001, h);
    cas_omap_put(om, 1200002, h);

    uint64_t id;

    ASSERT_INT_EQ(cas_omap_alloc(om, 1200000, 100, &id), CAS_OK);
    ASSERT_INT_EQ((int)(id - 1200000), 3);

    ASSERT_INT_EQ(cas_omap_alloc(om, 1200005, 100, &id), CAS_OK);
    ASSERT_INT_EQ((int)(id - 1200000), 5);

    // alloc in empty region
    ASSERT_INT_EQ(cas_omap_alloc(om, 9999000, 0, &id), CAS_OK);
    ASSERT_INT_EQ((int)id, 9999000);

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Alloc in full range
 ****************************************************************/

static void
test_alloc_full(void)
{
    struct cas *store = make_store("full");
    struct cas_omap *om = cas_omap_new(store);

    char h[CAS_HASH_HEX + 1];

    put_dummy(store, "x", h);

    for (int i = 0; i < 5; i++)
        cas_omap_put(om, 100 + (uint64_t)i, h);

    uint64_t id;

    ASSERT_INT_EQ(cas_omap_alloc(om, 100, 5, &id), CAS_OMAP_EFULL);

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Foreach iteration
 ****************************************************************/

struct iter_ctx {
    uint64_t ids[16];
    int count;
};

static int
iter_cb(uint64_t id, const char *hash, void *ctx)
{
    (void)hash;
    struct iter_ctx *ic = ctx;

    if (ic->count < 16)
        ic->ids[ic->count] = id;
    ic->count++;
    return 0;
}

static void
test_foreach(void)
{
    struct cas *store = make_store("iter");
    struct cas_omap *om = cas_omap_new(store);

    char h[CAS_HASH_HEX + 1];

    put_dummy(store, "v", h);

    cas_omap_put(om, 500, h);
    cas_omap_put(om, 1000000, h);
    cas_omap_put(om, 200, h);

    struct iter_ctx ic = { .count = 0 };

    ASSERT_INT_EQ(cas_omap_foreach(om, iter_cb, &ic), CAS_OK);
    ASSERT_INT_EQ(ic.count, 3);
    ASSERT_INT_EQ((int)ic.ids[0], 200);
    ASSERT_INT_EQ((int)ic.ids[1], 500);
    ASSERT_INT_EQ((int)ic.ids[2], 1000000);

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Overwrite existing slot
 ****************************************************************/

static void
test_overwrite(void)
{
    struct cas *store = make_store("overwrite");
    struct cas_omap *om = cas_omap_new(store);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    put_dummy(store, "first", h1);
    put_dummy(store, "second", h2);

    cas_omap_put(om, 7, h1);
    cas_omap_put(om, 7, h2);

    char out[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_get(om, 7, out), CAS_OK);
    ASSERT_STR_EQ(out, h2);

    uint64_t count = cas_omap_count(om);

    ASSERT_INT_EQ((int)count, 1);

    cas_omap_free(om);
    cas_free(store);
}

/****************************************************************
 * Multiple snapshots share pages
 ****************************************************************/

static void
test_cow(void)
{
    struct cas *store = make_store("cow");
    struct cas_omap *om = cas_omap_new(store);

    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    put_dummy(store, "aaa", h1);
    put_dummy(store, "bbb", h2);

    cas_omap_put(om, 0, h1);
    cas_omap_put(om, 256, h1);

    char snap1[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_store(om, snap1), CAS_OK);

    // modify only page 0
    cas_omap_put(om, 1, h2);

    char snap2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_omap_store(om, snap2), CAS_OK);

    // snap1 still works
    struct cas_omap *om1 = cas_omap_new(store);

    ASSERT_INT_EQ(cas_omap_load(om1, snap1), CAS_OK);
    ASSERT(!cas_omap_exists(om1, 1));
    ASSERT(cas_omap_exists(om1, 0));
    ASSERT(cas_omap_exists(om1, 256));

    // snap2 has the new entry
    struct cas_omap *om2 = cas_omap_new(store);

    ASSERT_INT_EQ(cas_omap_load(om2, snap2), CAS_OK);
    ASSERT(cas_omap_exists(om2, 1));

    cas_omap_free(om2);
    cas_omap_free(om1);
    cas_omap_free(om);
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

    fprintf(stderr, "cas-omap tests:\n");
    RUN(test_put_get);
    RUN(test_delete);
    RUN(test_snapshot);
    RUN(test_alloc);
    RUN(test_alloc_full);
    RUN(test_foreach);
    RUN(test_overwrite);
    RUN(test_cow);
    TEST_REPORT();
}
