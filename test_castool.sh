#!/bin/sh
# test_castool.sh : end-to-end tests for the castool CLI
#
# The C tests reach the library; they cannot reach argument parsing,
# exit statuses, file permissions, or the messages a person actually
# sees.  Those are where the topic verbs keep their security-relevant
# behaviour, so they get tested here instead of by hand.
#
# POSIX sh only: no bashisms, so this runs wherever the build does.
#
# PUBLIC DOMAIN (CC0-1.0)

set -u

CASTOOL=${CASTOOL:-./castool}
count=0
fail=0

TMP=$(mktemp -d "${TMPDIR:-/tmp}/test_castool.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

D="$TMP/depot"

# --- assertions -----------------------------------------------------

ok() {
	count=$((count + 1))
	if [ "$1" != 0 ]; then
		fail=$((fail + 1))
		printf '  FAIL %s\n' "$2" >&2
	fi
}

# assert_status <want> <label> -- <command...>
assert_status() {
	want=$1; label=$2; shift 3
	"$@" >"$TMP/out" 2>"$TMP/err"
	got=$?
	if [ "$got" = "$want" ]; then
		ok 0 "$label"
	else
		ok 1 "$label (exit $got, wanted $want)"
		sed 's/^/      /' "$TMP/err" >&2
	fi
}

# assert_match <pattern> <label> -- looks at the last command's output
assert_match() {
	if grep -q "$1" "$TMP/out" "$TMP/err" 2>/dev/null; then
		ok 0 "$2"
	else
		ok 1 "$2 (no match for '$1')"
		sed 's/^/      /' "$TMP/out" "$TMP/err" >&2
	fi
}

assert_no_match() {
	if grep -q "$1" "$TMP/out" "$TMP/err" 2>/dev/null; then
		ok 1 "$2 (unexpected match for '$1')"
	else
		ok 0 "$2"
	fi
}

group() { printf '  --- %s\n' "$1" >&2; }

# --- core verbs -----------------------------------------------------

group "core"

echo "hello" > "$TMP/a.txt"
echo "world" > "$TMP/b.txt"

assert_status 0 "import" -- \
	"$CASTOOL" -d "$D" import world "$TMP/a.txt" "$TMP/b.txt"

assert_status 0 "refs" -- "$CASTOOL" -d "$D" refs
assert_match "world" "refs lists the new ref"

assert_status 0 "ls" -- "$CASTOOL" -d "$D" ls world
assert_match "a.txt" "ls shows an imported file"

assert_status 0 "fsck clean" -- "$CASTOOL" -d "$D" fsck

# A hash that is not in the depot must fail rather than print nothing
# and succeed, which is the failure mode that hides a broken depot.
assert_status 1 "cat of a missing object fails" -- \
	"$CASTOOL" -d "$D" cat \
	1111111111111111111111111111111111111111111111111111111111111111

ROOT=$(cat "$D/refs/world.root")

# --- signing-dependent verbs ----------------------------------------

"$CASTOOL" -d "$D" keygen "$TMP/probe.key" >/dev/null 2>"$TMP/probe.err"
if grep -q "without a signing backend" "$TMP/probe.err"; then
	printf '  --- topics: skipped, no signing backend\n' >&2
	printf '%d assertions, %d failures\n' "$count" "$fail" >&2
	[ "$fail" = 0 ] || exit 1
	exit 0
fi

group "keys"

K="$TMP/id.key"

assert_status 0 "keygen" -- "$CASTOOL" -d "$D" keygen "$K"
assert_match "topic-id" "keygen prints the topic id"

# Mode is checked with find, since stat's flags differ between GNU and
# BSD and this has to run wherever the build does.
if [ -n "$(find "$K" -perm 600 2>/dev/null)" ]; then
	ok 0 "key file is 0600"
else
	ok 1 "key file is 0600 (it is not)"
fi

# Overwriting a key is not rotation: the topic id is the key, so it
# destroys every topic that key publishes.
assert_status 1 "keygen refuses to clobber" -- \
	"$CASTOOL" -d "$D" keygen "$K"
assert_match "already exists" "clobber says why"

assert_status 0 "keyid" -- "$CASTOOL" -d "$D" keyid "$K"
assert_match "public-key" "keyid prints the public key"

assert_status 0 "keyid with a label" -- "$CASTOOL" -d "$D" keyid "$K" demo
assert_match '\-demo' "labelled ref name is derived"

assert_status 0 "keyid reports an unusable label" -- \
	"$CASTOOL" -d "$D" keyid "$K" "a/b"
assert_match "not usable" "bad label is reported, not dropped"

chmod 644 "$K"
assert_status 1 "world-readable key refused" -- \
	"$CASTOOL" -d "$D" keyid "$K"
assert_match "readable by others" "permission refusal says why"
chmod 600 "$K"

echo "not a key" > "$TMP/junk.key"
chmod 600 "$TMP/junk.key"
assert_status 1 "junk key refused" -- \
	"$CASTOOL" -d "$D" keyid "$TMP/junk.key"
assert_match "not a key file" "junk refusal says why"

assert_status 1 "missing key refused" -- \
	"$CASTOOL" -d "$D" keyid "$TMP/absent.key"

group "publish"

# A dangling root verifies perfectly and resolves to nothing, and the
# subscriber is the one who finds out.
assert_status 1 "publish refuses a root not in the depot" -- \
	"$CASTOOL" -d "$D" publish "$K" \
	1111111111111111111111111111111111111111111111111111111111111111
assert_match "not in this depot" "dangling root says why"

assert_status 1 "publish refuses a non-hash" -- \
	"$CASTOOL" -d "$D" publish "$K" notahash

# "-lfoo" used to be read as "-l" and swallow the next argument.
assert_status 1 "publish rejects an unknown option" -- \
	"$CASTOOL" -d "$D" publish -lbogus "$K" "$ROOT"
assert_match "unknown option" "unknown option is named"

assert_status 0 "publish seq 1" -- \
	"$CASTOOL" -d "$D" publish -l demo "$K" "$ROOT"
assert_match "seq 1" "first publish is seq 1"

REF=$("$CASTOOL" -d "$D" keyid "$K" demo 2>/dev/null | \
      awk '/^ref/ { print $2 }')

assert_status 0 "topic" -- "$CASTOOL" -d "$D" topic "$REF"
assert_match "seq      1" "topic shows the head sequence"
assert_match "(none)" "first record has no predecessor"

# The ref holds the record address, never the root.  That is what makes
# every resolution pass through a signature check.
REFVAL=$(cat "$D/refs/$REF.root")
if [ "$REFVAL" = "$ROOT" ]; then
	ok 1 "ref holds the record address, not the root"
else
	ok 0 "ref holds the record address, not the root"
fi

echo "again" > "$TMP/c.txt"
"$CASTOOL" -d "$D" import world "$TMP/a.txt" "$TMP/c.txt" >/dev/null 2>&1
ROOT2=$(cat "$D/refs/world.root")

assert_status 0 "publish seq 2" -- \
	"$CASTOOL" -d "$D" publish -l demo "$K" "$ROOT2"
assert_match "seq 2" "second publish advances the sequence"

assert_status 0 "topic-log" -- "$CASTOOL" -d "$D" topic-log "$REF"
assert_match "^2 " "log starts at the head"
assert_match "^1 " "log reaches the first record"

# A second key with the same label does not contend for the first key's
# topic: the id leads the ref name, so it publishes its own.  Taking over
# somebody else's topic would need their key, which is the whole point of
# a self-certifying name.
K2="$TMP/other.key"

"$CASTOOL" -d "$D" keygen "$K2" >/dev/null 2>&1

REF2=$("$CASTOOL" -d "$D" keyid "$K2" demo 2>/dev/null | \
       awk '/^ref/ { print $2 }')
if [ "$REF2" = "$REF" ]; then
	ok 1 "a different key derives a different topic"
else
	ok 0 "a different key derives a different topic"
fi

HEAD_BEFORE=$(cat "$D/refs/$REF.root")
assert_status 0 "the second key publishes its own topic" -- \
	"$CASTOOL" -d "$D" publish -l demo "$K2" "$ROOT2"

if [ "$(cat "$D/refs/$REF.root")" = "$HEAD_BEFORE" ]; then
	ok 0 "the first topic is untouched by it"
else
	ok 1 "the first topic is untouched by it"
fi

assert_status 0 "fsck after publishing" -- "$CASTOOL" -d "$D" fsck

printf '%d assertions, %d failures\n' "$count" "$fail" >&2
[ "$fail" = 0 ] || exit 1
exit 0
