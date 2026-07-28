/* cas-sign.c : signed version records for CAS refs */
/* PUBLIC DOMAIN (CC0-1.0) */

#include "cas-sign.h"

#include <string.h>

/****************************************************************
 * Backend selection
 ****************************************************************
 *
 * Compile time, no runtime registry, mirroring cas-codec.c.  Exactly
 * one backend or none.
 */

#if defined(CAS_SIGN_USER)
/* Supplied by the application; the declarations come with it. */
#elif defined(CAS_WITH_MONOCYPHER)
int cas_sign_eddsa_sign(unsigned char sig[CAS_SIGN_SIG_LEN],
                        const unsigned char sk[CAS_SIGN_SECKEY_LEN],
                        const void *msg, size_t len);
int cas_sign_eddsa_check(const unsigned char sig[CAS_SIGN_SIG_LEN],
                         const unsigned char pk[CAS_SIGN_PUBKEY_LEN],
                         const void *msg, size_t len);
int cas_sign_eddsa_key_pair(unsigned char sk[CAS_SIGN_SECKEY_LEN],
                            unsigned char pk[CAS_SIGN_PUBKEY_LEN],
                            const unsigned char seed[CAS_SIGN_SEED_LEN]);
#define SIGN_BACKEND_SIGN     cas_sign_eddsa_sign
#define SIGN_BACKEND_CHECK    cas_sign_eddsa_check
#define SIGN_BACKEND_KEYPAIR  cas_sign_eddsa_key_pair
#endif

int
cas_sign_available(void)
{
#ifdef SIGN_BACKEND_SIGN
    return 1;
#else
    return 0;
#endif
}

const char *
cas_sign_strerror(int err)
{
    switch (err) {
    case CAS_OK:                 return "success";
    case CAS_SIGN_ENOBACKEND:    return "no signing backend compiled in";
    case CAS_SIGN_EBADFORM:      return "malformed version record";
    case CAS_SIGN_EBADBIND:      return "topic id does not match key";
    case CAS_SIGN_EBADSIG:       return "signature does not verify";
    default:                     return cas_strerror(err);
    }
}

int
cas_sign_key_pair(unsigned char sk[CAS_SIGN_SECKEY_LEN],
                  unsigned char pk[CAS_SIGN_PUBKEY_LEN],
                  const unsigned char seed[CAS_SIGN_SEED_LEN])
{
#ifdef SIGN_BACKEND_KEYPAIR
    return SIGN_BACKEND_KEYPAIR(sk, pk, seed);
#else
    (void)sk; (void)pk; (void)seed;
    return CAS_SIGN_ENOBACKEND;
#endif
}

void
cas_sign_topic_id(unsigned char out[CAS_HASH_LEN],
                  const unsigned char pk[CAS_SIGN_PUBKEY_LEN])
{
    /* The bare key bytes, with none of the "type len\0" framing an
     * object address carries.  A topic id names a key, not an object,
     * and keeping the two derivations distinct means a record can
     * never be mistaken for the identity it belongs to. */
    cas_digest(pk, CAS_SIGN_PUBKEY_LEN, out);
}

/****************************************************************
 * Record layout
 ****************************************************************/

#define VREC_MAGIC      "VRv1"
#define VREC_MAGIC_LEN  4

#define VREC_OFF_MAGIC     0
#define VREC_OFF_RESERVED  4
#define VREC_OFF_TOPIC     8
#define VREC_OFF_PUBKEY   40
#define VREC_OFF_SEQ      72
#define VREC_OFF_TIME     80
#define VREC_OFF_ROOT     88
#define VREC_OFF_PREV    120
#define VREC_OFF_SIG     152

static void
store_le64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (i * 8));
}

