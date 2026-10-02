# M9 guest state evidence — explicit registers and memory

2026-10-01: BUILD/VERIFY passed. EXPLAIN lesson pending.

## Scope

M9 defines what guest values *mean* before anything executes: a 32-entry
64-bit register file with the CPU's 32-bit sign-extension rule, and a
byte-addressable little-endian memory region with explicit alignment and
bounds rules. Nothing here executes guest code, and no frontend exists yet:
there is nothing to observe until the M10 interpreter consumes this API.

## Register rules

| Rule | Behavior |
| --- | --- |
| Width | 32 registers × 64 bits, storing values (not an address cache) |
| R0 | constant zero: reads return zero, every write is ignored |
| 32-bit writes | sign-extend into the 64-bit register, the CPU's rule for all 32-bit results |
| 32-bit reads | return the low 32 bits |
| Index | a 5-bit guest field; values ≥ 32 throw instead of indexing out of bounds |
| PC | a plain 32-bit guest address; no wrap logic at this layer |

## Memory rules

| Rule | Behavior |
| --- | --- |
| Region | one contiguous region; base + size must fit the 32-bit address space; size must be nonzero |
| Endianness | little-endian, assembled byte by byte with explicit unsigned shifts |
| Alignment | natural alignment per width (2/4/8); a misaligned access throws |
| Bounds | the whole access must lie inside the region; uint64 math prevents address wrap |
| Errors | std::runtime_error naming the address and width; never silent, never host memory |
| Bulk | `write_bytes` copies at byte granularity, for loading images and test fixtures |

## Why this is the honest shape for M9

- No host undefined behavior: unsigned shifts, sign extension via an explicit
  mask, uint64 range math, and no reinterpretation of the byte buffer as a
  wider type on any path.
- No overreach: one region covers what the fixtures and the coming interpreter
  need. The full EE address map (main RAM, scratchpad, hardware registers),
  exceptions and interrupts are added when observed execution requires them.
- Errors are context, not crashes: every invalid access throws with the
  address, so the interpreter milestone can decide which failures become guest
  exceptions and which are hard stops.

## Verification

- Unit fixtures cover: constructor limits (zero size, region crossing the top
  of the address space), little-endian byte order for all four widths,
  register roundtrips, the zero register, 32-bit sign extension in both
  directions, misaligned and out-of-range reads/writes and bulk copies,
  `contains()` bounds (including the topmost word `0xfffffffc`), PC roundtrip
  and state-owned memory access.
- Everything is synthetic: no real image is involved, so there is nothing to
  compare against Ghidra at this layer. The first real-data check arrives with
  the M10 interpreter running decoded instructions against hand-computed
  expected values.

## Limits

No HI/LO pair yet (they arrive with the multiply/divide instructions that use
them and with the first observed need), no exceptions or interrupts, no
caches, one memory region only, and no execution. Alignment errors throw
instead of modeling the hardware's address-error exception; that divergence is
deliberate and recorded here.

Next: M10-M12 — a small test interpreter over this state, starting with
straight-line synthetic programs and their hand-computed results.
