# Protocol

## Framing

Fixed 44-byte header, payload, 4-byte payload CRC (48 bytes of overhead):

| Offset | Field | Notes |
| --- | --- | --- |
| 0 | magic u32 | ``'TTF1'`` (0x31465454) |
| 4 | version u16 | protocol version, currently 1 |
| 6 | type u16 | ``MessageType`` |
| 8 | flags u32 | reserved; responses set bit 0 |
| 12 | session u64 | 0 before ``HELLO`` |
| 20 | sequence u64 | strictly increasing per direction |
| 28 | nonce u64 | non-zero, unused within the replay window |
| 36 | payload length u32 | checked against the maximum before any read |
| 40 | header CRC u32 | CRC-32C over bytes 0..39 |
| 44 | payload | |
| 44+len | payload CRC u32 | CRC-32C over the payload |

Decoding rejects bad magic, unsupported version, bad header or payload CRC, an over-large declared length,
truncation and trailing bytes. Corruption is never resynchronised past silently.

## Messages

``HELLO``, ``HELLO_ACK``, ``GOODBYE``, ``PING``, ``PONG``, ``REQUEST``, ``RESPONSE``, ``EVENT``, ``ERROR``.
A message is a type plus up to 48 tagged fields, encoded as a count followed by ascending, unique,
length-prefixed fields. Text is validated UTF-8 with a hard byte bound; fixed-width types carry exact widths;
blocks are nested canonical structures with bounded counts.

Operations: ``REGISTER_JOB``, ``REGISTER_GROUP``, ``PUBLISH_TOPOLOGY``, ``APPLY_POLICY``, ``RETIRE_JOB``,
``REBASE_EPOCH``, ``BEGIN_STEP``, ``BEGIN_PHASE``, ``END_PHASE``, ``END_STEP``, ``REQUEST_TRAFFIC``,
``COMPLETE_FLOW``, ``REVALIDATE_FLOW``, ``BEGIN_CHECKPOINT_BURST``, ``END_CHECKPOINT_BURST``,
``EVALUATE_PACING``, ``ADMIT_REPLACEMENT``, ``RETIRE_INCARNATION``, ``LOOKUP_JOB``, ``LOOKUP_HANDLE``,
``LOOKUP_DECISION``, ``LOOKUP_STEP_REPORT``, ``LOOKUP_FLOW_RECEIPT``, ``GROUP_UTILIZATION``,
``CURRENT_POLICY``, ``STATUS``, ``SHUTDOWN``.

Tag values are stable wire values. Never renumber one; only append.

## Session and identity binding

1. The client sends ``HELLO`` with its 128-bit boot identity and protocol version.
2. The coordinator answers with the session id (epoch in the high 32 bits), the epoch, its own boot identity,
   the session ceiling and the payload ceiling, or an ``ERROR`` frame when it refuses.
3. ``REGISTER_JOB`` binds the session to the minted job, generation and incarnation. A process that is not the
   owner binds to an existing job with ``LOOKUP_HANDLE``, and regains authority only through
   ``ADMIT_REPLACEMENT``.
4. Every later request carries an authority token whose provenance fields the coordinator **overwrites** from
   the session envelope. A token that claims a different job, incarnation, boot identity, session or epoch is
   refused with ``AuthorityMismatch``: identity is proven by the connection, never asserted by the caller.

## Replay and duplicate handling

Per session and direction: sequence must strictly increase; a nonce must be unused inside a bounded window
(256 by default). A repeated sequence or nonce is ``ReplayDetected``; a sequence that moves backwards is
``SequenceViolation``; a zero nonce is ``ProtocolViolation``. The window is deliberately finite, so sequences
(not nonces) are the ordering authority.

## Bounds

Maximum payload 64 KiB; maximum 48 fields per message; text at most 256 bytes; collection counts bounded per
type; state files at most 8 MiB. Every one of those is checked before it can influence an allocation, and a
violation is a named error, never a silent truncation.
