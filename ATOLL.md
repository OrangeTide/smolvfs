# ATOLL -- shared foundations for REEF and SHOAL

Status: **proposed design. Not implemented.**

An atoll is a reef enclosing a lagoon: one structure containing the
others. This document holds everything the two protocol specifications
share, so that neither restates it and the two cannot drift apart.

PUBLIC DOMAIN (CC0-1.0)

## Document map

```
  ATOLL.md      terminology, domains, verification, transport contract,
                schema notation, shared enums          <- you are here
    |
    +-- REEF.md      the content plane: fetch objects and ranges
    |     |
    |     +-- DOWNLOAD.md   REEF's HTTP static-hosting profile
    |
    +-- SHOAL.md     the naming and federation plane: signed topics
    |
    +-- FRAMEWORK.md the seams and the in-memory test harness

  FORMAT.md     object and container byte layouts (storage, not protocol)
  PRUNING.md    reference and history pruning
```

Both protocols are transport- and encoding-agnostic. Anything normative
about *how bytes travel* is here; anything about *what to say* is in the
plane specifications.

## A1. Terminology

- **Object** -- an immutable byte string stored under its address.
- **Address** -- `BLAKE2b-256("type len\0" || plaintext)`, the identity
  of an object and the check on its integrity.
- **Stored form** -- the octets as they sit in a depot: data region
  followed by trailer. Compression and re-encoding are properties of the
  stored form, not of the address.
- **Depot** -- one local store of objects and refs.
- **Ref** -- a mutable local name for a root address.
- **Topic** -- a mutable, signed, versioned name in SHOAL's namespace.
- **Requester** and **provider** -- the two roles in a session. A node is
  usually both.
- **Session** -- one requester talking to one provider, from handshake to
  close.
- **Domain** -- the disclosure class of content, per A3.

## A2. Addresses on the wire

An address is carried as the **32 raw octets** of the digest, never the
64-character hex form used in file names and APIs. Encoders must not
introduce a text representation.

## A3. Data domains

Content is classified by who may see it. Four classes exist:

- **local** -- private to this server. Caches, session state,
  credentials. Never served by either protocol.
- **server** -- shared with trusted servers in the federation.
- **client** -- shared with authenticated clients and with trusted
  servers.
- **public** -- shareable with anyone.

Because the client class is defined to include trusted servers, the four
form a chain rather than a lattice:

```
  local  <  server  <  client  <  public
```

A total order makes "more permissive" a single comparison rather than a
policy evaluation.

### A3.1. A domain attaches to a root, not to an object

An address is a function of the bytes alone, so **an object cannot carry
a domain**. Identical bytes produce one address no matter which class
wrote them. Classification therefore attaches to a ref or topic, and its
reachable closure inherits it.

### A3.2. One depot per domain

Enforcement is by separation, not by labelling objects inside one depot.
Which depot answered a request is a static, auditable fact, where
deriving a class from graph reachability is a per-request computation
that answers ambiguously when an object is reachable from two classes.
Separate depots also close the dedup side channel, in which a write
succeeding as a dedup hit tells the writer that someone else holds those
bytes.

**Resolution flows toward permissiveness and never back.** A client-domain
request may be answered from the client or public depot. A public request
is never answered from the server or local depot.

### A3.3. Never answer a bare address outside the public domain

Anyone holding a candidate plaintext can compute its address and ask
whether a provider holds it. For low-entropy content, such as a record
with a known schema and a guessable identifier, that turns a guess into a
confirmation and enumeration follows.

Restricted domains therefore do not expose lookup by bare address. REEF
implements this with a disclosure set; see REEF.md.

A corollary: outside the public domain, "no such object" and "you may not
have that object" must be the same answer. A provider that distinguishes
them reintroduces the enumeration this rule forbids.

### A3.4. Enforcement is not smolvfs's job

smolvfs stores bytes and has no notion of a requester. The boundary is
enforced by which depot a request resolves against and by the peer
authentication that selects it. A domain recorded in a file is
documentation, not a control.

## A4. Verification

