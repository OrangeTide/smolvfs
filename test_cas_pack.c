/* test_cas_pack.c : unit tests for the CAS packfile module */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas.h"
#include "cas-pack.h"
#include "cas-codec.h"
#include "test.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CAS_WITH_MINIZ
/* does haystack contain needle as a byte substring? */
static int
contains(const unsigned char *hay, size_t haylen,
         const unsigned char *needle, size_t needlelen)
{
	if (needlelen == 0 || needlelen > haylen)
		return 0;
	for (size_t i = 0; i + needlelen <= haylen; i++)
		if (memcmp(hay + i, needle, needlelen) == 0)
			return 1;
	return 0;
}
#endif

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
 * test_pack_endianness
 *
 * The packfile index and footer must store their 64-bit fields
 * little-endian so a pack is byte-identical across architectures.
 * Inspect the raw bytes: the low-order byte leads and the high
 * bytes are zero for small values.  This fails on any host that
 * writes the fields in native (e.g. big-endian) order.
 ****************************************************************/

static void
test_pack_endianness(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/endian", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, "alpha", 5, h1), CAS_OK);
	ASSERT_INT_EQ(cas_put(store, "beta", 4, h2), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	int fd = open(packpath, O_RDONLY);
	ASSERT(fd >= 0);
	off_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	ASSERT(sz >= 3 * CAS_PACK_BLOCK);

	unsigned char *buf = malloc((size_t)sz);
	ASSERT(buf != NULL);
	ASSERT_INT_EQ((int)read(fd, buf, (size_t)sz), (int)sz);
	close(fd);

	/* footer is the last block; entry_count is the 8 bytes after
	 * the 8-byte magic.  Two objects, so byte[0] == 2, rest zero. */
	const unsigned char *ec = buf + sz - CAS_PACK_BLOCK
	                          + CAS_PACK_MAGIC_LEN;
	ASSERT_INT_EQ(ec[0], 2);
	for (int i = 1; i < 8; i++)
		ASSERT_INT_EQ(ec[i], 0);

	/* first index entry precedes the footer by two blocks; its
	 * offset field (after the 32-byte hash) must little-endian
	 * decode to a valid in-file trailer offset. */
	const unsigned char *e0 = buf + sz - 3 * CAS_PACK_BLOCK;
	const unsigned char *offp = e0 + CAS_HASH_LEN;
	uint64_t off = 0;
	for (int i = 0; i < 8; i++)
		off |= (uint64_t)offp[i] << (i * 8);
	ASSERT(off + CAS_PACK_BLOCK <= (uint64_t)sz);

	unsigned char magic_v1[] = CAS_PACK_TRAILER_MAGIC;
	ASSERT(memcmp(buf + off, magic_v1, CAS_PACK_MAGIC_LEN) == 0);

	free(buf);
	cas_free(store);
}

#ifdef CAS_WITH_MINIZ
/****************************************************************
 * test_pack_compressed
 *
 * A packed v2 (compressed) object must stay compressed: the
 * packfile must hold the codec payload, not the inflated
 * plaintext, and lookups must decode it back.
 ****************************************************************/

static void
test_pack_compressed(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/compressed", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	/* a plain v1 object */
	char h_v1[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, "regular-v1-object", 17, h_v1),
	              CAS_OK);

	/* a compressible v2 object with a distinctive marker; after
	 * DEFLATE the marker no longer appears literally */
	unsigned char plain[300];
	memset(plain, 'A', sizeof(plain));
	memcpy(plain + 120, "COMPRESSED-PAYLOAD-MARKER-XYZ", 29);
	size_t len = sizeof(plain);

	unsigned char payload[512];
	size_t plen = sizeof(payload);
	ASSERT_INT_EQ(cas_codec_encode(CAS_CODEC_DEFLATE, plain, len,
	                               payload, &plen), CAS_OK);
	ASSERT(plen < len);

	char h_v2[CAS_HASH_HEX + 1];
	cas_hash_object("blob", plain, len, h_v2);
	ASSERT_INT_EQ(cas_put_precompressed(store, "blob", CAS_CODEC_DEFLATE,
	                                    payload, plen, len, h_v2),
	              CAS_OK);

	ASSERT_INT_EQ(cas_pack_create(store, packpath), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);
	ASSERT_INT_EQ((int)cas_pack_count(pack), 2);

	/* v1 lookup returns plaintext */
	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, h_v1, type,
	              sizeof(type)), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, 17);
	ASSERT(memcmp(cf.data, "regular-v1-object", 17) == 0);
	cas_close(&cf);

	/* v2 lookup decodes back to plaintext */
	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, h_v2, type,
	              sizeof(type)), CAS_OK);
	ASSERT_STR_EQ(type, "blob");
	ASSERT_INT_EQ((int)cf.len, (int)len);
	ASSERT(memcmp(cf.data, plain, len) == 0);
	cas_close(&cf);

	/* fsck decodes both objects and verifies their hashes */
	struct fsck_result fr = {0};
	ASSERT_INT_EQ(cas_pack_fsck(pack, fsck_counter, &fr), CAS_OK);
	ASSERT_INT_EQ(fr.total, 2);
	ASSERT_INT_EQ(fr.ok, 2);
	cas_pack_close(pack);

	/* the packfile holds the payload, never the inflated plaintext */
	int fd = open(packpath, O_RDONLY);
	ASSERT(fd >= 0);

	off_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	unsigned char *buf = malloc((size_t)sz);
	ASSERT(buf != NULL);
	ASSERT_INT_EQ((int)read(fd, buf, (size_t)sz), (int)sz);
	close(fd);

	ASSERT(contains(buf, (size_t)sz, payload, plen));
	ASSERT(!contains(buf, (size_t)sz,
	                 (const unsigned char *)"COMPRESSED-PAYLOAD-MARKER-XYZ",
	                 29));

	free(buf);
	cas_free(store);
}

