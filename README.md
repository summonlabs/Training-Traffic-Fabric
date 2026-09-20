# Training Traffic Fabric 1.0.0

Open-source, vendor-neutral C++20 runtime for governing synchronization-heavy distributed-training network
traffic: step- and phase-aware priorities, bandwidth obligations, collective bursts, checkpoint interaction,
and generation-bound communication authority.

Training Traffic Fabric (TTF) is a library, a coordinator process, a client library and a command-line
surface. It answers one question, repeatedly and deterministically:

> For this distributed-training job and step, which network traffic may run now, at what class and rate,
> under the current phase, synchronization, topology, capacity, checkpoint and policy evidence?

## What this repository is not

TTF owns **training-specific network policy and authority**. It does not implement, and does not pretend to
implement, any of the following adjacent systems:

| Adjacent system | Why it is out of scope |
| --- | --- |
| Training framework / autograd engine | TTF never runs a forward or backward pass |
| Collective library (NCCL, RCCL, MPI, Gloo) | TTF decides whether and how fast; it does not move the bytes |
| Optimizer or parameter server | Optimizer state is traffic TTF classifies, never state it computes |
| Route planner / topology discovery | TTF consumes capacity evidence; it does not probe links |
| Generic bandwidth broker / qdisc | TTF is scoped to training phases, steps and barriers |
| Generic checkpoint store | TTF models the burst a checkpoint creates, not the bytes on disk |
| GPU / node scheduler | TTF never places work |

A real deployment keeps those systems and puts TTF between its workload and its network policy. The
coordinator never opens a data connection: it issues authority, and the workload acts on it.

## Core model

**Identities and generations.** Strongly typed, non-interchangeable:

- Identities: `TrainingJobId`, `TrainingStepId`, `PhaseId`, `ParallelismGroupId`, `TrafficIntentId`,
  `IncarnationId`, `SessionId`, `CheckpointBurstId`, and a 128-bit `BootId` minted per process start.
- Generations: `TrainingGeneration`, `WorkloadContractGeneration`, `TopologyGeneration`,
  `PolicyGeneration`, `EpochId`.

Distinct types make it a compile error to pass a step where a phase is expected, and a generation can never
be compared against a generation of another kind.

**Phase classes.** `FORWARD_COMM`, `BACKWARD_COMM`, `GRADIENT_SYNC`, `PARAMETER_SYNC`, `PIPELINE_TRANSFER`,
`DATA_INGEST`, `CHECKPOINT`, `RECOVERY`, `CONTROL`, and `UNKNOWN`. Parsing is canonical and case-insensitive
against exactly those spellings: no aliases, no prefix matching, no guessing. A framework label such as
`fwd_comm` is recorded as `UNKNOWN` with the original text kept verbatim as a hint.

**Authority tokens.** Every state-changing request carries a token binding job generation, incarnation, boot
identity, session, epoch, workload contract generation, topology generation, policy generation, step and
phase. A decision is meaningful only while all of those are still current.

**Decisions.** A decision records those bindings, the selected service class, the effective priority, the
granted rate envelope, the outcome (`ADMIT`, `DEFER`, `REJECT`, `REVALIDATE`, `THROTTLE`), a deterministic
`ErrorCode`, and explanation clauses naming the training semantics that produced it.

## Authority rules

These are enforced in code and covered by tests; each is listed with the code a caller sees.