Integrity comes from the address and never from the transport. A
requester accepts an object only after checking it, and a provider that
returns wrong bytes is detected here and nowhere else.

### A4.0. Validate on write, trust on read

Checking happens at the **trust boundary**, which is the moment an object
obtained from outside is admitted to a depot. Nothing external is stored
before it passes, and nothing that failed is stored at all.

Once an object is in the depot it is trusted when read. A reader does no
work to re-establish what admission established. This is what keeps
verification affordable: an object is checked once, on the way in, rather
than on every walk, lookup, and listing for the rest of its life.

The policy has three parts, and all three are needed:

1. **On write, from outside.** Full verification, by the rules of A4.1
   and A4.3. This is the only place a malicious actor is turned away, so
   it is the one place that may not be skipped or sampled.
2. **On read, from the depot.** Trusted. Checks that a read performs
   anyway, because it is already walking the bytes, are worth keeping as
   assertions against depot damage. Checks that would change a read's
   cost are not: they buy nothing a hostile party could have exploited,
   since the hostile party never got past step 1.
3. **Periodically, over the whole depot.** fsck re-runs the equivalent
   checks. This catches what happened to an object *after* admission,
   which is a different question from what it was when it arrived: bit
   rot, a truncated write, a filesystem-level tamper. Admission cannot
   detect these and a trusting read will not either.

The boundary has to be a real place in the code, not a convention. In
smolvfs it is `cas_tree_put_checked`, and the unchecked primitive beneath
it, `cas_put_object_at`, writes bytes at whatever address the caller
names and verifies nothing. A REEF requester commits through the checked
call. Anything reaching for the primitive is asserting it has already
verified, and is where to look first when something malformed turns up in
a depot.

### A4.1. Three encoding classes, and no fourth

Every stored encoding must be checkable against the address. Three
classes are admissible and the list is closed. The full byte-level rules
are in [FORMAT.md](FORMAT.md); what matters here is the obligation each
class places on a requester.

1. **Raw.** The stored bytes are the plaintext. Hash them.
2. **Transparent.** A total, deterministic decode recovers the
   plaintext and the stored form carries nothing else. Decode, then hash
   the plaintext. This is compression.
3. **Canonically re-encoded.** The stored form is a deterministic
   function of the plaintext and additionally carries a read path the
   plaintext does not constrain. Two checks are required: recover the
   plaintext and hash it against the address, **and** re-derive the
   stored form from that plaintext and compare it byte-for-byte. This is
   `htree`.

The line between 2 and 3 is the whole of it. A compressed object's only
read path is decoding, so recovering the plaintext is a complete check.
An `htree` carries a hash table that a lookup consults directly, and
recovering the correct entry set does not prove the table agrees with it.
Only pinning every byte does.

**An internal checksum is never an integrity check.** The `htree` adler32
and the packfile index checksum are corruption pre-filters, cheap to
evaluate and trivial to forge. Neither may stand in for verification
against the address, and no encoding may be admitted whose stored form
can only be checked that way.

`chunked` (SHOAL D9) is class 3 with respect to its chunk list, which is
checkable on arrival. The plaintext it names cannot be recovered without
the chunks, so that half of the check is **deferred** until they are
present. Deferral is permitted; absence of a check is not.

### A4.2. Partial transfers are never visible

An object cannot be verified until all of it is present, so a requester
must stage the incoming bytes somewhere that is not a valid address, and
commit only after the check passes.

Staging belongs **inside the destination depot**, not in a system
temporary directory. Being on the same filesystem by construction, the
commit is an atomic rename with no copy, on every platform, with no
hardlink trickery. A staging area also gives crash resumption for free
and can be swept by the same grace period the collector already applies.

Staging is not itself content-addressed. Storing each transport window as
an object would mint one object per message: a 100 MB transfer over a
2 KiB message limit is roughly 50,000 objects, which is a poor trade for
the loose-object layout. The right way to avoid large staging is to chunk
large content (SHOAL D9), because then each chunk is independently
addressed, independently verifiable, and small enough to hold in memory,
and a partially fetched file is simply a manifest with chunks missing.

### A4.3. The canonical form is enforced, not assumed

