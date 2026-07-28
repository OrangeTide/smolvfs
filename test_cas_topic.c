/* test_cas_topic.c : unit tests for signed topics over refs */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas-topic.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[] = "/tmp/test_cas_topic_XXXXXX";

static void
cleanup(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd)) { /* best effort */ }
}

static const unsigned char seed_a[CAS_SIGN_SEED_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

static struct cas_tree *
make_tree(const char *name, struct cas **store_out)
{
    char depot[512];

    snprintf(depot, sizeof(depot), "%s/%s", tmpdir, name);

    struct cas *store = cas_new(depot);

    if (!store)
        return NULL;
    *store_out = store;
    return cas_tree_new(store);
}

/* Build record `seq` of a chain, returning its bytes and address. */
static void
mkrec(unsigned char out[CAS_VREC_LEN], char addr[CAS_HASH_HEX + 1],
      const unsigned char sk[CAS_SIGN_SECKEY_LEN],
      const unsigned char pk[CAS_SIGN_PUBKEY_LEN],
      uint64_t seq, const char *prev, unsigned rootbias)
{
    struct cas_vrec v;

    memset(&v, 0, sizeof(v));
    memcpy(v.pubkey, pk, CAS_SIGN_PUBKEY_LEN);
    v.seq = seq;
    v.timestamp = 1700000000 + (int64_t)seq;
    snprintf(v.root, sizeof(v.root), "%064llx",
             (unsigned long long)(0x2000 + seq * 16 + rootbias));
    if (prev)
        snprintf(v.prev, sizeof(v.prev), "%s", prev);

    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, out), CAS_OK);
    ASSERT_INT_EQ(cas_vrec_address(out, addr), CAS_OK);
}

/****************************************************************
 * Ref naming
 ****************************************************************/

static void
test_ref_name(void)
{
    unsigned char id[CAS_HASH_LEN];
    char name[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

    memset(id, 0xab, sizeof(id));

    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, NULL),
                  CAS_OK);
    ASSERT_INT_EQ((int)strlen(name), CAS_HASH_HEX);

    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "assets"),
                  CAS_OK);
    ASSERT_INT_EQ((int)strlen(name), CAS_HASH_HEX + 1 + 6);
    ASSERT_STR_EQ(name + CAS_HASH_HEX, "-assets");

    /* The whole reason for encoding rather than allowing a hierarchy:
     * no label can put a separator into a ref name, so nothing here can
     * reach outside refs/. */
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "a/b"),
                  CAS_ERR);
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "..'"),
                  CAS_ERR);
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "../x"),
                  CAS_ERR);
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "a\\b"),
                  CAS_ERR);

    /* leading '.' hides the ref, leading '-' reads as an option */
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, ".hidden"),
                  CAS_ERR);
    ASSERT_INT_EQ(cas_topic_ref_name(name, sizeof(name), id, "-rf"),
                  CAS_ERR);

    /* a buffer that cannot hold the result fails rather than truncating
     * into some other topic's name */
    char small[8];

    ASSERT_INT_EQ(cas_topic_ref_name(small, sizeof(small), id, NULL),
                  CAS_ERR);
}

/****************************************************************
 * Publish and resolve
 ****************************************************************/

static void
test_publish_and_head(void)
{
    if (!cas_sign_available())
        return;

    struct cas *store;
    struct cas_tree *ct = make_tree("pub", &store);

    ASSERT(ct != NULL);
    if (!ct)
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    unsigned char id[CAS_HASH_LEN];
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

    cas_sign_topic_id(id, pk);
    ASSERT_INT_EQ(cas_topic_ref_name(refname, sizeof(refname), id,
                                     "world"), CAS_OK);

    struct cas_vrec head;

    /* nothing published yet */
    ASSERT_INT_EQ(cas_topic_head(ct, refname, &head, NULL),
                  CAS_ENOTFOUND);

    unsigned char r1[CAS_VREC_LEN], r2[CAS_VREC_LEN];
    char a1[CAS_HASH_HEX + 1], a2[CAS_HASH_HEX + 1];

    mkrec(r1, a1, sk, pk, 1, NULL, 0);
    mkrec(r2, a2, sk, pk, 2, a1, 0);

    ASSERT_INT_EQ(cas_topic_publish(ct, refname, r1, "first"), CAS_OK);

    char got_addr[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_topic_head(ct, refname, &head, got_addr), CAS_OK);
    ASSERT_INT_EQ((int)head.seq, 1);
    ASSERT_STR_EQ(got_addr, a1);

    /* The ref names the record, not the root.  That is what makes every
     * resolution of a topic pass through a signature check. */
    char raw[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_tree_ref_read(ct, refname, raw), CAS_OK);
    ASSERT_STR_EQ(raw, a1);
    ASSERT(strcmp(raw, head.root) != 0);

    ASSERT_INT_EQ(cas_topic_publish(ct, refname, r2, "second"), CAS_OK);
    ASSERT_INT_EQ(cas_topic_head(ct, refname, &head, got_addr), CAS_OK);
    ASSERT_INT_EQ((int)head.seq, 2);
    ASSERT_STR_EQ(got_addr, a2);

    cas_tree_free(ct);
    cas_free(store);
}

