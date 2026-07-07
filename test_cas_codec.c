/* test_cas_codec.c : unit tests for the CAS codec table */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "cas-codec.h"
#include "cas.h"
#include "test.h"

#include <string.h>

/****************************************************************
 * Tests
 ****************************************************************/

static void
test_none_intrinsic(void)
{
    const char *msg = "hello world";
    char buf[32];
    size_t outlen = sizeof(buf);

    ASSERT(cas_codec_supported(CAS_CODEC_NONE));
    ASSERT(cas_codec_can_encode(CAS_CODEC_NONE));

    ASSERT_INT_EQ(cas_codec_encode(CAS_CODEC_NONE, msg, 11, buf,
                                   &outlen), CAS_OK);
    ASSERT_INT_EQ((int)outlen, 11);

    char plain[32];

    ASSERT_INT_EQ(cas_codec_decode(CAS_CODEC_NONE, buf, 11, plain, 11),
                  CAS_OK);
    ASSERT(memcmp(plain, msg, 11) == 0);

    /* a length mismatch is a decode error for NONE */
    ASSERT_INT_EQ(cas_codec_decode(CAS_CODEC_NONE, buf, 11, plain, 10),
                  CAS_ERR);
}

/* a tag that is not in the compile-time table is unsupported */
static void
test_absent_codec(void)
{
    char buf[8];

    ASSERT(!cas_codec_supported(CAS_CODEC_ZSTD));
    ASSERT(!cas_codec_can_encode(CAS_CODEC_ZSTD));
    ASSERT_INT_EQ(cas_codec_decode(CAS_CODEC_ZSTD, "xx", 2, buf, 2),
                  CAS_ETYPE);
}

/* the compression policy classifies text vs binary, data-only */
static void
test_policy(void)
{
    const char *json = "{ \"key\": \"value\", \"n\": 12345 }";
    const char *utf8 = "caf\xc3\xa9 na\xc3\xafve \xe2\x9c\x93";  /* high bytes = text */

    /* half control bytes -> binary */
    unsigned char binary[64];
    for (int i = 0; i < 64; i++)
        binary[i] = (i & 1) ? 0x01 : 'A';

    /* NEVER: always raw */
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_NEVER, CAS_CODEC_DEFLATE,
                                   json, strlen(json)), CAS_CODEC_NONE);

    /* ALWAYS: always the codec, even for binary */
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_ALWAYS, CAS_CODEC_DEFLATE,
                                   binary, sizeof(binary)),
                  CAS_CODEC_DEFLATE);

    /* GUESS: text yes, binary no */
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_GUESS, CAS_CODEC_DEFLATE,
                                   json, strlen(json)), CAS_CODEC_DEFLATE);
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_GUESS, CAS_CODEC_DEFLATE,
                                   utf8, strlen(utf8)), CAS_CODEC_DEFLATE);
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_GUESS, CAS_CODEC_DEFLATE,
                                   binary, sizeof(binary)), CAS_CODEC_NONE);

    /* GUESS: empty is not worth compressing */
    ASSERT_INT_EQ(cas_codec_policy(CAS_COMPRESS_GUESS, CAS_CODEC_DEFLATE,
                                   "", 0), CAS_CODEC_NONE);
}

#ifdef CAS_WITH_MINIZ
static void
test_deflate_roundtrip(void)
{
    char plain[4096];

    for (size_t i = 0; i < sizeof(plain); i++)
        plain[i] = (char)('a' + (i % 4));

    ASSERT(cas_codec_supported(CAS_CODEC_DEFLATE));
    ASSERT(cas_codec_can_encode(CAS_CODEC_DEFLATE));

    unsigned char payload[8192];
    size_t plen = sizeof(payload);

    ASSERT_INT_EQ(cas_codec_encode(CAS_CODEC_DEFLATE, plain,
                                   sizeof(plain), payload, &plen),
                  CAS_OK);
    /* highly compressible input must shrink */
    ASSERT(plen < sizeof(plain));

    char out[4096];

    ASSERT_INT_EQ(cas_codec_decode(CAS_CODEC_DEFLATE, payload, plen,
                                   out, sizeof(out)), CAS_OK);
    ASSERT(memcmp(out, plain, sizeof(plain)) == 0);

    /* wrong expected length fails cleanly */
    ASSERT_INT_EQ(cas_codec_decode(CAS_CODEC_DEFLATE, payload, plen,
                                   out, sizeof(out) - 1), CAS_ERR);
}
#endif

int
main(void)
{
    RUN(test_none_intrinsic);
    RUN(test_absent_codec);
    RUN(test_policy);
#ifdef CAS_WITH_MINIZ
    RUN(test_deflate_roundtrip);
#endif
    TEST_REPORT();
}
