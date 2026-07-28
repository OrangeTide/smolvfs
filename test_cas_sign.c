/* test_cas_sign.c : unit tests for signed version records */
/* Copyright (c) 2026 Jon Mayo <jon@rm-f.net>
 * Licensed under BSD-2-Clause-Patent OR MIT */

#define _POSIX_C_SOURCE 200809L

#include "cas-sign.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* A fixed seed, so a failure reproduces. */
static const unsigned char seed_a[CAS_SIGN_SEED_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

static const unsigned char seed_b[CAS_SIGN_SEED_LEN] = {
    0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
    0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
    0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
    0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0,
};

static const char root_a[] =
    "8eb26db6f2c6a4b1c0e5d4a3928176f5e4d3c2b1a09876543210fedcba987654";
static const char root_b[] =
    "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef";

static void
fill(struct cas_vrec *v, const unsigned char pk[CAS_SIGN_PUBKEY_LEN],
     uint64_t seq, const char *root, const char *prev)
{
    memset(v, 0, sizeof(*v));
    memcpy(v->pubkey, pk, CAS_SIGN_PUBKEY_LEN);
    v->seq = seq;
    v->timestamp = 1700000000;
    snprintf(v->root, sizeof(v->root), "%s", root);
    if (prev)
        snprintf(v->prev, sizeof(v->prev), "%s", prev);
}

/****************************************************************
 * Backend absent
 ****************************************************************/

static void
test_no_backend_is_not_success(void)
{
    if (cas_sign_available())
        return;

    /* The property that matters when nothing is compiled in: every
     * path reports that it could not verify, and none of them returns
     * CAS_OK.  A build without a signer must not look like one that
     * checked and approved. */
    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a),
                  CAS_SIGN_ENOBACKEND);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    memset(pk, 0x11, sizeof(pk));
    fill(&v, pk, 1, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_SIGN_ENOBACKEND);

    /* decode parses far enough to reject a malformed record, but a
     * well-formed one still cannot come back OK */
    struct cas_vrec got;

    ASSERT_INT_EQ(cas_vrec_decode(rec, sizeof(rec), &got),
                  CAS_SIGN_ENOBACKEND);
}

/****************************************************************
 * Round trip and tampering
 ****************************************************************/

static void
test_round_trip(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 7, root_a, root_b);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_OK);

    struct cas_vrec got;

    ASSERT_INT_EQ(cas_vrec_decode(rec, sizeof(rec), &got), CAS_OK);
    ASSERT_INT_EQ((int)got.seq, 7);
    ASSERT_STR_EQ(got.root, root_a);
    ASSERT_STR_EQ(got.prev, root_b);
    ASSERT(memcmp(got.pubkey, pk, CAS_SIGN_PUBKEY_LEN) == 0);
    ASSERT(got.timestamp == 1700000000);

    /* the topic id is the bare hash of the key, not an object address */
    unsigned char want[CAS_HASH_LEN];

    cas_sign_topic_id(want, pk);
    ASSERT(memcmp(got.topic_id, want, CAS_HASH_LEN) == 0);
}

static void
test_first_record_has_no_prev(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 1, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_OK);

    struct cas_vrec got;

    ASSERT_INT_EQ(cas_vrec_decode(rec, sizeof(rec), &got), CAS_OK);
    ASSERT_STR_EQ(got.prev, "");
}

static void
test_tampering_is_caught(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 3, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_OK);

    struct cas_vrec got;

    /* Every signed field, one at a time.  Redirecting the root is the
     * attack the whole record exists to stop: it is what "whoever
     * controls the ref controls which tree the client materializes"
     * means in practice. */
    static const int signed_offsets[] = { 72, 80, 88, 120, };

    for (size_t i = 0; i < sizeof(signed_offsets) /
                           sizeof(signed_offsets[0]); i++) {
        unsigned char bad[CAS_VREC_LEN];

        memcpy(bad, rec, sizeof(bad));
        bad[signed_offsets[i]] ^= 0x01;
        ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                      CAS_SIGN_EBADSIG);
    }

    /* the signature itself */
    unsigned char bad[CAS_VREC_LEN];

    memcpy(bad, rec, sizeof(bad));
    bad[152] ^= 0x01;
    ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                  CAS_SIGN_EBADSIG);
}

