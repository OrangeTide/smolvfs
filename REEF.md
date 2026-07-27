# REEF -- the content plane

Status: **proposed design. Not implemented.**

REEF moves immutable, content-addressed objects between a requester and a
provider. Naming, signing, and federation are the concern of SHOAL
([SHOAL.md](SHOAL.md)), which layers on top.

**[ATOLL.md](ATOLL.md) holds everything this specification shares with
SHOAL**: terminology, addresses on the wire, data domains, the
verification rules, the transport contract, message sizing, the schema
notation, the shared enumerations, and cancellation. This document does
not restate them and cannot be read without them.

A reef is accreted immutable structure that only grows, which is what a
content-addressed depot is. A shoal is the mobile group that lives around
it.

PUBLIC DOMAIN (CC0-1.0)

## Goals

- Fetch objects and byte ranges by address, verified against that
  address, over any transport meeting the contract in ATOLL A5.
- Keep the provider free of state that scales with content, so a static
  origin can implement the public-domain subset.
- Enforce the domain rules of ATOLL A3, so a restricted domain cannot be
  enumerated.
- Be testable without a network ([FRAMEWORK.md](FRAMEWORK.md)).

## Non-goals

- Naming, signatures, and version chains. Those are SHOAL.
- Object mutation and upload. A provider serves what it holds. Content
  arrives in a depot by local means or by SHOAL's authoritative push,
  never by an unsolicited REEF write.
- Reliability, congestion control, encryption, and peer authentication,
  all of which belong to the transport.
- Live game state. REEF carries immutable content: assets, checkpoints,
  and slowly-changing authoritative records. Per-tick entity state
  belongs on the game's own netcode.

## Sessions and requests

A **session** is one requester talking to one provider. Its state is
bounded: negotiated parameters, the in-flight request table, and, in a
restricted domain, the disclosure set.

A **request** is identified by a `req` id chosen by the requester. An id
must not be reused while a request bearing it is outstanding, nor before
its `Cancelled` reply arrives if it was retired (ATOLL A9). Requests are
independent, so a provider may answer them in any order and a requester
may keep up to `max_inflight` open.

Every request produces one logical reply, which may arrive as several
messages when the payload exceeds `msg_max`.

## Capabilities

Two capabilities move an object larger than one message. **Neither is
mandatory**, and a session that negotiates neither is conformant but can
fetch only objects that fit in a single message.

- `FEAT_RANGE` -- the provider answers `GetRange`, returning the window
  the requester asked for. Needs no provider state, resumes trivially,
  and is what an HTTP origin already does with `Range`.
- `FEAT_MULTIPART` -- a single `Get` may be answered by a run of
  `ObjectData` messages, `more = 1` on every part but the last. This
  removes a round trip per window, which matters a great deal at
  netchan's 2 KiB ceiling (ATOLL A6) and not at all over HTTP.

`FEAT_HAVE` is independent and covers the batch existence query.

### A provider may insist

A provider is free to require capabilities of its requesters:

- It may refuse the session outright, answering `HelloAck` with
  `Status.Unsupported`, when the requester offers neither transfer
  capability and the provider serves nothing small enough to be useful.
- It may refuse an individual `Get` that would exceed `msg_max` when no
  transfer capability was negotiated, answering `Status.TooLarge`.

**A `TooLarge` reply must still carry `total`.** Without it the requester
learns only that it failed, when what it needs to know is how big the
object is, so it can decide whether to negotiate differently or ask
another provider. This is the one case where a failed reply carries
useful data.

## Segmentation and completion

An object is complete when the requester holds octets `0` through
`total` contiguously **and** the address verifies (ATOLL A4). The hash is
the real completeness check; contiguity is bookkeeping that decides when
to run it.

Rules that make this workable:

- A reply's `offset` equals the offset of the request that produced it,
  and its payload length is at most what was asked for. A provider may
  return less for any reason, and the requester must tolerate a short
  read rather than treating it as an error.
- Under `FEAT_MULTIPART`, the parts of one request are contiguous and in
  increasing offset order, beginning at the request's offset and ending
  with `more = 0`.
- `total` must not change between replies for one address within a
  session. A change is a protocol violation and fails the session.
- The requester **may** run concurrent requests for different windows of
  one object under distinct ids, in which case it owns the reassembly. It
  must not store anything until the contiguity and hash conditions above
  both hold.

The simple strategy, and the one the conformance suite exercises, is
sequential: issue the next `GetRange` at the offset where the last reply
ended, appending to a staging file as ATOLL A4.2 requires.

