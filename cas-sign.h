/* cas-sign.h : signed version records for CAS refs */
/* PUBLIC DOMAIN (CC0-1.0) */

#ifndef CAS_SIGN_H
#define CAS_SIGN_H

#include "cas.h"

#include <stddef.h>
#include <stdint.h>

/****************************************************************
 * What this is for
 ****************************************************************
 *
 * A ref is a mutable pointer to a root address, and whoever controls
 * it controls which tree a client materializes.  Fetching one over
 * TLS moves that trust to the serving origin, which is the wrong
 * party: a mirror, a cache, or a community server should be able to
 * carry a ref without being able to change what it means.
 *
 * A version record moves the trust anchor to the publisher.  It is a
 * fixed-size, self-addressed CAS object that names a root, carries a
 * sequence number, links to its predecessor, and is signed by the key
 * the topic is named after.  Verifying one needs no registry and no
 * connection to anybody: the name binds to the key.
 *
 * The record says nothing about transport.  It verifies the same
 * whether it arrived over HTTP, from a peer, or off a USB stick.
 *
 * Chain and monotonicity checks are a layer above this one, which
 * concerns itself with a single record.
 */

/****************************************************************
 * Signature scheme
 ****************************************************************
 *
 * EdDSA over curve25519 with **BLAKE2b**, as implemented by
 * monocypher's crypto_eddsa_* family.
 *
 * This is deliberately *not* RFC 8032 Ed25519, which is the same
 * construction with SHA-512.  A signature here will not verify with a
 * library advertising "Ed25519", and one produced by such a library
 * will not verify here.  The choice keeps BLAKE2b as the only hash
 * primitive in the system, which smolvfs already depends on for every
 * address.  An implementation in another language wanting to
 * interoperate implements Ed25519 with BLAKE2b-512 substituted for
 * SHA-512 throughout; monocypher is the normative reference.
 *
 * The backend is selected at compile time, mirroring the codec table
 * in cas-codec.h.  Build with -DCAS_WITH_MONOCYPHER for the bundled
 * one.  An application bringing its own defines CAS_SIGN_USER when
 * building cas-sign.c:
 *
 *   #define CAS_SIGN_USER(X) X(my_sign, my_check)
 *
 * With no backend the module still parses records but cannot verify
 * them, and says so with CAS_SIGN_ENOBACKEND rather than reporting
 * success.  An absent verifier must never look like a passing one.
 */

#define CAS_SIGN_SEED_LEN    32
#define CAS_SIGN_PUBKEY_LEN  32
#define CAS_SIGN_SECKEY_LEN  64
#define CAS_SIGN_SIG_LEN     64

/** Errors specific to this module.  Numbered clear of the CAS_* codes
 *  in cas.h so a caller can pass either through one int. */
enum {
    CAS_SIGN_ENOBACKEND = -20,  /* no signer compiled in */
    CAS_SIGN_EBADFORM   = -21,  /* not a well-formed record */
    CAS_SIGN_EBADBIND   = -22,  /* topic id is not the hash of the key */
    CAS_SIGN_EBADSIG    = -23,  /* signature does not verify */
};

/** Human-readable text for a CAS_SIGN_* code. */
const char *
cas_sign_strerror(int err);

/** Nonzero if a signing backend is compiled in. */
int
cas_sign_available(void);

/** Derive a key pair from a 32-byte seed.
 *  The seed is the secret; keep it out of any depot that is served.
 *  Returns CAS_OK, or CAS_SIGN_ENOBACKEND.
 */
int
cas_sign_key_pair(unsigned char sk[CAS_SIGN_SECKEY_LEN],
                  unsigned char pk[CAS_SIGN_PUBKEY_LEN],
                  const unsigned char seed[CAS_SIGN_SEED_LEN]);

/** Topic id for a public key: BLAKE2b-256 of the 32 key bytes, with no
 *  object framing.  This is the binding that makes a topic name
 *  self-certifying, so it is spelled out rather than left to a caller.
 */
void
cas_sign_topic_id(unsigned char out[CAS_HASH_LEN],
                  const unsigned char pk[CAS_SIGN_PUBKEY_LEN]);

/****************************************************************
 * Version records
 ****************************************************************
 *
 * Fixed-width binary, so exactly one byte sequence encodes a given
 * record and there is no canonical-form question to get wrong.  Byte
 * layout is in FORMAT.md under "Version record".
 */

#define CAS_VREC_TYPE        "vrec"
#define CAS_VREC_LEN         216
#define CAS_VREC_SIGNED_LEN  152  /* the prefix the signature covers */

struct cas_vrec {
    unsigned char topic_id[CAS_HASH_LEN];
    unsigned char pubkey[CAS_SIGN_PUBKEY_LEN];
    uint64_t seq;
    int64_t timestamp;                  /* seconds; informational only */
    char root[CAS_HASH_HEX + 1];        /* the address published */
    char prev[CAS_HASH_HEX + 1];        /* predecessor, "" if first */
};

/** Fill in topic_id from pubkey, then serialize and sign.
 *
 *  The caller supplies seq, timestamp, root, and prev; pubkey must
 *  match sk.  Returns CAS_OK, CAS_SIGN_ENOBACKEND, or CAS_ERR if a
 *  field is malformed.
 */
int
cas_vrec_encode(struct cas_vrec *v,
                const unsigned char sk[CAS_SIGN_SECKEY_LEN],
                unsigned char out[CAS_VREC_LEN]);

/** Parse and fully verify a record.
 *
 *  Checks the length and magic, that topic_id is the hash of the
 *  embedded key, and that the signature covers the record.  There is
 *  no unverified parse: nothing wants one, and offering it would
 *  invite a caller to skip the part that matters.
 *
 *  Returns CAS_OK, or CAS_SIGN_EBADFORM / EBADBIND / EBADSIG /
 *  ENOBACKEND.
 */
int
cas_vrec_decode(const unsigned char *buf, size_t len,
                struct cas_vrec *out);

#endif /* CAS_SIGN_H */
