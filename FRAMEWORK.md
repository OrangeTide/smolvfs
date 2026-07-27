# Test framework for REEF and SHOAL

Status: **proposed design. Not implemented.**

An abstract harness that runs the protocols of [REEF.md](REEF.md) and,
later, [SHOAL.md](SHOAL.md) with no network and no wire format, so that
protocol logic can be tested on its own and the same tests can be reused
by anyone who vendors smolvfs and supplies their own encoding.

PUBLIC DOMAIN (CC0-1.0)

## The idea

netchan's protocol core never names a socket: datagrams arrive through a
feed call and leave through a send call, tagged with an address the core
copies but never interprets. That one seam is what lets the same object
file run over UDP, over a WebSocket, and through an encrypted tunnel.

This framework applies the same discipline twice, because REEF has two
things to abstract rather than one.

```
  protocol logic (walk, verify, disclosure set, session state)
  ------------------------------------------------------------
  codec seam        message struct  <->  octets
  ------------------------------------------------------------
  transport seam    octets (or structs) to and from a peer
  ------------------------------------------------------------
  simnet            |  netchan  |  HTTP  |  TCP  |  ...
```

Everything above the codec seam is what the conformance suite tests.
Everything below either seam is somebody else's problem, including ours.

## Two seams, not one

**The transport seam** carries a message to a named peer and delivers
messages back. It never names a socket, and it never inspects a payload.
It reports the peer identity conclusion the REEF contract requires, and
nothing more.

**The codec seam** turns a message struct into octets and back. Splitting
it out is what makes the protocol encoding-agnostic in fact rather than
in aspiration: an implementer supplies a codec and the protocol logic
above is untouched.

If only the transport were abstracted, every vendor would inherit our
encoding. If both are abstracted but the simulator always runs structs
through a codec, a vendor with no codec cannot test anything at all. So
the harness runs in either of two modes.

### Direct mode

Message structs pass from requester to provider without encoding. Fast,
and it isolates protocol logic: a failure here is a bug in the walk, the
session rules, or the disclosure set, never in a codec.

This is the mode a vendor uses when they only want the protocol, and the
mode to reach for when debugging protocol behaviour.

### Round-trip mode

Every message is encoded, the octets are handed to the transport, and the
far side decodes. The same tests run unchanged and now also exercise the
codec. This is what catches a field that is written but never read, a
length that overflows, and a decoder that fails to skip an unknown field.

A reference codec ships with the framework so round-trip mode works out
of the box and so conformance vectors have something to be expressed in.

## Interfaces

Sketched, not final. Each seam is a struct of function pointers plus an
opaque context, in the style the rest of smolvfs already uses.

```c
/* Transport seam.  Sends a message to a peer and delivers received
 * messages by callback.  Never interprets the payload. */
struct reef_transport {
    /* Queue one message for delivery to peer.  Returns 0 or an error. */
    int (*send)(void *ctx, reef_peer_t peer,
                const void *msg, size_t len);

    /* Largest message this transport will carry, in octets. */
    size_t (*msg_max)(void *ctx);

    /* What the transport concluded about a peer's identity, which REEF
     * turns into a domain.  It performs no authentication itself. */
    int (*peer_identity)(void *ctx, reef_peer_t peer);

    void *ctx;
};

/* Codec seam.  In direct mode both calls are the identity and len is
 * sizeof the struct. */
struct reef_codec {
    int (*encode)(void *ctx, const struct reef_msg *in,
                  void *buf, size_t buflen);
    int (*decode)(void *ctx, const void *buf, size_t len,
                  struct reef_msg *out);
    void *ctx;
};
```

The union of every message in the REEF dispatch block is one tagged
`struct reef_msg`, so the seams carry a single type and the protocol
switches on its tag.

## simnet

The in-memory network. It holds a set of nodes, a queue of messages in
flight, and a clock, and it is entirely deterministic.

**Determinism is the point.** Every scheduling and fault decision is
drawn from a seeded generator, and the clock advances only when the
harness says so. A failing test reproduces exactly from its seed, which
is the difference between a useful concurrency test and an intermittent
one nobody trusts.

Conditions it must model:

- **Latency and jitter**, per link, so requests genuinely complete out of
  order.
- **Loss and reordering**, for transports that permit it. REEF requires
  reliability within a session, so this exercises the adapter that
  provides it, not REEF itself.
- **Partition and heal**, including a partition that opens mid-walk.
- **Peer disconnect** with requests outstanding.
- **Bandwidth and stalling**, so a provider that goes quiet is
  distinguishable from one that is merely slow.

## Adversarial cases

A harness that only models a flaky network tests the least interesting
half. The properties worth the design are all about a provider behaving
badly, and each has a matching fault the simulator must be able to
inject.

| Fault | Property under test |
|---|---|
| Provider returns bytes that hash to something else | Verification rejects them and the object is never stored |
| Provider truncates a payload or lies about `total` | Reassembly rejects the object |
| Provider claims to hold content it lacks in `HaveReply` | Requester recovers by asking another provider |
| Requester asks for an address never disclosed | `Denied` in a restricted domain, on every path |
| Provider answers `NotFound` in a restricted domain | Conformance failure: existence leaked |
| Provider offers a feature not in `Hello` | Requester ignores it rather than following |
| Two providers, one honest and one corrupt | The walk completes from the honest one |

The disclosure-set rows matter most, because they are the only thing
standing between a restricted domain and enumeration, and they are easy
to get subtly wrong in a way no ordinary test would notice.

## Conformance suite

The tests are written once against the seams and run in a matrix:

```
  {direct, round-trip} x {simnet, netchan, HTTP, ...}
```

A vendor plugs in their transport, their codec, or both, and runs the
same suite. Passing it means their pairing implements REEF, not that it
resembles it.

**Prove the abstraction with two transports.** An in-memory simulator
alone will happily validate a seam that no real network can implement.
Porting `examples/cas-fetch.c` onto the transport seam gives a second,
genuinely different transport and is the cheapest way to find out whether
the abstraction survives contact with HTTP. That port is the acceptance
test for the seam design, not an optional extra.

## Conformance vectors

A pluggable codec means interoperability cannot be checked by comparing
octets alone, so vectors come in two layers:

- **Semantic vectors**: a message with its field numbers and values, and
  the reply a conformant provider must produce. These bind any codec.
- **Reference encoding vectors**: the same messages as octets under the
  reference codec. These bind anyone who chooses to use it, and give a
  new codec something to check itself against.

## Build and layout

Sketch, following the existing tree:

```
  reef.c / reef.h            protocol logic, both roles
  reef-codec-ref.c           the reference codec
  simnet.c / simnet.h        deterministic in-memory network
  test_reef.c                the conformance suite
```

`reef.c` links against `cas.h` and `cas-tree.h` for storage and against
neither seam's implementation. simnet and the reference codec are test
scaffolding a vendor may keep or discard, which is why they are separate
translation units rather than `#ifdef` blocks inside the protocol.

## Open questions

- **Where the disclosure set lives.** Provider-side session state in
  `reef.c` is the obvious home, but a static-origin adapter has no
  sessions and can only serve the public domain. Whether that is a stated
  limitation or something the adapter fakes is undecided.
- **How much of a peer the framework simulates.** A provider needs a real
  depot to serve from. Whether simnet nodes get temporary on-disk depots
  or an in-memory `struct cas` shim changes what the suite can cover.
- **Message struct shape.** One tagged union keeps the seams simple but
  makes the struct as large as its biggest member. Whether that matters
  depends on how many sessions a shard runs at once.
