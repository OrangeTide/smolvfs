#!/bin/sh
# test.sh : run all test binaries
set -e

fail=0

for t in test_cas test_cas_codec test_vfs test_cas_tree test_cas_pack test_cas_omap test_vfs_snap test_cas_sign test_cas_topic; do
    if ./"$t"; then
        :
    else
        fail=1
    fi
done

# The CLI needs its own driver: exit statuses, file permissions, and the
# messages a person reads are not reachable from the C tests.
if [ -x ./castool ] && [ -x ./test_castool.sh ]; then
    if ./test_castool.sh; then
        :
    else
        fail=1
    fi
else
    echo "skipping castool tests (build castool first)" >&2
fi

if [ "$fail" -ne 0 ]; then
    echo "SOME TESTS FAILED" >&2
    exit 1
fi

echo "all tests passed" >&2