| Rule | Observable outcome |
| --- | --- |
| Late traffic from a superseded step | `StaleStep` |
| Traffic from a closed job generation | `StaleJobGeneration` |
| Traffic from a retired or replaced process | `StaleIncarnation` / `IncarnationRetired` / `StaleBootIdentity` |
| Authority from a previous coordinator epoch | `StaleEpoch` |
| Workload contract changed underneath a request | `StaleContractGeneration` |
| Capacity evidence republished mid-step | `StaleTopologyGeneration`; live decisions invalidated |
| Token claiming a policy generation the fabric never issued | `StalePolicyGeneration` |
| Token provenance that disagrees with the session envelope | `AuthorityMismatch` (identity comes from the connection, never from the caller) |
| Traffic in an `UNKNOWN` phase | `BEST_EFFORT` only, capped by the policy ceiling, never isolated, never preempting |
| A caller claiming a known class for an `UNKNOWN` phase | `Conflict` (no promotion of unknown framework hints) |
| Unknown phase when the contract forbids it | `ConservativeUnknownPhase` |
| No usable capacity evidence | `MissingTopologyEvidence` (nothing is granted from `UNSUPPORTED` evidence) |
| Rate floor above the class ceiling | `RateAboveCeiling` (refused, not silently downgraded) |
| Capacity genuinely exhausted | `NoCapacity` as `DEFER` when the class is deferrable, otherwise `REJECT` |
| An already-closed flow reported again | `FlowAlreadyClosed` |
| A rejoin presenting the dead process boot identity | `Conflict` |

Service classes, their priorities, floors, ceilings and preemptibility are data, not code: a `PolicyDocument`
carries them, and `ApplyPolicy` mints a new generation.

### Checkpoint burst isolation

`BeginCheckpointBurst` gives a checkpoint window priority over a group without laundering its semantics:

- checkpoint traffic keeps service class `CHECKPOINT`; isolation changes its *effective priority*, never its
  class, and the decision carries `isolated = true` with a `CheckpointClassPreserved` explanation;
- live non-barrier-critical traffic in that group is displaced (`IsolationDeferred`) or throttled
  (`IsolationThrottled`) according to its class `isolation_ceiling_bps`;
- new non-barrier-critical traffic is deferred with `IsolationActive`;
- barrier-critical classes (control, barrier, recovery) are exempt.

Gradient-sync traffic therefore can never silently inherit, or be reported as, checkpoint priority, and
checkpoint traffic can never be reported as a synchronisation-critical class.

### Straggler-sensitive pacing

`EvaluatePacing` answers hold / release / throttle from the participant count, the declared grace window, the
step deadline and the remaining slack. No grace window means no hold; a hold that would run past a deadline
with zero slack becomes a release-at-deadline (`THROTTLE`) with a `SlackExhausted` explanation. TTF never
guesses which rank is slow: it reports straggler *counts* derived from what the workload declared.

### Recovery and rejoin

`AdmitReplacement` requires the new process own boot identity, refuses the dead one, mints a fresh
incarnation, retires the old one, fences every operation carrying it, closes the interrupted step with its
accounting balanced (commitments booked as cancelled, never as completions), and returns a resume floor: the
replacement may only open steps strictly later than any the retired incarnation observed.

## Persistence semantics

One file, atomically replaced: header (magic, format version, epoch, sequence, payload length, payload CRC,
header CRC), payload, end marker. The header is integrity-checked independently so a corrupt length can never
steer a read, and the file must end exactly where the header says.

Durable: registrations, generations, incarnations, topology evidence, policy, open-step structure, retained
step reports, fencing evidence. **Volatile by design**: in-flight flows, decisions and flow receipts. After a
coordinator restart they do not come back as live grants; they come back as *revalidation required*.

Restart behaviour: the epoch advances, every job is rebased onto it, and every token minted in the previous
epoch is refused (`AuthorityMismatch` at the envelope, `StaleEpoch` in the core). A commitment that was in
flight when the snapshot was taken is reconciled as cancelled, so a closed step report stays balanced.

An acknowledgement is never published before the durability point it claims: the coordinator performs the
mutation, commits the state, and only then sends the response. A commit failure marks the coordinator
degraded; it keeps serving reads and refuses further mutations until restarted, because it can no longer
promise that what it acknowledges is durable.

## Process, epoch and generation behaviour

