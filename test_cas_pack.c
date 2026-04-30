/* test_cas_pack.c : unit tests for the CAS packfile module */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas.h"
#include "cas-pack.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/test_cas_pack_XXXXXX";

static void
cleanup(void)
{
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
	if (system(cmd)) { /* best effort */ }
}

/****************************************************************
 * Helpers
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

/****************************************************************
 * test_pack_create_open
 ****************************************************************/

static void
test_pack_create_open(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/create", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];
	char h3[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "aaa", 3, h1), CAS_OK);
	ASSERT_INT_EQ(cas_put(store, "bbb", 3, h2), CAS_OK);
	ASSERT_INT_EQ(cas_put_object(store, "tree", "body", 4,
	                             h3), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);
	ASSERT_INT_EQ((int)cas_pack_count(pack), 3);

	cas_pack_close(pack);

	/* packing again overwrites */
	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);
	ASSERT_INT_EQ((int)cas_pack_count(pack), 3);
	cas_pack_close(pack);

	cas_free(store);
}

/****************************************************************
 * test_pack_lookup
 ****************************************************************/

static void
test_pack_lookup(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/lookup", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	char h_blob[CAS_HASH_HEX + 1];
	char h_tree[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "hello", 5, h_blob),
	              CAS_OK);
	ASSERT_INT_EQ(cas_put_object(store, "tree", "tdata", 5,
	                             h_tree), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, h_blob,
	              type, sizeof(type)), CAS_OK);
	ASSERT_STR_EQ(type, "blob");
	ASSERT_INT_EQ((int)cf.len, 5);
	ASSERT(memcmp(cf.data, "hello", 5) == 0);
	cas_close(&cf);

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, h_tree,
	              type, sizeof(type)), CAS_OK);
	ASSERT_STR_EQ(type, "tree");
	ASSERT_INT_EQ((int)cf.len, 5);
	ASSERT(memcmp(cf.data, "tdata", 5) == 0);
	cas_close(&cf);

	char fake[CAS_HASH_HEX + 1];
	memset(fake, '0', CAS_HASH_HEX);
	fake[CAS_HASH_HEX] = '\0';
	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, fake,
	              type, sizeof(type)), CAS_ENOTFOUND);

	cas_pack_close(pack);
	cas_free(store);
}

/****************************************************************
 * test_pack_exists
 ****************************************************************/

static void
test_pack_exists(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/exists", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	char hash[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "data", 4, hash), CAS_OK);
	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	ASSERT(cas_pack_exists(pack, hash));

	char fake[CAS_HASH_HEX + 1];
	memset(fake, '0', CAS_HASH_HEX);
	fake[CAS_HASH_HEX] = '\0';
	ASSERT(!cas_pack_exists(pack, fake));

	cas_pack_close(pack);
	cas_free(store);
}

/****************************************************************
 * test_pack_foreach
 ****************************************************************/

static void
test_pack_foreach(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/foreach", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "one", 3, h1), CAS_OK);
	ASSERT_INT_EQ(cas_put(store, "two", 3, h2), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	struct count_ctx cc = {0};
	cas_pack_foreach(pack, count_visitor, &cc);
	ASSERT_INT_EQ(cc.count, 2);

	cas_pack_close(pack);
	cas_free(store);
}

/****************************************************************
 * test_pack_fsck
 ****************************************************************/

static void
test_pack_fsck(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/fsck", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "x", 1, h1), CAS_OK);
	ASSERT_INT_EQ(cas_put(store, "y", 1, h2), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	struct fsck_result r = {0};
	ASSERT_INT_EQ(cas_pack_fsck(pack, fsck_counter, &r),
	              CAS_OK);
	ASSERT_INT_EQ(r.total, 2);
	ASSERT_INT_EQ(r.ok, 2);
	ASSERT_INT_EQ(r.corrupt, 0);

	cas_pack_close(pack);
	cas_free(store);
}

/****************************************************************
 * test_pack_integrated
 ****************************************************************/

static void
test_pack_integrated(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/integrated", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "alpha", 5, h1), CAS_OK);
	ASSERT_INT_EQ(cas_put(store, "beta", 4, h2), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	ASSERT_INT_EQ(cas_remove(store, h1), CAS_OK);
	ASSERT_INT_EQ(cas_remove(store, h2), CAS_OK);

	cas_free(store);
	store = cas_new(depot);
	ASSERT(store != NULL);

	ASSERT(cas_exists(store, h1));
	ASSERT(cas_exists(store, h2));

	struct cas_file cf;
	ASSERT_INT_EQ(cas_open(store, &cf, h1), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, 5);
	ASSERT(memcmp(cf.data, "alpha", 5) == 0);
	cas_close(&cf);

	char h1dup[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, "alpha", 5, h1dup),
	              CAS_OK);
	ASSERT_STR_EQ(h1, h1dup);

	struct count_ctx cc = {0};
	cas_foreach(store, count_visitor, &cc);
	ASSERT_INT_EQ(cc.count, 0);

	cas_free(store);
}

/****************************************************************
 * test_pack_empty
 ****************************************************************/

static void
test_pack_empty(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/empty", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);
	ASSERT(access(packpath, F_OK) != 0);

	cas_free(store);
}

/****************************************************************
 * test_pack_empty_blob
 ****************************************************************/

static void
test_pack_empty_blob(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/empty_blob", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	char hash[CAS_HASH_HEX + 1];

	ASSERT_INT_EQ(cas_put(store, "", 0, hash), CAS_OK);
	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);
	ASSERT_INT_EQ((int)cas_pack_count(pack), 1);

	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];
	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, hash,
	              type, sizeof(type)), CAS_OK);
	ASSERT_STR_EQ(type, "blob");
	ASSERT_INT_EQ((int)cf.len, 0);
	ASSERT(cf.data == NULL);
	cas_close(&cf);

	cas_pack_close(pack);
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

	fprintf(stderr, "--- cas-pack tests ---\n");

	RUN(test_pack_create_open);
	RUN(test_pack_lookup);
	RUN(test_pack_exists);
	RUN(test_pack_foreach);
	RUN(test_pack_fsck);
	RUN(test_pack_integrated);
	RUN(test_pack_empty);
	RUN(test_pack_empty_blob);

	TEST_REPORT();
}