Segmentation is a transport concern, unrelated to the content chunking of
SHOAL D9. A chunked object's manifest and its chunks are ordinary objects
here, each fetched by its own address, and chunking is the preferred way
to avoid large staging entirely.

## Domains and the disclosure set

The session's domain comes from the transport's identity conclusion and
selects which depot answers, per ATOLL A3.2.

The public domain answers any address. Restricted domains must not
(ATOLL A3.3), and REEF enforces that with a **disclosure set**. An
address is fetchable in a restricted domain only if it is either:

- a root the session is authorized for, obtained through `GetRef`; or
- an address that appeared inside the body of an object already delivered
  in this session.

The provider keeps a per-session set seeded by authorized roots and
extended as objects are sent. This is cheap, and it matches how a
requester actually works, since child addresses are learned by reading
their parent. Walking a tree proceeds normally while asking for an
unrelated address fails. The set is bounded by the graph the session may
read and is discarded when the session closes.

### Which refs a session may read is SHOAL's business

`GetRef` seeds the disclosure set, so the mapping from peer identity to
readable ref names is the hinge the whole mechanism turns on. **REEF does
not define it.** It is SHOAL's, and until SHOAL specifies it an
implementation must default to refusing every name outside the public
domain rather than inventing a policy. A disclosure set seeded from an
unauthorized root defeats the entire mechanism.

### A static origin serves the public domain only

The disclosure set is per-session state and a static origin has no
sessions. It cannot distinguish requesters, so everything it can serve it
will serve to anyone. A static-origin provider is therefore **conformant
only in the public domain**. This is a stated limitation, not something
the adapter fakes.

An origin may still sit behind its own access control, as DOWNLOAD.md
describes. That is not REEF enforcing a domain; it is the origin
admitting or refusing a request before REEF is involved, with no
disclosure set and so no defence against enumeration by anyone admitted.
Safe for high-entropy content, unsafe for guessable records.

## Messages

Addresses are 32 raw octets (ATOLL A2). Object payloads are the stored
form (ATOLL A1), so compression and the re-encoded types survive transfer
untouched and the requester verifies per ATOLL A4.

```
# REEF v1 message schema.  Notation and shared enums: ATOLL A7, A8.

# Feature bits, carried in the features field as a set.  None is
# mandatory; see Capabilities.
#   0x01  FEAT_RANGE       GetRange is answered
#   0x02  FEAT_MULTIPART   a Get may be answered by several ObjectData
#   0x04  FEAT_HAVE        Have is answered
#   remaining bits reserved; an unrecognised bit is ignored

message Hello
    uint16 version      = 1     # protocol version, 1
    uint32 features     = 2     # what the requester can accept
    uint32 msg_max      = 3     # largest message the requester receives
    uint16 max_inflight = 4     # requests the requester will keep open
end

message HelloAck
    uint16 version      = 1
    uint32 features     = 2     # the intersection both ends will use
    uint32 msg_max      = 3     # largest message the provider receives
    uint16 max_inflight = 4     # ceiling the provider imposes
    uint32 req_timeout  = 5     # ms after which the provider may retire a
                                # request; 0 means it states no policy
    uint8  domain       = 6     # Domain granted to this session
    uint8  status       = 7
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
    uint64 offset = 3           # octets into the stored form
    uint32 length = 4           # a maximum; the provider may return less
end

message ObjectData
    uint32 req     = 1
    uint8  status  = 2
    uint64 offset  = 3          # where this payload sits in the stored form
    uint64 total   = 4          # total stored length; set even on TooLarge
    uint8  more    = 5          # 1 if further parts follow for this req
    bytes  payload = 6
end

message Cancel
    uint32 req    = 1           # request to retire; see ATOLL A9
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
    10 Cancel
    11 Fault
end
```

Offsets and lengths are 64-bit so the protocol does not impose a size
ceiling of its own, matching the storage seam in FRAMEWORK.md. `length`
stays 32-bit because no single reply approaches 4 GiB.

## Conversation rules

- The requester sends `Hello` first and waits for `HelloAck`. A provider
  answering anything else has failed the session.
- `features` in `HelloAck` is the set both ends will use and must be a
  subset of what `Hello` offered. Asking for an unnegotiated feature
  earns `Unsupported`.
- `msg_max` is directional. Each side states what it can receive and
  neither may exceed the other's figure. The floor is 1024 octets
  (ATOLL A6); a smaller figure fails the session.