static uint64_t
load_le64(const unsigned char *p)
{
    uint64_t v = 0;

    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static const unsigned char vrec_zero[CAS_HASH_LEN];

int
cas_vrec_encode(struct cas_vrec *v,
                const unsigned char sk[CAS_SIGN_SECKEY_LEN],
                unsigned char out[CAS_VREC_LEN])
{
    if (!v || !sk || !out)
        return CAS_ERR;
    if (!cas_valid_hash(v->root))
        return CAS_ERR;
    if (v->prev[0] != '\0' && !cas_valid_hash(v->prev))
        return CAS_ERR;

    memset(out, 0, CAS_VREC_LEN);
    memcpy(out + VREC_OFF_MAGIC, VREC_MAGIC, VREC_MAGIC_LEN);

    cas_sign_topic_id(v->topic_id, v->pubkey);
    memcpy(out + VREC_OFF_TOPIC, v->topic_id, CAS_HASH_LEN);
    memcpy(out + VREC_OFF_PUBKEY, v->pubkey, CAS_SIGN_PUBKEY_LEN);

    store_le64(out + VREC_OFF_SEQ, v->seq);
    store_le64(out + VREC_OFF_TIME, (uint64_t)v->timestamp);

    if (cas_hex_decode(v->root, CAS_HASH_HEX,
                       out + VREC_OFF_ROOT, CAS_HASH_LEN) != 0)
        return CAS_ERR;

    /* An absent predecessor is 32 zero bytes, which is already there
     * from the memset.  A first record and one whose predecessor
     * happened to hash to zero are indistinguishable, which is fine:
     * BLAKE2b does not produce that. */
    if (v->prev[0] != '\0' &&
        cas_hex_decode(v->prev, CAS_HASH_HEX,
                       out + VREC_OFF_PREV, CAS_HASH_LEN) != 0)
        return CAS_ERR;

#ifdef SIGN_BACKEND_SIGN
    return SIGN_BACKEND_SIGN(out + VREC_OFF_SIG, sk, out,
                             CAS_VREC_SIGNED_LEN);
#else
    return CAS_SIGN_ENOBACKEND;
#endif
}

int
cas_vrec_decode(const unsigned char *buf, size_t len,
                struct cas_vrec *out)
{
    if (!buf || !out || len != CAS_VREC_LEN)
        return CAS_SIGN_EBADFORM;
    if (memcmp(buf + VREC_OFF_MAGIC, VREC_MAGIC, VREC_MAGIC_LEN) != 0)
        return CAS_SIGN_EBADFORM;

    /* The reserved word exists to keep the record's one variable point
     * of the future from being a new length.  It has to stay zero, or
     * a signer could smuggle bytes past a reader that ignored it. */
    for (int i = 0; i < 4; i++) {
        if (buf[VREC_OFF_RESERVED + i] != 0)
            return CAS_SIGN_EBADFORM;
    }

    memcpy(out->topic_id, buf + VREC_OFF_TOPIC, CAS_HASH_LEN);
    memcpy(out->pubkey, buf + VREC_OFF_PUBKEY, CAS_SIGN_PUBKEY_LEN);

    /* The binding is what makes the name self-certifying, so it is
     * checked before the signature: a record signed by the wrong key
     * for this topic is not a signature failure, it is the wrong
     * identity, and the two deserve different answers. */
    unsigned char expect[CAS_HASH_LEN];

    cas_sign_topic_id(expect, out->pubkey);
    if (memcmp(expect, out->topic_id, CAS_HASH_LEN) != 0)
        return CAS_SIGN_EBADBIND;

    out->seq = load_le64(buf + VREC_OFF_SEQ);
    out->timestamp = (int64_t)load_le64(buf + VREC_OFF_TIME);

    cas_hex_encode(buf + VREC_OFF_ROOT, CAS_HASH_LEN, out->root);

    if (memcmp(buf + VREC_OFF_PREV, vrec_zero, CAS_HASH_LEN) == 0)
        out->prev[0] = '\0';
    else
        cas_hex_encode(buf + VREC_OFF_PREV, CAS_HASH_LEN, out->prev);

#ifdef SIGN_BACKEND_CHECK
    if (SIGN_BACKEND_CHECK(buf + VREC_OFF_SIG, out->pubkey, buf,
                           CAS_VREC_SIGNED_LEN) != CAS_OK)
        return CAS_SIGN_EBADSIG;
    return CAS_OK;
#else
    /* Parsed, but nothing here can vouch for it.  Reporting success
     * would make a build without a backend look like a verifying one. */
    return CAS_SIGN_ENOBACKEND;
#endif
}
