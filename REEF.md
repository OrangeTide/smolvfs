# REEF -- a transport-agnostic protocol for fetching content-addressed objects

Status: **proposed design. Not implemented.**

REEF moves immutable, content-addressed objects between a requester and a
provider. It is the content plane. Naming, signing, and federation are
the concern of SHOAL (see [SHOAL.md](SHOAL.md)), which is specified
separately and layers on top.

A reef is accreted immutable structure that only grows, which is what a
content-addressed depot is. A shoal is the mobile group that lives around
it.

PUBLIC DOMAIN (CC0-1.0)

## Relationship to the other documents

- [FORMAT.md](FORMAT.md) specifies object bytes. REEF moves those bytes
  and does not reinterpret them.
- [DOWNLOAD.md](DOWNLOAD.md) is the HTTP static-hosting profile of this
  protocol: the same walk, expressed as plain `GET` against an origin
  with no server-side logic. It remains valid and is the baseline any
  other transport is measured against.
- [SHOAL.md](SHOAL.md) supplies signed topic heads, the data domains REEF
  enforces, and the resolver that decides which provider to ask.

## Goals

- Fetch objects and byte ranges by address, verified against that
  address, over any transport that can carry bounded messages.
- Keep the provider free of protocol state that scales with content:
  a static origin adapter must be able to implement the whole thing.
- Let an implementer supply their own encoding without changing the
  protocol's meaning.
- Enforce the data domains of SHOAL D11 through D13, so a restricted
  domain cannot be enumerated.
- Be testable without a network (see [FRAMEWORK.md](FRAMEWORK.md)).

## Non-goals

- Naming, signatures, and version chains. Those are SHOAL.
- Object mutation and upload. A provider serves what it holds. Content
  arrives in a depot by local means or by SHOAL's authoritative push,
  never by an unsolicited REEF write.
- Reliability, congestion control, encryption, and peer authentication.
  All belong to the transport, which surfaces only its conclusions.
- Live game state. REEF carries immutable content: assets, checkpoints,
  and slowly-changing authoritative records. Per-tick entity state
  belongs on the game's own netcode.

## The transport contract

REEF is defined over **messages**, not streams or datagrams. A transport
is usable if it provides all of the following.

1. **Whole messages.** A message is delivered intact or not at all. The
   transport never hands up a fragment.
2. **A known maximum message size.** Both ends learn `msg_max` during the
   handshake. REEF never emits a message larger than the peer's stated
   limit.
3. **Per-request ordering.** Messages carrying the same request id arrive
   in the order they were sent. No ordering is required *between*
   different request ids, which is what allows a datagram transport to
   run requests concurrently without a global sequence.
4. **Reliability within a session.** A message accepted for sending is
   delivered or the session fails. REEF has no retransmission of its own.
5. **A peer identity conclusion.** The transport reports whether the peer
   is anonymous, an authenticated client, or a federation member. REEF
   turns that into a domain and never performs authentication itself.

Nothing else is assumed. In particular the transport may reorder across
requests, may multiplex REEF with unrelated traffic, and may close the
session at any point.

Three transports are expected to matter:

- **HTTP or HTTPS.** Request and response map onto `GET`, ranges onto
  `Range`. This is the DOWNLOAD.md profile, and it can serve a depot as
  static files with no application logic.
- **A reliable datagram library.** netchan supplies reliable ordered
  channels over UDP, so a REEF session is one channel and `msg_max`
  follows from the channel's payload limit.
- **A plain TCP stream.** A length prefix in front of each message
  satisfies the contract, in the manner of SMTP or NNTP.

## Sessions, requests, and flow

A **session** is one requester talking to one provider. It opens with a
handshake, carries some number of requests, and closes. Session state is
small and bounded: negotiated parameters, the in-flight request table,
and, in a restricted domain, the disclosure set described below.

A **request** is identified by a `req` id chosen by the requester. An id
must not be reused while a request bearing it is outstanding. Requests
are independent, so a provider may answer them in any order, and a
requester may have up to `max_inflight` of them open at once.

Every request produces exactly one logical reply, which may arrive as
several messages when the payload exceeds `msg_max` (see Segmentation).

## Segmentation

An object is usually larger than one message. There are two ways to move
it, and a provider must implement the first.

**Ranged pull is the baseline.** The requester issues `GetRange` for
successive windows sized to fit `msg_max`, and each reply is one
`ObjectData`. This needs no provider state, resumes trivially, gives the
requester exact flow control, and is what an HTTP origin already does
with `Range`. A provider that implements nothing else is conformant.

**Multipart push is an optional capability.** When both ends set
`FEAT_MULTIPART`, a single `Get` may be answered by a run of
`ObjectData` messages with `more = 1` on every part but the last. This
removes a round trip per window, which matters on a high-latency link and
not at all on a local one. The requester bounds the provider's output by
declining the feature.