- Every process mints its own `BootId`; nothing infers identity from a name, a rank or a PID.
- Session ids embed the coordinator epoch, so a session id from a previous epoch can never collide with, or
  be mistaken for, a current one.
- The coordinator acceptor waits on the listener and a wake pair and never polls; sessions are bounded,
  reaped when they end, and observe a stop request within a bounded window, so repeated start/stop and
  connect/disconnect cycles neither leak threads nor stall shutdown.
- Shutdown stops accepting, signals sessions, joins every thread, releases sockets and state, and leaves the
  accounting balanced. The `SHUTDOWN` operation stops the coordinator *after* its own acknowledgement is on
  the wire.

## Protocol and trust boundary

Fixed 48-byte framing (magic, version, type, flags, session, sequence, nonce, payload length, header CRC,
payload CRC). Canonical message encoding: a length-prefixed, strictly ordered, duplicate-free field set with
bounded text, bounded counts and no trailing bytes. All wire input is untrusted: lengths are checked before
they reach an allocation, and every rejection is a deterministic code (`BadMagic`, `BadVersion`,
`BadChecksum`, `Truncated`, `TrailingGarbage`, `TooManyFields`, `UnknownField`, `FieldOrderViolation`,
`ReplayDetected`, `SequenceViolation`, ...). Duplicate sequences and nonces inside a bounded window are
refused. The full message and field catalogue is in `docs/protocol.md`.

## Proof surfaces

Every exercised capability is labelled; nothing is claimed beyond what was run.

| Surface | Label | Evidence |
| --- | --- | --- |
| Deterministic core, codecs, state machine | REAL | `tests/unit`, `tests/adversarial` |
| Framed TCP transport, sessions, replay refusal, capacity | REAL | `tests/adversarial/protocol_tests.cpp` (loopback TCP to a real coordinator process) |
| Multiprocess kill / replacement / stale-frame fencing | REAL | `tests/multiprocess/multiprocess_proof.cpp` (separate OS processes, forced termination mid-step) |
| Coordinator restart, epoch advancement, durable history | REAL | `tests/adversarial/persistence_tests.cpp`, `tests/multiprocess` |
| Randomized state-machine invariants | REAL | `tests/stochastic/property_tests.cpp` (seeded, reproducible) |
| Concurrency, lifecycle churn, socket-runtime balance | REAL | `tests/stochastic/concurrency_tests.cpp` |
| Capacity evidence and topology | SYNTHETIC | Host-loopback capacity model (`SYNTHETIC:single-host loopback model`); every link record carries its label |
| Multi-node, RDMA, SmartNIC/DPU, NVLink, InfiniBand, RoCE, programmable switches | UNSUPPORTED | None of that hardware was used; no claim is made |
| AddressSanitizer build | REAL | `build/asan` with `-DTTF_ENABLE_ASAN=ON`: all eight suites pass under ASan, which also deploys the MSVC ASan runtime beside each executable |
| POSIX platform paths | UNSUPPORTED on this host | The POSIX socket and process paths are written for portability but were not executed here |

The runtime reports the label of the evidence behind every decision (`REAL`, `SYNTHETIC`, `UNSUPPORTED`), and
capacity from `UNSUPPORTED` evidence is never spent.

## Build, test, install

Requirements: CMake 3.25 or newer, a C++20 compiler (validated with MSVC 19.44 under `/W4 /WX`), and Threads.
There is no third-party runtime dependency.

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

Options: `TTF_BUILD_APPS`, `TTF_BUILD_TESTS`, `TTF_BUILD_EXAMPLES`, `TTF_WARNINGS_AS_ERRORS`,
`TTF_ENABLE_ASAN`.

Install and consume:

```
cmake --install build/release --prefix /path/to/prefix
cmake -S consumer -B build/consumer -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build/consumer
./build/consumer/ttf-consumer
```

`consumer/` is a deliberately independent project: it is not part of the runtime own build and finds the
package only through `find_package(ttf 1.0 REQUIRED CONFIG)`.