/* A refusal must leave the topic exactly as it was. */
static void
check_refused(struct cas_tree *ct, const char *refname,
              const unsigned char *rec, int want, const char *keep_addr)
{
    ASSERT_INT_EQ(cas_topic_publish(ct, refname, rec, "nope"), want);

    char addr[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_tree_ref_read(ct, refname, addr), CAS_OK);
    ASSERT_STR_EQ(addr, keep_addr);
}

static void
test_refusals_do_not_move_the_ref(void)
{
    if (!cas_sign_available())
        return;

    struct cas *store;
    struct cas_tree *ct = make_tree("refuse", &store);

    ASSERT(ct != NULL);
    if (!ct)
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    unsigned char id[CAS_HASH_LEN];
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

    cas_sign_topic_id(id, pk);
    ASSERT_INT_EQ(cas_topic_ref_name(refname, sizeof(refname), id, NULL),
                  CAS_OK);

    unsigned char r1[CAS_VREC_LEN], r2[CAS_VREC_LEN], r3[CAS_VREC_LEN];
    char a1[CAS_HASH_HEX + 1], a2[CAS_HASH_HEX + 1], a3[CAS_HASH_HEX + 1];

    mkrec(r1, a1, sk, pk, 1, NULL, 0);
    mkrec(r2, a2, sk, pk, 2, a1, 0);
    mkrec(r3, a3, sk, pk, 3, a2, 0);

    ASSERT_INT_EQ(cas_topic_publish(ct, refname, r1, "first"), CAS_OK);
    ASSERT_INT_EQ(cas_topic_publish(ct, refname, r2, "second"), CAS_OK);

    /* older than the head */
    check_refused(ct, refname, r1, CAS_SIGN_ESTALE, a2);

    /* a jump: newer, but the caller has to decide about the hole */
    unsigned char r5[CAS_VREC_LEN];
    char a5[CAS_HASH_HEX + 1];

    mkrec(r5, a5, sk, pk, 5, a3, 0);
    check_refused(ct, refname, r5, CAS_SIGN_EGAP, a2);

    /* a second, different record at the head's seq: key compromise */
    unsigned char rfork[CAS_VREC_LEN];
    char afork[CAS_HASH_HEX + 1];

    mkrec(rfork, afork, sk, pk, 2, a1, 7);
    ASSERT(strcmp(afork, a2) != 0);
    check_refused(ct, refname, rfork, CAS_SIGN_EFORK, a2);

    /* right seq, but linking to something that is not the head */
    unsigned char rbad[CAS_VREC_LEN];
    char abad[CAS_HASH_HEX + 1];

    mkrec(rbad, abad, sk, pk, 3, a1, 0);
    check_refused(ct, refname, rbad, CAS_SIGN_ECHAIN, a2);

    /* a tampered record never gets as far as the succession rules */
    unsigned char rt[CAS_VREC_LEN];

    memcpy(rt, r3, sizeof(rt));
    rt[88] ^= 0x01;                    /* the published root */
    check_refused(ct, refname, rt, CAS_SIGN_EBADSIG, a2);

    /* and the real successor still works afterwards */
    ASSERT_INT_EQ(cas_topic_publish(ct, refname, r3, "third"), CAS_OK);

    struct cas_vrec head;

    ASSERT_INT_EQ(cas_topic_head(ct, refname, &head, NULL), CAS_OK);
    ASSERT_INT_EQ((int)head.seq, 3);

    cas_tree_free(ct);
    cas_free(store);
}