Segmentation is a transport concern and is unrelated to the content
chunking of SHOAL D9. A chunked object's manifest and its chunks are
ordinary objects here, each fetched by its own address.

## Domains and disclosure

The session's domain comes from the transport's identity conclusion, and
it selects which depot answers, per SHOAL D12. Resolution runs toward
more permissive domains and never back.

The public domain answers any address. Restricted domains must not,
because anyone holding a candidate plaintext can compute its address and
ask, which turns a guess into a confirmation and makes low-entropy
records enumerable (SHOAL D13).

REEF enforces this with a **disclosure set**. An address is fetchable in
a restricted domain only if it is either:

- a root the session is authorized for, obtained through `GetRef` for a
  ref name the peer may read; or
- an address that appeared inside the body of an object already delivered
  in this session.

This is cheap. The provider keeps a per-session set of addresses it has
handed out, seeded by authorized roots and extended as objects are sent.
It also matches how a requester actually works, since child addresses are
learned by reading their parent. Walking a tree therefore proceeds
normally, while asking for an unrelated address fails.

The set is bounded by the size of the graph the session is authorized to
read, and it is discarded when the session closes.

### Errors must not leak existence

In a restricted domain, "no such object" and "you may not have that
object" are the same answer: `Denied`. A provider that distinguishes them
reintroduces exactly the enumeration D13 forbids. The public domain has
nothing to protect and may return `NotFound`.

## Schema notation

The message schema is written in a small IDL. It describes field names,
types, numbers, and grouping. It does **not** describe a byte layout: any
encoding that preserves the semantics below is conformant, and an
implementer is expected to supply one.

The notation is deliberately identical to netchan's microser IDL so that
generator can produce a codec directly, but the definition here is
self-contained and does not depend on that project.

### Grammar

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

### Types

`uint8`, `int8`, `uint16`, `int16`, `uint32`, `int32`, `uint64`, `int64`
are integers of the stated width and signedness. `bytes` is an opaque
octet string. `string` is a `bytes` whose content is UTF-8 text.

### Semantics an encoding must preserve

- **Fields are numbered from 1 to 31** and identified by number, not
  position. An encoding may place them in any order.
- **Every field is optional.** A field absent from a message decodes to
  zero, or to an empty `bytes` or `string`.
- **An unknown field is skipped, not an error.** A reader that meets a
  field number it does not recognise must be able to step past it and
  continue. This is what lets a newer writer add a field without breaking
  an older reader, and it is the only forward-compatibility mechanism
  either protocol relies on.
- **A `bytes` or `string` field carries at most 65535 octets.**
- **A `dispatch` block assigns each message a tag** in 1 to 255, unique
  within the block. The tag identifies which message follows.
- **In a `case` union**, the discriminant field selects which variants are
  meaningful; variants belonging to other members decode to zero.

### Packed arrays

The IDL has no repeated field. Where REEF needs a list of fixed-width
elements it packs them into one `bytes` field as their concatenation,
with the element width fixed by the field's definition and the count
implied by the length. An address list is therefore `n * 32` octets, and
at most 2047 addresses fit in one field.

This convention is REEF's, not the notation's. A field using it says so.

## Messages

Addresses on the wire are the **32 raw octets** of the BLAKE2b-256
digest, not the 64-character hex form used in file names and APIs.

Object payloads are the **stored form**: the bytes as they sit in the
depot, data region followed by trailer, exactly as DOWNLOAD.md serves a
loose object. Compression and the re-encoded types therefore survive the
transfer untouched, and the requester verifies after decoding locally.

```
# REEF v1 message schema.

enum Status
    Ok          = 0
    NotFound    = 1     # public domain only; see Errors must not leak
    Denied      = 2     # not permitted, or absent in a restricted domain
    TooLarge    = 3     # request exceeds a negotiated limit
    Unsupported = 4     # feature not negotiated
    Malformed   = 5
    Internal    = 6
end

enum Domain
    Local   = 0         # never appears on the wire; a local depot is not served
    Server  = 1
    Client  = 2
    Public  = 3
end

# Feature bits, carried in the features field as a set.
#   0x01  FEAT_RANGE       GetRange is supported (required in v1)
#   0x02  FEAT_MULTIPART   a Get may be answered by several ObjectData
#   0x04  FEAT_HAVE        Have is supported
#   remaining bits reserved; an unrecognised bit is ignored

message Hello
    uint16 version      = 1     # protocol version, 1
    uint32 features     = 2     # what the requester can accept
    uint32 msg_max      = 3     # largest message the requester will receive
    uint16 max_inflight = 4     # requests the requester will keep open
end

message HelloAck
    uint16 version      = 1
    uint32 features     = 2     # the intersection both ends will use
    uint32 msg_max      = 3     # largest message the provider will receive
    uint16 max_inflight = 4     # ceiling the provider imposes
    uint8  domain       = 5     # Domain granted to this session
    uint8  status       = 6
end

message GetRef
    uint32 req    = 1
    string name   = 2           # ref name; seeds the disclosure set
end

message RefValue
    uint32 req    = 1
    uint8  status = 2
    bytes  root   = 3           # 32 octets, empty unless status is Ok
end

message Have
    uint32 req    = 1
    bytes  addrs  = 2           # packed array of 32-octet addresses
end

message HaveReply
    uint32 req     = 1
    uint8  status  = 2
    bytes  present = 3          # bitmap, one bit per queried address,
                                # least significant bit first
end

message Get
    uint32 req    = 1
    bytes  addr   = 2           # 32 octets
end

message GetRange
    uint32 req    = 1
    bytes  addr   = 2
    uint32 offset = 3           # octets into the stored form
    uint32 length = 4           # octets requested
end

message ObjectData
    uint32 req     = 1
    uint8  status  = 2
    uint32 offset  = 3          # where this payload sits in the stored form
    uint32 total   = 4          # total stored length of the object
    uint8  more    = 5          # 1 if further parts follow for this req
    bytes  payload = 6
end

message Fault
    uint32 req    = 1           # 0 when the fault is not request-scoped
    uint8  status = 2
    string detail = 3           # human-readable, never parsed
end

dispatch Reef
     1 Hello
     2 HelloAck
     3 GetRef
     4 RefValue
     5 Have
     6 HaveReply
     7 Get
     8 GetRange
     9 ObjectData
    10 Fault
end
```

