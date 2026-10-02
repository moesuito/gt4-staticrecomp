# M13 evidence — the first real GT4 function compiled natively

2026-10-01: BUILD/VERIFY passed. This is the first major technical landmark.
EXPLAIN lesson pending.

## Candidate selection

The evidence-backed function closures (expanded with extra seeds) were scanned
for a small leaf function:

- Rejected: syscall stubs (`0x5ae100`, `0x5adb30`, ...), COP0 accessors
  (`0x5b72a8`, `0x5b72f8`), and functions with jump tables — `0x5af850` reads
  like a normal prologue but contains a `jr a0` switch and `lbu`/`sra`/`slti`
  words outside the decoded subset.
- Chosen: **`0x00577878`** — discovered as a `direct-call` target, a single
  block of **4 instructions**, every word supported. It is a real setter: it
  stores `a1`, `a2`, `a3` into `[a0]`, `[a0+4]`, `[a0+8]` and returns, and its
  third store is the `jr ra` **delay slot** — so the very first translated
  function exercises memory semantics and the delay-slot rule together.

## Translation approach

New tool `gt4translate`: reads the verified CORE, decodes with the M6 decoder,
classifies with the M7 flow model, and emits a C++ header with one statement
per instruction plus the original assembly as comments. Scope of this slice:
single-block leaf functions ending in `jr ra`; anything else is rejected with
the offending address and instruction, with no partial output.

The delay slot is emitted *before* the return, in execution order, and marked
in the comments. `jr ra` becomes `state.set_pc(low 32 bits of r31)`.

The generated header is derivative of game code: CMake generates it **into the
ignored build tree** and never commits it. The translation test only exists on
machines that have the local CORE, matching the input-hygiene rules.

## The generated function (actual output, trimmed)

```cpp
inline void function_00577878(ee::GuestState& state) {
    // 0x00577878: ac870008  sw a3, 0x8(a0)
    state.memory().write_word(detail::effective_address(state, 4, 8), state.read_gpr32(7));

    // 0x0057787c: ac850000  sw a1, 0x0(a0)
    state.memory().write_word(detail::effective_address(state, 4, 0), state.read_gpr32(5));

    // 0x00577884: ac860004  sw a2, 0x4(a0)
    state.memory().write_word(detail::effective_address(state, 4, 4), state.read_gpr32(6));

    // 0x00577880: 03e00008  jr ra (the delay slot above runs first)
    state.set_pc(static_cast<std::uint32_t>(state.read_gpr64(31))); // return to ra
}
```

## Verification

- `ee_translation` test: **6 input states** (zero, negative, high-bit and mixed
  patterns for `a1..a3`; different structure pointers into the scratch area;
  different `ra` and junk registers). For each state the test runs two
  independent paths:
  1. the **translated C++ function** on a fresh `GuestState`, and
  2. the **interpreter** executing the same four real words, loaded from the
     verified CORE.
  It then compares **all 32 registers**, the **entire memory image
  byte-for-byte** (the full loaded text), and the continuation pc (`ra`).
- Result: `translated 0x00577878 matches the interpreter on 6 input states`
  (13/13 CTest). The milestone asked for at least five valid input states
  matching registers, touched memory, writes and continuation.
- CLI checks: byte-identical repeated runs, output-file/stdout equality, and
  rejection of unsupported words and internal transfers with context.

The two paths are independent implementations of the same documented
semantics; agreement is evidence of implementation correctness, exactly like
the Ghidra comparison was for decoding. It is not hardware execution proof —
that is what the later observation milestones (M14+, PCSX2 captures) add.

## Limits

One function; the translator handles single-block leaves only. Branches, calls
and unsupported families need the next translator slices. The interpreter
remains the oracle, and both sides share the same specification — the next
external observation step is what breaks that shared-speculation risk.

## Next

- M14 direction: automate observation/snapshots toward PCSX2 comparison, and
  extend the translator along the M7 CFG shapes (branches first).
