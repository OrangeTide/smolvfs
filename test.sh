#!/bin/sh
# test.sh : run all test binaries
set -e

fail=0

for t in test_cas test_vfs test_cas_tree test_cas_pack test_cas_omap test_vfs_snap; do
    if ./"$t"; then
        :
    else
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "SOME TESTS FAILED" >&2
    exit 1
fi

echo "all tests passed" >&2