## Conversation rules

- The requester sends `Hello` first and waits for `HelloAck`. A provider
  that answers anything else has failed the session.
- `features` in `HelloAck` is the set both ends will use, and it must be
  a subset of what `Hello` offered. Asking for an unnegotiated feature
  earns `Unsupported`.
- `msg_max` is directional. Each side states what it can receive, and
  neither may exceed the other's figure.
- A provider must not have more than `max_inflight` requests open from
  one requester; the excess earns `TooLarge`.
- A reply carries the `req` of its request. A `Fault` with `req = 0` is a
  session-level failure and the session ends.
- A requester verifies every object against the address it asked for
  before storing it. A provider that returns wrong bytes is detected here
  and nowhere else, which is the property the whole design rests on.

## Worked exchange

Materialising a snapshot named `world` over a datagram transport, with
`msg_max` of 1024 and multipart declined:

```
->  Hello       version=1 features=RANGE|HAVE msg_max=1024 max_inflight=8
<-  HelloAck    version=1 features=RANGE|HAVE msg_max=1024 max_inflight=8
                domain=Client status=Ok

->  GetRef      req=1 name="world"
<-  RefValue    req=1 status=Ok root=8eb26db6...

# the root is now in the disclosure set

->  GetRange    req=2 addr=8eb26db6... offset=0 length=1000
<-  ObjectData  req=2 status=Ok offset=0 total=412 more=0 payload=...

# that tree names three children, all now disclosed; ask which are needed
# after testing locally with cas_exists

->  Have        req=3 addrs=<2 packed addresses>
<-  HaveReply   req=3 status=Ok present=0b11

->  GetRange    req=4 addr=<child a> offset=0 length=1000
->  GetRange    req=5 addr=<child b> offset=0 length=1000
<-  ObjectData  req=5 status=Ok offset=0 total=1000 more=0 payload=...
<-  ObjectData  req=4 status=Ok offset=0 total=8300 more=0 payload=...
->  GetRange    req=6 addr=<child a> offset=1000 length=1000
...
```

Replies to different request ids arrive out of order, which the contract
permits. Replies within one id do not.

## Deferred

- **Pack transport.** DOWNLOAD.md fetches a pack index and then reads
  object extents by range, which collapses many HTTP requests into few.
  A multiplexing message transport gains much less from this, and SHOAL
  D12 requires a pack never span domains. A feature bit is reserved and
  the mechanism is left unspecified in v1.
- **Provider-initiated content.** A provider populating its own depot is
  a local operation and needs no protocol. Authoritative update across
  servers is SHOAL's problem, not REEF's.
- **Compression negotiation.** Objects already carry their own codec tag,
  so transport compression would mostly recompress compressed bytes.

## Open questions

- **Disclosure set cost.** Bounded by the authorized graph, but a session
  walking a very large tree accumulates a large set. Whether to cap it,
  and what to do when the cap is hit, is undecided.
- **`Have` in restricted domains.** A batch existence query is useful for
  skipping content the requester already holds, but it answers questions
  about addresses. Restricting it to the disclosure set keeps it safe and
  also makes it much less useful, since disclosed addresses are the ones
  the requester already knows about.
- **Ranges past `uint32`.** Offsets and lengths are 32-bit, capping a
  single object at 4 GiB of stored form. Chunking makes this unreachable
  in practice, but the field widths are a v1 commitment.
- **Session resumption.** Reconnecting currently discards the disclosure
  set and starts from a root again. Whether that is worth optimising
  depends on how often sessions drop mid-walk.
