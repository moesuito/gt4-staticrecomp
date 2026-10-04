# 0035 - P07 RPC telemetry and strict mode (slice 73)

Date: 2026-10-04. Status: accepted (implemented in slice 73).
Predecessors: 0014 (service handshakes), 0015 (SIF register mirror),
0017 (disc image file service), 0019 (PCDV disc reads), 0020 (dual layer
and PCDV volume ops), 0021 (PRTS block cache). Evidence:
`docs/reverse-engineering/slice73-p07-rpc-telemetry.md` (the measured
inventory from the 400/3000/20000/90000-service disc legs plus the
strict-mode stop). Closes no incident; answers PLAN.md item P07
(RPC rastreavel e estrito) without changing any reply byte.

## Context

Contract C12 (generic zeroed RPC replies) coexists with the green
differential: the boot binds 23 SIF servers by 90,000 services, and
every unlisted (SID, function) pair answers a silent empty result.
PLAN section 4.4, item 6 forbids declaring a found SID a working
protocol: the Opus SID map is a candidate list, and each pair needs
registration, buffers, completion and consumer evidence. The model
also carries two disputed identities (0x80000400 as fileio/CDVD vs
MCSERV, 0x80001300 as disc subsystem vs DBCMAN) whose values the
game's own checks accept either way.

## Decision

**Observe, classify and stop loudly; change no reply.**

1. **Telemetry is observation only.** `Kernel` records every bind and
   every call with caller pc, thread, send/recv/result sizes, receive
   buffer, server buffer and the first four request words, plus per
   pair the calling threads and sample pcs. Recording reads the guest
   and writes nothing, so the default behavior is byte-identical with
   or without an observer.
2. **Telemetry is not state.** The pair and bind maps never enter the
   snapshot blob, `states_match` never sees them, and both engines
   record identically. Checkpoint compatibility is unchanged:
   `rpc_model` stays 1, no provenance bump.
3. **Honest per-pair classes** (`classify_rpc_pair`): implemented and
   verified (real data path consumed by the game), compatibility
   constant (the game's own check accepts the value), valid absence
   or failure (currently no pure pair: the not-found paths live
   inside implemented pairs and their notes name them),
   explicit provisional (verified word inside an unverified reply),
   unknown (the silent empty result). Named SIDs without pair
   evidence stay candidates; the two disputed identities say so in
   their notes.
4. **Strict mode is opt-in.** Off by default the boot behaves exactly
   as before. With `--strict-rpc` the first unknown pair throws
   before any reply is written, naming pc, thread, SID, function,
   send/recv sizes, both buffers, the sd handle and the request
   words. Both engines take the flag so a differential stays
   comparable.
5. **Inventory is printed, not guessed.** `--threads` prints the bind
   table and the per-pair table from the run above; completion reads
   `sync-end-packet+dmac5` and callback reads `none-tracked` because
   that is what the model implements today.

## Acceptance (all green in slice 73, MSVC 19.51 x64, disc boot)

- 400-service legs (with and without disc): 1 pair, 0 unknown calls,
  pinned by the new `gt4boot_rpc_inventory` CTest.
- 3000/20000/90000-service disc legs: 22 pairs, 32 unknown calls
  from the real boot; module calls 215013 at 90k, identical to the
  slice-71 census leg.
- Strict leg: stops at sid 0x80000592 fn 0 with the full context
  (exit 1, FAILURE line) instead of the silent empty result.
- Default legs are behavior-identical: the 90k differential stays
  green (exit 0, state identical), because no reply byte changed.
- `ee_kernel` unit tests pin the classes, the recording and both
  strict directions (unknown stops, known answers).

## Explicit non-coverage

- No per-pair async callback is tracked; every pair completes
  synchronously today and the printout says so.
- Every caller pc at 90k is the single SIFRPC wrapper 0x005AE064 on
  thread 1: the pc does not discriminate callers yet; SID, function,
  sizes and buffers do.
- The absence-or-failure class has no pure pair; the file-open
  handle-0 and pre-registration volume-size zeros are named inside
  their implemented pairs.
- Provisional pairs (PCDV 1, LGDEV 12/4) show no calls at 90k; their
  evidence comes from earlier slices and the 300k-service journal
  census, which also lists the later-phase unknowns (PBGM 8, SPUP 4,
  LGDEV 6/0x0D/0x0F) for the next slice.