Class 3 verification works by re-deriving the stored form, so it is only
as strong as the canonical form being genuinely canonical. For directory
objects that means two rules, specified in [FORMAT.md](FORMAT.md) and
checked by a reader rather than trusted:

- **Names are well-formed UTF-8**, rejecting overlong sequences,
  surrogates, and anything past U+10FFFF. Overlong forms are the reason
  this is enforced at read time and not only at write time: `C0 AF` is
  not the octet `0x2F`, so it passes a test for `/` while decoding to
  U+002F in a consumer less strict than the producer. A requester that
  relaxes this hands its caller a path separator that its own checks
  never saw.
- **Entries are strictly ascending by name in unsigned octet order**,
  which for well-formed UTF-8 is also codepoint order. Strictness is what
  forbids duplicate names, and a duplicate is not cosmetic: two entries
  under one name let a listing and a lookup disagree about which child
  that name has, which is the divergence A4.1's byte pinning exists to
  catch, arriving through the entry set instead of the encoding.

A provider serving a directory object that breaks either rule is serving
a malformed object, and the requester rejects it rather than repairing
it. Per A4.0 this happens on admission, so a later walk of the same
object in the depot may assume both rules hold.

### A4.4. Normalization is a producer's problem

Unicode normalization is **not** performed. NFC requires tables smolvfs
will not carry, so names differing only by normalization are distinct
entries with distinct addresses even when they render identically.

This has a federation cost worth stating plainly. Two members storing the
same asset directory from platforms that disagree by default, macOS
decomposing where Linux does not, produce different addresses for that
directory and for every ancestor of it, and the two copies never dedup.
Nothing in either protocol detects this, because both copies are
internally consistent and correctly addressed.

The fix belongs in the content pipeline: normalize names before storing
them. A federation that shares assets should settle on one form and apply
it at publish time rather than discovering the divergence as a dedup
miss.

## A5. The transport contract

Both protocols are defined over **messages**. A transport is usable if it
provides all of the following.

1. **Whole messages.** A message is delivered intact or not at all.
2. **A known maximum message size.** Both ends learn it in the handshake,
   and neither exceeds the other's figure. See A6.
3. **Per-request ordering.** Messages carrying the same request id arrive
   in the order sent. No ordering is required between different ids,
   which is what lets a datagram transport run requests concurrently
   without a global sequence.
4. **Reliability within a session.** A message accepted for sending is
   delivered or the session fails. Neither protocol retransmits.
5. **A peer identity conclusion.** The transport reports whether the peer
   is anonymous, an authenticated client, or a federation member. The
   protocols turn that into a domain and never authenticate for
   themselves.

Nothing else is assumed. The transport may reorder across requests, may
multiplex protocol traffic with unrelated traffic, and may close the
session at any point. Encryption and congestion control are entirely its
business.

## A6. Message size

There is one negotiated limit, `msg_max`, stated separately by each side
for what it can *receive*. Two levels, one for streams and one for
datagrams, are unnecessary: the transport's own capability already
expresses the difference through the value it advertises.

**Every implementation must accept messages of at least 1024 octets.**
Below that figure interoperation is not attempted. Representative values:

| Transport | Practical `msg_max` |
|---|---|
| Raw UDP, no reassembly | about 1200 less headers |
| netchan | 2048 (`NC_MAX_MSG`, fragmented over a 1200-octet MTU) |
| TCP stream with a length prefix | 65536 or more |
| HTTP | effectively unbounded |

The netchan figure is the one to design against, because it is the
smallest ceiling a real transport imposes here and it is small. A
megabyte moved through 2 KiB messages is roughly 500 exchanges, which is
why content chunking and multipart replies both matter more than they
would over HTTP.

A requester cannot compute a message's encoded size, because the codec is
pluggable. **Therefore the sender of a payload is responsible for fitting
it**, and any length in a request is a maximum the responder may return
less than for any reason.

## A7. Schema notation

Messages are specified in a small IDL that describes field names, types,
numbers, and grouping. It does **not** describe a byte layout: any
encoding preserving the semantics in A7.3 is conformant, and an
implementer is expected to supply one.