## Using it

Run a coordinator and drive it with the tools:

```
ttf-coordinator --port 0 --state ./state            # prints the bound port
ttfctl status --port <port>
ttfctl policy --port <port>
ttf-publisher --port <port> --scenario fresh        # a trainer-side step
ttfctl snapshot --file ./state/ttf-state.bin
```

`ttfctl simulate` runs the same deterministic core in-process and prints the decision record with every
explanation clause, which is the fastest way to see why a decision came out as it did; `ttfctl selftest` runs
in-process invariant checks.

## Public API sketch

```cpp
ttf::Fabric fabric(ttf::FabricConfig{});

// Registration mints the generations the caller must echo back.
ttf::Result<ttf::JobHandle> handle = fabric.RegisterJob(registration);
fabric.RegisterGroup(group_registration);
ttf::Result<ttf::TopologyGeneration> topology = fabric.PublishTopologyEvidence(authority, synthetic_evidence);
fabric.ApplyPolicy(ttf::make_default_policy(ttf::PolicyGeneration::from_raw(2)));

// Lifecycle.
fabric.BeginStep(step_open_request);
fabric.BeginPhase(phase_open_request);
fabric.EndPhase(authority, phase_id, ttf::PhaseDisposition::Completed);
fabric.EndStep(authority, ttf::StepDisposition::Completed);

// Traffic.
ttf::Result<ttf::TrafficDecision> decision = fabric.RequestTraffic(intent);
fabric.CompleteFlow(completion);
fabric.RevalidateFlow(authority, decision.value().intent);

// Checkpoint interaction, pacing, recovery, views.
fabric.BeginCheckpointBurst(burst_request);
fabric.EndCheckpointBurst(authority, burst_id);
fabric.EvaluatePacing(pacing_intent);
fabric.AdmitReplacement(recovery_request);
fabric.RetireIncarnation(authority);
fabric.LookupJob(job);
fabric.LookupDecision(job, intent);
fabric.LookupStepReport(job, step);
fabric.GroupUtilizationFor(job);
fabric.ListJobs();

// Durability.
ttf::Result<ttf::ByteBuffer> snapshot = fabric.Snapshot();
ttf::Result<std::unique_ptr<ttf::Fabric>> restored = ttf::Fabric::Restore(bytes, config);
```

Every fallible call returns `ttf::Result<T>` and every denial carries a deterministic `ttf::ErrorCode`; no
API throws across its boundary and no boolean stands in for a reason. `Fabric` is thread-safe: one mutex
guards its state, no callback is invoked while it is held, and internal helpers never re-enter a locked path.

The coordinator adds `ttf::Coordinator` (start/stop/port/epoch/stats) and `ttf::Client` (typed session
operations plus a documented raw-request surface for adversarial work).

## Limitations observed

- **Single coordinator.** Authority is centralized in one coordinator process. It is durable and restartable,
  but it is not a replicated quorum; there is no consensus layer and no leader election.
- **No data plane.** TTF issues authority; enforcement is the workload or fabric job. Nothing here shapes
  packets or programs a switch.
- **Synthetic capacity.** All capacity evidence in this repository is a host-loopback model. A real
  deployment must publish measured evidence; the runtime refuses to spend evidence labelled `UNSUPPORTED`.
- **Straggler detection is declarative.** TTF paces from participant counts and deadlines the workload
  states; it does not measure per-rank progress.
- **Windows-validated.** The runtime is portable C++20, but this release was built and executed only on
  Windows/MSVC. The POSIX socket and process paths are written but UNVALIDATED here.
- **Bounded by design.** Jobs, sessions, flows per step, retained decisions, retained history and payload
  sizes are all capped; a workload that exceeds a cap receives a deterministic refusal rather than unbounded
  growth.
- **Logical time.** Deadline and slack arithmetic uses the fabric own monotonic tick counter, not wall-clock
  time, so network jitter is not modelled.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
