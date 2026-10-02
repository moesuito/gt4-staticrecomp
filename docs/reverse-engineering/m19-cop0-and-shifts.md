# M19 — COP0, BREAK and the 64-bit shift family

Date: 2026-10-01/02. Inputs: the pinned CORE. References: PCSX2 master
(`COP0.cpp` for the move semantics, the EI/DI gate and the Status read mask,
`R5900.h` for the Status bit layout), fetched 2026-10-01; used as documentation
of hardware behavior, no code copied.

## What was added (206 operations in total)

- **CP0 register file** in the guest state, starting from the live menu state
  the M14 observation captured: `Status = 0x40000000` (CU2 usable) and
  everything else zero. The code under test was captured from a running game,
  not from a cold reset, so the model starts where the game was.
- **`mfc0`**: sign-extended 32-bit read; `rt == 0` skips the read entirely
  (except register 9, Count, per the reference); register 12 (Status) reads
  through the `0xf0c79c1f` mask; the performance counter (25) stops with
  context instead of inventing a value.
- **`mtc0`**: writes through; register 16 (Config) protects the read-only
  cache-size bits and reports the fixed ones (`(value & ~0xFC0) | 0x440`);
  register 24 (Debug) accepts the write as feedback only; the performance
  counter stops with context.
- **`ei`/`di`**: both gated exactly like the reference — they take effect in
  kernel mode (`KSU == 0`) or when already in an exception level
  (`_EDI`/`EXL`/`ERL`), toggling `Status.EIE` (bit 16). The bit layout comes
  from PCSX2's `CP0regs` union.
- **`break`**: decodes and traps like `syscall` — the model stops at the word
  (the breakpoint handler is not modeled). In translated code it becomes an
  automatic halt, the same boundary rule as syscalls.
- **The 64-bit shift family (12)**: `dsll`/`dsrl`/`dsra`, the `…32` forms
  (amount + 32) and the variable `sllv`/`srlv`/`srav`/`dsllv`/`dsrlv`/`dsrav`.
  Added because the 0x58ce48 call tree reached `dsll32` — the
  `dsll32/srl`-style sign-extension idiom the function at 0x5bae00 opens with.

The translator emits the CP0 operations with helpers that mirror the
interpreter bit for bit (read mask, Config protection, EI gate), so translated
functions using `mfc0 Status` / `ei` stay verifiable.

## Evidence

- Hand-computed interpreter fixtures: the masked Status read, `mtc0` write-back,
  EI setting EIE in kernel mode and being gated out with `KSU` set to
  supervisor, the Config write masking, the break stopping with context, and
  the shift family's round-trips (`dsll`→`dsll32`→`dsrl32`), sign fills
  (`dsra32`/`srav`) and variable amounts.
- Decoder/disassembler fixtures for all 17 new encodings; the disassembler
  names the CP0 registers architecturally (`mfc0 v0, Status`).
- The CLI tests moved their "unsupported word" example to `ldl` at 0x001041f4
  because the COP0 and break words now decode; a scan of the first 200,000
  text words found 655 unsupported words, concentrated in the unaligned
  64-bit family (`ldl`/`ldr`/`sdl`/`sdr`) — the next op family.
- CTest 21/21; Python 71 collected (65 run, 6 skip).

## Survey after the slice

The 0x58ce48 call tree advanced through movn → lwl → the tail thunk and its
syscall boundary → cache → the critical edge → COP0 and `dsll32`. It now
stops at:

```text
ERROR: A transfer leaves the function in function 0x005bae00:
beql a2, zero, 0x005baeac at 0x005baea4
```

The cause is visible in the code: the beql's delay slot is a `break` (the
division trap idiom), so the walker marks the block `branch-in-delay-slot`
and stops following. The correct representation already exists in spirit: a
likely branch's delay slot runs only when taken, and the trap stops at the
boundary — so the emission should be `if (taken) { set_pc(delay); return; }`
with no inline statement. That, and the `ldl`/`ldr`/`sdl`/`sdr` family, are
the next two recorded pieces.