static void
test_another_publisher_cannot_take_a_topic(void)
{
    if (!cas_sign_available())
        return;

    struct cas *store;
    struct cas_tree *ct = make_tree("steal", &store);

    ASSERT(ct != NULL);
    if (!ct)
        return;

    static const unsigned char seed_b[CAS_SIGN_SEED_LEN] = {
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
        0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
        0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
        0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0,
    };
    unsigned char sk_a[CAS_SIGN_SECKEY_LEN], pk_a[CAS_SIGN_PUBKEY_LEN];
    unsigned char sk_b[CAS_SIGN_SECKEY_LEN], pk_b[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk_a, pk_a, seed_a), CAS_OK);
    ASSERT_INT_EQ(cas_sign_key_pair(sk_b, pk_b, seed_b), CAS_OK);

    unsigned char id[CAS_HASH_LEN];
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

    cas_sign_topic_id(id, pk_a);
    ASSERT_INT_EQ(cas_topic_ref_name(refname, sizeof(refname), id, NULL),
                  CAS_OK);

    unsigned char ra[CAS_VREC_LEN], rb[CAS_VREC_LEN];
    char aa[CAS_HASH_HEX + 1], ab[CAS_HASH_HEX + 1];

    mkrec(ra, aa, sk_a, pk_a, 1, NULL, 0);
    mkrec(rb, ab, sk_b, pk_b, 2, aa, 0);

    ASSERT_INT_EQ(cas_topic_publish(ct, refname, ra, "a"), CAS_OK);

    /* B's record is perfectly valid and newer.  It is still not this
     * topic, and the name binding is what says so.  Without it, holding
     * a signing key would be enough to redirect somebody else's ref. */
    check_refused(ct, refname, rb, CAS_SIGN_ETOPIC, aa);

    cas_tree_free(ct);
    cas_free(store);
}

static void
test_head_survives_reopen(void)
{
    if (!cas_sign_available())
        return;

    char depot[512];

    snprintf(depot, sizeof(depot), "%s/reopen", tmpdir);

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    unsigned char id[CAS_HASH_LEN];
    char refname[CAS_HASH_HEX + 2 + CAS_TOPIC_LABEL_MAX + 1];

    cas_sign_topic_id(id, pk);
    ASSERT_INT_EQ(cas_topic_ref_name(refname, sizeof(refname), id, NULL),
                  CAS_OK);

    unsigned char r1[CAS_VREC_LEN];
    char a1[CAS_HASH_HEX + 1];

    mkrec(r1, a1, sk, pk, 1, NULL, 0);

    {
        struct cas *store = cas_new(depot);

        ASSERT(store != NULL);
        if (!store)
            return;

        struct cas_tree *ct = cas_tree_new(store);

        ASSERT_INT_EQ(cas_topic_publish(ct, refname, r1, "first"),
                      CAS_OK);
        cas_tree_free(ct);
        cas_free(store);
    }

    /* The head is ordinary ref state, so it is on disk and the update
     * log came along with it.  That is the whole point of reusing refs
     * rather than inventing storage for a head pointer. */
    {
        struct cas *store = cas_new(depot);

        ASSERT(store != NULL);
        if (!store)
            return;

        struct cas_tree *ct = cas_tree_new(store);
        struct cas_vrec head;
        char addr[CAS_HASH_HEX + 1];

        ASSERT_INT_EQ(cas_topic_head(ct, refname, &head, addr), CAS_OK);
        ASSERT_INT_EQ((int)head.seq, 1);
        ASSERT_STR_EQ(addr, a1);

        cas_tree_free(ct);
        cas_free(store);
    }
}

int
main(void)
{
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    atexit(cleanup);

    fprintf(stderr, "--- cas-topic tests (backend: %s) ---\n",
            cas_sign_available() ? "monocypher" : "none");

    RUN(test_ref_name);
    RUN(test_publish_and_head);
    RUN(test_refusals_do_not_move_the_ref);
    RUN(test_another_publisher_cannot_take_a_topic);
    RUN(test_head_survives_reopen);

    TEST_REPORT();
}
