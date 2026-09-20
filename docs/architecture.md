# Architecture

## Layers

```
apps/            ttf-coordinator, ttfctl, ttf-publisher      (process boundary)
src/             fabric_{api,core,traffic,recovery,state}    (deterministic core + snapshot codec)
                 coordinator, client, wire, codec, net, persistence, model, util
include/ttf/     the installed public surface
tests/           unit, adversarial, persistence, protocol, property, concurrency, scale, multiprocess
consumer/        an independent find_package(ttf) consumer
```

The core (``ttf::Fabric``) performs no I/O, spawns no threads and reads no clock. Given the same sequence of
calls it produces the same decisions, the same accounting and the same snapshot bytes. Everything that is not
deterministic lives above it:

| Concern | Owner |
| --- | --- |
| Authority, arbitration, accounting, explanations | ``ttf::Fabric`` |
| Durability ordering, epoch, sessions, transport | ``ttf::Coordinator`` |
| Framing, canonical message encoding | ``ttf::encode_frame`` / ``ttf::Message`` |
| Durable file format, atomic replacement | ``ttf::StateStore`` |
| Sockets, wake pairs, bounded waits | ``ttf::Socket``, ``ttf::WakePair``, ``ttf::WaitReadableWithin`` |

## State

One ``FabricState`` holds the policy, the logical clock, statistics and a map of ``JobState``. A job holds its
generation, incarnation, boot identity, epoch, groups, topology evidence, retired incarnations, the open
``StepState`` (phases, live flows, accounting, fences, an optional checkpoint burst), bounded step history,
bounded decisions and receipts, and monotonic id allocators.

Hot lookups are keyed (job by id, group by id, decision by intent, link evidence by group with a binary
search) and per-group utilisation is maintained incrementally, so admission never scans the flow table. The
one deliberate scan is preemption victim selection, and it is counted in ``FabricStats::arbitration_scans`` so
tests can prove that ordinary admits never scan.

## Locking

``Fabric`` uses exactly one mutex. Public methods lock once, call a ``detail::*`` function that assumes the
lock is held, and never call back out while holding it. Internal helpers never re-enter a locked path.
``Coordinator`` uses one mutex for the session registry, statistics and the commit sequence; it is never held
across a fabric call, a socket operation, a join, or a send.

Two defects found by testing are worth naming because they are the exact patterns this discipline prevents:

1. The session-capacity refusal path held the coordinator mutex and then called ``send_message``, which
   acquires the same mutex: a self-deadlock. The counter update is now a separate scope.
2. Idle sessions were once blocked in a raw ``recv`` that teardown tried to interrupt with a cross-thread
   ``shutdown``. Reads now wait for readability with a bounded budget so the session observes a stop request
   itself, and a session that is mid-request always finishes its reply first.

## Lifecycle

Coordinator start: open the state store, load and validate the snapshot, restore the fabric, advance the
epoch, rebase every job onto it, commit a baseline, create the wake pair, bind and listen, start the acceptor.
The acceptor waits on {listener, wake}, accepts sessions up to the configured ceiling, reaps finished
sessions, and on stop shuts down remaining sockets and joins every thread. A session thread reads frames,
enforces the replay window, dispatches, and writes the reply; the ``SHUTDOWN`` operation is honoured after its
own reply is written.

## Durability ordering

```
plan -> validate authority -> prepare -> perform effect -> verify -> durable commit -> publish result
```

The fabric performs the effect; the coordinator commits the resulting snapshot; only then is the response
sent. A commit failure marks the coordinator degraded: reads continue, mutations are refused with ``NotReady``,
and the condition is reported in ``CoordinatorStats`` and by ``ttfctl status``.