/* cas_pack_create_z under GUESS: compress text, leave binary raw */
static void
test_pack_create_z(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/create_z", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	/* a compressible text blob (all printable) */
	unsigned char text[4096];
	memset(text, 'Q', sizeof(text));
	char thash[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, text, sizeof(text), thash), CAS_OK);

	/* an incompressible binary blob (half control bytes) */
	unsigned char bin[4096];
	for (size_t i = 0; i < sizeof(bin); i++)
		bin[i] = (i & 1) ? 0x01 : 0xa5;
	char bhash[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, bin, sizeof(bin), bhash), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create_z(store, packpath, CAS_COMPRESS_GUESS,
	              CAS_CODEC_DEFLATE), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);
	ASSERT_INT_EQ((int)cas_pack_count(pack), 2);

	/* both objects decode back to their originals */
	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, thash, type,
	              sizeof(type)), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, (int)sizeof(text));
	ASSERT(memcmp(cf.data, text, sizeof(text)) == 0);
	cas_close(&cf);

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, bhash, type,
	              sizeof(type)), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, (int)sizeof(bin));
	ASSERT(memcmp(cf.data, bin, sizeof(bin)) == 0);
	cas_close(&cf);

	struct fsck_result fr = {0};
	ASSERT_INT_EQ(cas_pack_fsck(pack, fsck_counter, &fr), CAS_OK);
	ASSERT_INT_EQ(fr.ok, 2);
	cas_pack_close(pack);

	/* the binary blob is stored raw (GUESS skipped it), so its bytes
	 * appear verbatim in the packfile; the highly compressible text
	 * blob was shrunk, so 4096 'Q' bytes do not appear contiguously */
	int fd = open(packpath, O_RDONLY);
	ASSERT(fd >= 0);
	off_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	unsigned char *buf = malloc((size_t)sz);
	ASSERT(buf != NULL);
	ASSERT_INT_EQ((int)read(fd, buf, (size_t)sz), (int)sz);
	close(fd);

	ASSERT(contains(buf, (size_t)sz, bin, sizeof(bin)));
	ASSERT(!contains(buf, (size_t)sz, text, sizeof(text)));

	free(buf);
	cas_free(store);
}

/* cas_pack_create_z under ALWAYS on incompressible data: the encoder
 * is attempted, expands the data past the save-space margin, so the
 * object is kept raw and still decodes.  This is the path GUESS skips
 * before ever calling the encoder. */