static void
test_wrong_key_is_a_binding_failure(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk_a[CAS_SIGN_SECKEY_LEN], pk_a[CAS_SIGN_PUBKEY_LEN];
    unsigned char sk_b[CAS_SIGN_SECKEY_LEN], pk_b[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk_a, pk_a, seed_a), CAS_OK);
    ASSERT_INT_EQ(cas_sign_key_pair(sk_b, pk_b, seed_b), CAS_OK);
    ASSERT(memcmp(pk_a, pk_b, CAS_SIGN_PUBKEY_LEN) != 0);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk_a, 1, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk_a, rec), CAS_OK);

    struct cas_vrec got;

    /* Swap in B's key without touching the topic id.  The binding is
     * checked first, so this is EBADBIND rather than EBADSIG: the
     * record is not a broken signature, it is the wrong identity, and
     * a caller deciding whether to trust a publisher wants to know
     * which. */
    unsigned char bad[CAS_VREC_LEN];

    memcpy(bad, rec, sizeof(bad));
    memcpy(bad + 40, pk_b, CAS_SIGN_PUBKEY_LEN);
    ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                  CAS_SIGN_EBADBIND);

    /* And a record whose key and topic id agree but whose signature
     * came from another key is a signature failure. */
    memcpy(bad, rec, sizeof(bad));
    fill(&v, pk_b, 1, root_a, NULL);

    unsigned char rec_b[CAS_VREC_LEN];

    ASSERT_INT_EQ(cas_vrec_encode(&v, sk_b, rec_b), CAS_OK);
    memcpy(bad + 152, rec_b + 152, CAS_SIGN_SIG_LEN);
    ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                  CAS_SIGN_EBADSIG);
}

static void
test_malformed_records(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 1, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_OK);

    struct cas_vrec got;

    ASSERT_INT_EQ(cas_vrec_decode(rec, CAS_VREC_LEN - 1, &got),
                  CAS_SIGN_EBADFORM);
    ASSERT_INT_EQ(cas_vrec_decode(rec, CAS_VREC_LEN + 1, &got),
                  CAS_SIGN_EBADFORM);
    ASSERT_INT_EQ(cas_vrec_decode(NULL, CAS_VREC_LEN, &got),
                  CAS_SIGN_EBADFORM);

    unsigned char bad[CAS_VREC_LEN];

    memcpy(bad, rec, sizeof(bad));
    bad[0] = 'X';
    ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                  CAS_SIGN_EBADFORM);

    /* Reserved bytes must stay zero.  They are inside the signed
     * prefix, so a signer could set them and a reader that shrugged
     * would accept content the format has not defined. */
    memcpy(bad, rec, sizeof(bad));
    bad[4] = 1;
    ASSERT_INT_EQ(cas_vrec_decode(bad, sizeof(bad), &got),
                  CAS_SIGN_EBADFORM);
}

static void
test_encode_rejects_bad_fields(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 1, "not-a-hash", NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_ERR);

    fill(&v, pk, 1, root_a, "also-not-a-hash");
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_ERR);
}

static void
test_encoding_is_deterministic(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v1, v2;
    unsigned char r1[CAS_VREC_LEN], r2[CAS_VREC_LEN];

    fill(&v1, pk, 42, root_a, root_b);
    fill(&v2, pk, 42, root_a, root_b);
    ASSERT_INT_EQ(cas_vrec_encode(&v1, sk, r1), CAS_OK);
    ASSERT_INT_EQ(cas_vrec_encode(&v2, sk, r2), CAS_OK);

    /* Fixed layout and a deterministic signature scheme, so one record
     * has one encoding and therefore one address.  Two publishers of
     * the same generation converge instead of forking the store. */
    ASSERT(memcmp(r1, r2, CAS_VREC_LEN) == 0);
}

static void
test_record_is_an_ordinary_object(void)
{
    if (!cas_sign_available())
        return;

    unsigned char sk[CAS_SIGN_SECKEY_LEN], pk[CAS_SIGN_PUBKEY_LEN];

    ASSERT_INT_EQ(cas_sign_key_pair(sk, pk, seed_a), CAS_OK);

    struct cas_vrec v;
    unsigned char rec[CAS_VREC_LEN];

    fill(&v, pk, 1, root_a, NULL);
    ASSERT_INT_EQ(cas_vrec_encode(&v, sk, rec), CAS_OK);

    /* Self-addressed: class 1 raw under ATOLL A4.1, so it verifies by
     * hashing its own bytes and needs none of the re-encoded
     * machinery. */
    char h1[CAS_HASH_HEX + 1], h2[CAS_HASH_HEX + 1];

    ASSERT_INT_EQ(cas_hash_object(CAS_VREC_TYPE, rec, sizeof(rec), h1),
                  CAS_OK);
    ASSERT_INT_EQ(cas_hash_object(CAS_VREC_TYPE, rec, sizeof(rec), h2),
                  CAS_OK);
    ASSERT_STR_EQ(h1, h2);
    ASSERT_INT_EQ((int)strlen(h1), CAS_HASH_HEX);
    ASSERT(cas_valid_hash(h1));
}

int
main(void)
{
    fprintf(stderr, "--- cas-sign tests (backend: %s) ---\n",
            cas_sign_available() ? "monocypher" : "none");

    RUN(test_no_backend_is_not_success);
    RUN(test_round_trip);
    RUN(test_first_record_has_no_prev);
    RUN(test_tampering_is_caught);
    RUN(test_wrong_key_is_a_binding_failure);
    RUN(test_malformed_records);
    RUN(test_encode_rejects_bad_fields);
    RUN(test_encoding_is_deterministic);
    RUN(test_record_is_an_ordinary_object);

    TEST_REPORT();
}
