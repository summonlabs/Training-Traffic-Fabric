# Testing

Tests are proof obligations. Each executable is one proof surface and one CTest entry; no test carries a
timeout, because a hang is a defect to diagnose rather than something to cut short.

| Executable | Surface | Representative obligations |
| --- | --- | --- |
| ``ttf_unit_tests`` | deterministic unit | labels and policy conservatism, registration and generations, step monotonicity, stale-generation codes, step N vs N+1 isolation, unknown-phase handling, preemption, contract limits, checkpoint isolation, policy and topology invalidation, recovery and rejoin, retention bounds, pacing, snapshot round trips, accounting closure |
| ``ttf_adversarial_tests`` | adversarial decoding | writer/reader bounds, every header field, truncation at every prefix, seeded frame mutation, canonical TLV rules, block round trips and mutation, state corruption and truncation, replay-window behaviour |
| ``ttf_persistence_tests`` | durability | commit/load round trip, sequence monotonicity, bit-flip and torn-tail refusal, incompatible version, damaged state making startup fail, restart epoch advancement, durable history, rejoin after restore |
| ``ttf_protocol_tests`` | protocol over real loopback TCP | handshake, malformed frames, unknown and incomplete operations, replay refusal, token binding, session capacity, 50-cycle churn, clean shutdown |
| ``ttf_property_tests`` | seeded randomised | 150 seeds x 150 operations with invariants after every operation; snapshot idempotence; step authority never surviving into the next step |
| ``ttf_concurrency_tests`` | concurrency and lifecycle | eight threads admitting and completing without over-commit, exactly one concurrent step-open winner, 20 start/stop cycles with a socket-runtime balance check, stop with idle sessions, six concurrent sessions keeping their own authority |
| ``ttf_scale_tests`` | scale and bounded cost | 2000 flows admitted with exact utilisation and zero arbitration scans, retention bounded with counted evictions, cost growth between N and 4N, 24 concurrent sessions |
| ``ttf_multiprocess_proof`` | real OS processes | trainer killed mid-step, replacement with a fresh boot identity, dead session and incarnation refused, fenced step balancing, coordinator restart with epoch advancement and durable history |

## Harness

``tests/support/test_harness.hpp`` is a small, dependency-free runner: ``TTF_TEST(suite, name)`` registers a
case, ``TTF_CHECK`` / ``TTF_CHECK_MSG`` / ``TTF_CHECK_EQ`` assert, ``TTF_REQUIRE_DECL`` and ``TTF_REQUIRE_OK`` unwrap
``ttf::Result`` values inside void tests, ``--filter`` selects cases and ``--list`` enumerates them. Comparisons
copy their operands rather than binding references, so a temporary inside an assertion cannot dangle.
Randomized tests record their seed with ``ttf::test::current_seed()`` so a failure prints a reproduction
command.

``tests/support/child_process.hpp`` spawns real processes with merged output and blocking, EOF-terminated
reads. It is what makes the multiprocess proof a real process proof: the trainer is terminated with
``TerminateProcess`` mid-step, not asked to leave politely.

## Defects found by this suite

The suite is written to break the runtime, and it did. Each of these was found by running it and fixed at the
root cause:

1. **Socket sentinel.** A moved-from socket kept the value 0, which is not ``INVALID_SOCKET`` on Windows, so
   closing it released a Winsock reference it never held and later sends failed with ``WSANOTOOLATE``-class
   errors. Fixed with an all-ones sentinel (``kNoSocket``).
2. **Client handshake send.** ``Client::Connect`` sent ``HELLO`` through the local socket *after* moving it into
   the client, so every connection failed with "send on a closed socket".
3. **Contract/group validation on the wire.** Decoders validated the job binding before the coordinator could
   supply it, so every register request was refused. Validation is now split into limits (wire) and binding
   (fabric).
4. **Accounting gap.** Deferred, rejected and throttled decisions were recorded on decisions and statistics
   but not folded into step accounting. All outcomes now flow through one closure point.
5. **Missing fence evidence.** A step fenced by an authority change recorded no fence event, so a report could
   not say why it was fenced. ``fence_open_step`` now records the reason, the observed and current
   incarnations, and the epoch.
6. **Evidence-free jobs could not be restored.** A job that had not yet published capacity evidence failed
   state validation on restore; absent evidence is now a legal state.
7. **Coordinator self-deadlock.** The session-capacity refusal path held the registry mutex and then called
   ``send_message``, which takes it again; the second session hung forever. The counter update is now scoped.
8. **Shutdown latency.** Interrupting a blocked read with a cross-thread ``shutdown`` left the session thread
   120 seconds in teardown on this platform. Sessions now observe the stop request themselves through a
   bounded readiness wait, and stop returns in milliseconds.
9. **Self-interrupted shutdown acknowledgement.** The ``SHUTDOWN`` operation set the stop flag before its own
   reply was written, letting the acceptor tear down the socket underneath it. The stop is now requested after
   the acknowledgement is on the wire.
10. **Sequence floor.** A diagnostic raw frame did not advance the session sequence floor, so the next
    ordinary request looked like a replay and fenced the connection.
11. **Client error flattening.** A refused session surfaced as ``UnexpectedMessage`` instead of the
    coordinator's code; ``ERROR`` frames are now decoded and returned.
12. **Harness dangling reference.** ``TTF_CHECK_EQ`` bound references to temporaries, which made assertions
    about values produced by ``Result::value()`` unreliable. Operands are now copied.