- The sender of a payload is responsible for fitting it inside the
  receiver's `msg_max`, since a requester cannot compute encoded sizes
  through a pluggable codec.
- A provider must not hold more than `max_inflight` open requests from
  one requester; the excess earns `TooLarge`.
- A reply carries the `req` of its request. A `Fault` with `req = 0` is a
  session-level failure and ends the session.
- Every reply carries an explicit `status`. A decoded `Status.Unset` is a
  protocol violation (ATOLL A7.4), never a success.
- A requester verifies every object against the address it asked for
  **before committing it**, and stages partial transfers per ATOLL A4.2.
  Commit is the trust boundary of ATOLL A4.0: everything a peer sent is
  checked here, because nothing downstream will check it again.
- A directory object carries checks beyond its address: the canonical
  form rules of ATOLL A4.3, and for an `htree` the byte pinning of A4.1.
  A requester that commits a tree without them is trusting the provider
  for the child addresses it will later descend into, which is the one
  thing the design does not permit.
- A requester never commits an object it could not check. There is no
  quarantine state and no admitted-but-unverified object, because a later
  reader will not distinguish one.

### Have and undisclosed addresses

In a restricted domain a batch may name addresses outside the disclosure
set. The provider **reports those absent** rather than failing the
request. Absent is the truthful answer from the requester's point of
view, it leaks nothing, and it avoids an error path that would itself
distinguish disclosed addresses from undisclosed ones.

`Have` remains useful under disclosure scoping. The requester already
knows the addresses, having read them from a parent; what it does not
know is *which provider holds them*, which is precisely what a batch
query answers and what multi-provider fetch needs.

## Worked exchange

Materialising a snapshot named `world` over netchan, `msg_max` 2048,
multipart declined:

```
->  Hello       version=1 features=RANGE|HAVE msg_max=2048 max_inflight=8
<-  HelloAck    version=1 features=RANGE|HAVE msg_max=2048 max_inflight=8
                req_timeout=30000 domain=Client status=Ok

->  GetRef      req=1 name="world"
<-  RefValue    req=1 status=Ok root=8eb26db6...

# the root is now in the disclosure set

->  GetRange    req=2 addr=8eb26db6... offset=0 length=2048
<-  ObjectData  req=2 status=Ok offset=0 total=412 more=0 payload=<412>

# the provider returned less than asked: the whole object fits.  That
# tree names three children, all now disclosed.  Two are missing locally.

->  Have        req=3 addrs=<2 packed addresses>
<-  HaveReply   req=3 status=Ok present=0b11

->  GetRange    req=4 addr=<child a> offset=0 length=2048
->  GetRange    req=5 addr=<child b> offset=0 length=2048
<-  ObjectData  req=5 status=Ok offset=0 total=1400 more=0 payload=<1400>
<-  ObjectData  req=4 status=Ok offset=0 total=8300 more=0 payload=<1900>

# short read: the codec's overhead left room for 1900, not 2048.  The
# requester appends to staging and continues from where the reply ended.

->  GetRange    req=6 addr=<child a> offset=1900 length=2048
...

# the player disconnects; the walk is abandoned
->  Cancel      req=6
<-  ObjectData  req=6 status=Cancelled offset=1900 total=8300 more=0
```

Replies to different ids arrive out of order, which the contract permits.
Replies within one id do not.

## Deferred

- **Pack transport.** DOWNLOAD.md fetches a pack index then reads object
  extents by range, collapsing many HTTP requests into few. A
  multiplexing message transport gains much less, and ATOLL A3.2 requires
  a pack never span domains. A feature bit is reserved; the mechanism is
  unspecified in v1.
- **Provider-initiated content.** A provider populating its own depot is
  a local operation needing no protocol. Authoritative update across
  servers is SHOAL's problem.
- **Compression negotiation.** Objects carry their own codec tag, so
  transport compression would mostly recompress compressed bytes.

## Open questions

- **Disclosure set cost.** Bounded by the authorized graph, but a session
  walking a very large tree accumulates a large set. Whether to cap it,
  and what to do at the cap, is undecided.
- **Session resumption.** Reconnecting discards the disclosure set and
  restarts from a root. Whether that is worth optimising depends on how
  often sessions drop mid-walk.
- **Recommended `req_timeout`.** The field exists and the provider's
  right to retire is functional rather than advisory (ATOLL A9), but a
  sensible default is not yet chosen.