The notation is deliberately identical to netchan's microser IDL, so that
generator can produce a codec directly. The definition here is
self-contained and does not depend on that project.

### A7.1. Grammar

A line beginning with `#` is a comment. Three block forms exist, each
closed by `end`:

```
enum NAME
    MEMBER = <integer>
    ...
end

message NAME
    <type> <field> = <number>
    ...
end

dispatch NAME
    <tag> MESSAGE
    ...
end
```

A `message` may contain a discriminated union:

```
message NAME
    case ENUM <field> = <number>
        MEMBER:
            <type> <field> = <number>
        ...
    end
end
```

### A7.2. Types

`uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `uint64`, `int64`
are integers of the stated width and signedness. `bytes` is an opaque
octet string. `string` is a `bytes` whose content is UTF-8 text.

### A7.3. Semantics an encoding must preserve

- **Fields are numbered 1 to 31** and identified by number, not position.
  An encoding may order them freely.
- **Every field is optional**, and an absent field decodes to zero, or to
  an empty `bytes` or `string`.
- **An unknown field is skipped, not an error.** A reader meeting a field
  number it does not recognise must step past it and continue. This is
  the only forward-compatibility mechanism either protocol relies on.
- **A `bytes` or `string` field carries at most 65535 octets.**
- **A `dispatch` block assigns each message a tag** in 1 to 255, unique
  within the block.
- **In a `case` union**, the discriminant selects which variants are
  meaningful; variants of other members decode to zero.

### A7.4. Zero is never a meaningful value

Because an absent field decodes to zero, **no enumeration may assign 0 to
a value with meaning**. Every enum reserves 0 as `Unset` and treats it as
a protocol violation on receipt.

Without this rule a dropped or mis-encoded status field would decode as
success, which is exactly the wrong direction for the field that decides
whether bytes are stored. The cost is one wasted enumerator per type.

### A7.5. Packed arrays

The notation has no repeated field. Where a list of fixed-width elements
is needed it is packed into one `bytes` field as their concatenation,
with the element width fixed by the field's definition and the count
implied by the length. An address list is `n * 32` octets, so at most
2047 addresses fit in one field.

This convention belongs to the protocols, not to the notation. A field
using it says so.

## A8. Shared enumerations

```
# Status accompanies every reply in both protocols.
enum Status
    Unset       = 0     # a protocol violation; see A7.4
    Ok          = 1
    NotFound    = 2     # public domain only; see A3.3
    Denied      = 3     # not permitted, or absent in a restricted domain
    TooLarge    = 4     # exceeds a negotiated limit
    Unsupported = 5     # feature not negotiated
    Cancelled   = 6     # request retired by either end
    Malformed   = 7
    Internal    = 8
end

# The domain granted to a session, per A3.  local never appears: a local
# depot is not served, so a session granted nothing receives Unset.
enum Domain
    Unset   = 0
    Server  = 1
    Client  = 2
    Public  = 3
end
```

A receiver treating `Unset` as anything but a failure has misimplemented
A7.4.

## A9. Cancellation

Either end may retire an outstanding request, and both protocols carry
the same mechanism.

- A requester sends `Cancel` naming the request id. The provider stops
  work and answers `Status.Cancelled` for that id.
- A provider may retire a request on its own by answering
  `Status.Cancelled`, whatever the reason.
- A retired id is free for reuse only after its `Cancelled` reply
  arrives. Reusing it earlier races with replies still in flight.

A provider **must** be permitted to retire requests, because per-request
state and the disclosure set otherwise grow without bound against a
requester that stops asking. The timeout at which it does so is policy,
recommended by an implementation rather than fixed here.

## Open questions

- **Domain of a chunk.** A manifest in a restricted domain may reference
  chunks identical to public ones. Deduplicating across that boundary
  reintroduces the confirmation attack of A3.3, so the safe default is
  not to, at the cost of storing popular chunks twice.
- **Promoting content between domains.** Publishing from client to public
  is a copy under A3.2. Whether that copy is explicit, whether it is
  reversible, and what becomes of content already disclosed under the
  wider class all need answers.
