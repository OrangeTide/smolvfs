#!/bin/sh
#
# get-smolvfs.sh -- vendor the smolvfs library from a tagged release.
#
# Copy this script into your own project (for example into scripts/) and run
# it to pull a tagged smolvfs source archive and replace your vendored copy
# with its contents. There is no git clone and no build step: it downloads the
# source archive GitHub publishes for a tag, unpacks it, and swaps it into your
# vendor directory. The only version you ever vendor is one that was tagged,
# and the release CI rejects a tag that does not match version.h.
#
# Usage:
#   get-smolvfs.sh <version> [dest-dir]
#
#   <version>    release to fetch, e.g. 0.1.0 (matches the smolvfs tag)
#   [dest-dir]   where smolvfs is vendored (default: third_party/smolvfs)
#
# Options:
#   -f, --force  overwrite dest even if it is not a prior smolvfs checkout
#   -h, --help   show this help
#
# Environment:
#   SMOLVFS_REPO     repo that publishes the tags (default:
#                    https://github.com/OrangeTide/smolvfs).
#   SMOLVFS_TAG      tag to fetch (default: v<version>). Set this if your tags
#                    use a different scheme.
#   SMOLVFS_ARCHIVE  full path or URL of the .tar.gz; overrides the above. May
#                    be a local file or a file:// URL for offline use.
#   SMOLVFS_SHA256   if set, the downloaded archive must match this SHA-256.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)
#

set -eu

: "${SMOLVFS_REPO:=https://github.com/OrangeTide/smolvfs}"

self=$(basename "$0")

usage() {
	sed -n '3,29p' "$0" | sed 's/^#\{1,2\} \{0,1\}//; s/^#$//'
	exit "${1:-0}"
}

die() {
	echo "$self: $*" >&2
	exit 1
}

force=0
version=""
dest=""
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help)  usage 0 ;;
	-f|--force) force=1 ;;
	--)         shift; break ;;
	-*)         die "unknown option: $1" ;;
	*)
		if [ -z "$version" ]; then version=$1
		elif [ -z "$dest" ]; then dest=$1
		else die "too many arguments"
		fi
		;;
	esac
	shift
done
[ -n "$version" ] || usage 1
dest=${dest:-third_party/smolvfs}

# Resolve where the archive comes from. Default is GitHub's auto-generated
# source tarball for the tag:
#   <repo>/archive/refs/tags/<tag>.tar.gz
if [ -n "${SMOLVFS_ARCHIVE:-}" ]; then
	src=$SMOLVFS_ARCHIVE
else
	repo=${SMOLVFS_REPO%.git}
	tag=${SMOLVFS_TAG:-v$version}
	src="$repo/archive/refs/tags/$tag.tar.gz"
fi

tmp=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT
arc=$tmp/src.tar.gz

# Fetch the archive: local path, file:// URL, or http(s)/ftp download.
case "$src" in
file://*)
	f=${src#file://}
	[ -f "$f" ] || die "no such file: $f"
	cp "$f" "$arc"
	;;
http://*|https://*|ftp://*)
	echo "$self: downloading $src"
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "$src" -o "$arc" || die "download failed: $src"
	elif command -v wget >/dev/null 2>&1; then
		wget -qO "$arc" "$src" || die "download failed: $src"
	else
		die "need curl or wget to download over the network"
	fi
	;;
*)
	[ -f "$src" ] || die "no such file: $src"
	cp "$src" "$arc"
	;;
esac

# Optional integrity check.
if [ -n "${SMOLVFS_SHA256:-}" ]; then
	if command -v sha256sum >/dev/null 2>&1; then
		got=$(sha256sum "$arc" | cut -d' ' -f1)
	elif command -v shasum >/dev/null 2>&1; then
		got=$(shasum -a 256 "$arc" | cut -d' ' -f1)
	else
		die "SMOLVFS_SHA256 set but no sha256sum/shasum available"
	fi
	[ "$got" = "$SMOLVFS_SHA256" ] || \
		die "sha256 mismatch: got $got, want $SMOLVFS_SHA256"
fi

# Unpack. GitHub wraps everything in a single smolvfs-<version>/ dir; strip
# it so dest holds the sources directly.
mkdir -p "$tmp/unpacked"
tar xzf "$arc" -C "$tmp/unpacked" || die "corrupt or unreadable archive: $src"
top=$(find "$tmp/unpacked" -mindepth 1 -maxdepth 1 -type d -name 'smolvfs-*' \
	| head -n 1)
[ -n "$top" ] || \
	top=$(find "$tmp/unpacked" -mindepth 1 -maxdepth 1 -type d | head -n 1)
[ -n "$top" ] || die "archive has no top-level directory"

# Guard against clobbering an unrelated destination.
if [ -e "$dest" ] && [ -n "$(ls -A "$dest" 2>/dev/null || true)" ]; then
	if [ "$force" -ne 1 ] && \
	    ! grep -q '^name: smolvfs' "$dest/UPSTREAM" 2>/dev/null; then
		die "'$dest' is not empty and is not a prior smolvfs checkout;
      pass --force to overwrite, or choose another destination"
	fi
fi

# Replace the vendored tree with the freshly unpacked archive.
mkdir -p "$dest"
find "$dest" -mindepth 1 -delete
(cd "$top" && tar cf - .) | (cd "$dest" && tar xf -)

# Record provenance.
sha=""
if command -v sha256sum >/dev/null 2>&1; then
	sha=$(sha256sum "$arc" | cut -d' ' -f1)
elif command -v shasum >/dev/null 2>&1; then
	sha=$(shasum -a 256 "$arc" | cut -d' ' -f1)
fi
{
	echo "name: smolvfs"
	echo "version: $version"
	echo "source: $src"
	[ -n "$sha" ] && echo "sha256: $sha"
	echo "synced_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$dest/UPSTREAM"

echo "$self: vendored smolvfs $version into $dest"
echo "  library sources: cas.c cas-codec.c cas-tree.c cas-pack.c cas-omap.c" \
     "vfs.c vfs-snap.c (with their headers and version.h)"
echo "  for DEFLATE, add cas-codec-miniz.c + third_party/miniz.c and build" \
     "with -DCAS_WITH_MINIZ -DMINIZ_NO_STDIO"
