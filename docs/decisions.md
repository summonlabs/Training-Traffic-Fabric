# Decisions and denial reasons

Every denial is a deterministic ``ttf::ErrorCode``. The tables below are the contract a caller can rely on.
"Where" names the layer that produces it: fabric (authority and arbitration), coordinator (sessions,
durability), codec (wire and file structure).

## Authority and fencing

| Code | Where | Meaning |
| --- | --- | --- |
| ``UnknownJob`` | fabric | no job with that id |
| ``StaleJobGeneration`` | fabric | token from a superseded job generation |
| ``StaleIncarnation`` | fabric | token from an incarnation that is not current |
| ``IncarnationRetired`` | fabric | incarnation explicitly retired (recovery, replacement, graceful exit) |
| ``StaleBootIdentity`` | fabric | token minted by a different process than the job owner |
| ``StaleEpoch`` | fabric | token from a previous coordinator epoch |
| ``StaleContractGeneration`` | fabric | workload contract changed |
| ``StaleTopologyGeneration`` | fabric | capacity evidence republished |
| ``StalePolicyGeneration`` | fabric | token claims a policy generation never issued |
| ``StaleStep`` | fabric | token names a step that is not the open step |
| ``StalePhase`` / ``PhaseNotActive`` | fabric | phase does not belong to the open step / is closed |
| ``StepNotActive`` | fabric | no step is open for the job |
| ``AuthorityMismatch`` | coordinator | token provenance disagrees with the session envelope |
| ``SessionFenced`` / ``ReplayDetected`` / ``SequenceViolation`` | coordinator | session-level refusals |

## Policy, capacity and phase semantics

| Code | Meaning |
| --- | --- |
| ``MissingTopologyEvidence`` | no link evidence for the group, or evidence labelled ``UNSUPPORTED`` |
| ``ConservativeUnknownPhase`` | the contract does not admit traffic in an ``UNKNOWN`` phase |
| ``Conflict`` | declared phase class contradicts the registered class; a policy generation that does not advance; a replacement presenting the dead boot identity |
| ``RateAboveCeiling`` | the stated minimum exceeds the class or UNKNOWN-phase ceiling |
| ``NoCapacity`` | no free capacity; ``DEFER`` when the class is deferrable and the caller allows it, otherwise ``REJECT`` |
| ``IsolationActive`` | a checkpoint burst holds the group |
| ``ContractLimitExceeded`` | per-step intent, byte or flow ceiling reached |
| ``ResourceExhausted`` | active-flow budget reached |
| ``FlowNotActive`` / ``FlowAlreadyClosed`` | completion for a flow that is gone or already closed |
| ``BurstAlreadyActive`` / ``BurstNotActive`` | checkpoint burst state mismatch |
| ``PacingHeld`` | a sync point is holding for stragglers (a ``DEFER`` with a hold-until tick) |

## Wire and file structure

``BadMagic``, ``BadVersion``, ``UnsupportedProtocolVersion``, ``BadLength``, ``BadChecksum``, ``BadEncoding``,
``MissingField``, ``DuplicateField``, ``UnknownField``, ``FieldOrderViolation``, ``TrailingGarbage``,
``TooManyFields``, ``Truncated``, ``InvalidUtf8``, ``TooLarge``, and for files ``CorruptState``,
``IncompatibleState``, ``PartialState``, ``StateTooLarge``.

## Decision outcomes

| Outcome | Meaning |
| --- | --- |
| ``ADMIT`` | traffic may run now under the granted envelope |
| ``DEFER`` | not now; re-request. No authority is held meanwhile |
| ``REJECT`` | never, under the current policy and evidence |
| ``REVALIDATE`` | a previous decision is no longer provably current |
| ``THROTTLE`` | admitted, but an isolation window or a deadline reduced the grant |

## The rules that decide

1. **Binding first.** Job generation, incarnation, boot identity, epoch, contract, topology and step are all
   checked before any capacity arithmetic. A stale token never consumes capacity.
2. **Class is derived, never requested.** The registered phase class and the policy generation select the
   service class; the caller's declared class is only a consistency assertion.
3. **Priority is a total order.** Classes carry unique priorities; ties break on synchronization
   criticality, then slack, then intent id, so arbitration never depends on container order.
4. **Capacity is spent from evidence.** Available capacity is the link's declared capacity minus its reserved
   floor minus the sum of grants to live flows.
5. **Preemption is explicit.** Only preemptible, non-barrier-critical, strictly-less-urgent flows in the same
   group are eligible, least urgent first; the victims are closed as cancelled and their decisions marked.
6. **Unknown stays conservative.** An ``UNKNOWN`` phase is arbitrated at the worst priority, capped by the
   policy ceiling, never isolated and never preempting.
7. **Invalidation is by rule.** Strict policy invalidation invalidates every live decision; otherwise a
   decision survives only if its class, priority and grant are unchanged. A new topology generation
   invalidates everything bound to the old one.
8. **Accounting closes.** A step closes with zero active flows and bytes committed equal to bytes completed
   plus bytes cancelled; fabric-initiated cancellation never invents transferred bytes.