static void
test_pack_create_z_always(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/create_z_always", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	/* high-entropy bytes (xorshift64star) that DEFLATE cannot
	 * shrink, so the packer's save-space margin rejects the
	 * attempt even though ALWAYS asked for it */
	unsigned char rnd[4096];
	uint64_t s = 0x9e3779b97f4a7c15ULL;
	for (size_t i = 0; i < sizeof(rnd); i++) {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		rnd[i] = (unsigned char)((s * 0x2545f4914f6cdd1dULL) >> 56);
	}
	char rhash[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, rnd, sizeof(rnd), rhash), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create_z(store, packpath, CAS_COMPRESS_ALWAYS,
	              CAS_CODEC_DEFLATE), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, rhash, type,
	              sizeof(type)), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, (int)sizeof(rnd));
	ASSERT(memcmp(cf.data, rnd, sizeof(rnd)) == 0);
	cas_close(&cf);

	struct fsck_result fr = {0};
	ASSERT_INT_EQ(cas_pack_fsck(pack, fsck_counter, &fr), CAS_OK);
	ASSERT_INT_EQ(fr.ok, 1);
	cas_pack_close(pack);

	/* the attempt was rejected, so the object sits raw in the pack */
	int fd = open(packpath, O_RDONLY);
	ASSERT(fd >= 0);
	off_t sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	unsigned char *buf = malloc((size_t)sz);
	ASSERT(buf != NULL);
	ASSERT_INT_EQ((int)read(fd, buf, (size_t)sz), (int)sz);
	close(fd);

	ASSERT(contains(buf, (size_t)sz, rnd, sizeof(rnd)));

	free(buf);
	cas_free(store);
}
#else
/****************************************************************
 * test_pack_create_z_no_encoder
 *
 * With no codec compiled in, cas_pack_create_z must still succeed:
 * every object is stored raw (the encoder-absent fallback) and reads
 * back intact regardless of the policy requested.
 ****************************************************************/

static void
test_pack_create_z_no_encoder(void)
{
	char depot[512], packpath[512];

	snprintf(depot, sizeof(depot), "%s/create_z_noenc", tmpdir);
	snprintf(packpath, sizeof(packpath), "%s/pack.dat",
	         depot);

	struct cas *store = cas_new(depot);
	ASSERT(store != NULL);

	/* text that GUESS would want to compress if an encoder existed */
	unsigned char text[4096];
	memset(text, 'Q', sizeof(text));
	char thash[CAS_HASH_HEX + 1];
	ASSERT_INT_EQ(cas_put(store, text, sizeof(text), thash), CAS_OK);

	ASSERT_INT_EQ(cas_pack_create_z(store, packpath, CAS_COMPRESS_GUESS,
	              CAS_CODEC_DEFLATE), CAS_OK);

	struct cas_pack *pack = cas_pack_open(packpath);
	ASSERT(pack != NULL);

	struct cas_file cf;
	char type[CAS_TYPE_MAX + 1];

	ASSERT_INT_EQ(cas_pack_lookup(pack, &cf, thash, type,
	              sizeof(type)), CAS_OK);
	ASSERT_INT_EQ((int)cf.len, (int)sizeof(text));
	ASSERT(memcmp(cf.data, text, sizeof(text)) == 0);
	cas_close(&cf);

	struct fsck_result fr = {0};
	ASSERT_INT_EQ(cas_pack_fsck(pack, fsck_counter, &fr), CAS_OK);
	ASSERT_INT_EQ(fr.ok, 1);
	cas_pack_close(pack);

	cas_free(store);
}
#endif

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
	RUN(test_pack_endianness);
#ifdef CAS_WITH_MINIZ
	RUN(test_pack_compressed);
	RUN(test_pack_create_z);
	RUN(test_pack_create_z_always);
#else
	RUN(test_pack_create_z_no_encoder);
#endif
	RUN(test_pack_lookup);
	RUN(test_pack_exists);
	RUN(test_pack_foreach);
	RUN(test_pack_fsck);
	RUN(test_pack_integrated);
	RUN(test_pack_empty);
	RUN(test_pack_empty_blob);

	TEST_REPORT();
}
