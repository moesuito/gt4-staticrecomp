# M17 — Tail thunks, syscall boundaries and the cache hint

Date: 2026-10-01. Inputs: the pinned CORE.

## What changed in `gt4translate`

- **Functions entered above their own transfers now translate.** The walk may
  reach addresses below the function's entry (tail thunks jump down to shared
  stubs); the old "a transfer leaves it below its entry" rejection is gone.
  The emission scans from the lowest reachable address and opens the body
  with a `goto` to the entry label, so a caller still starts at the entry.
  Runs of addresses with no reachable code collapse into one range comment —
  the thunk below spans a 4,812-word gap and the generated function stays
  readable.
- **Syscalls reached by the walk become stop points** (like the explicit halt
  argument): the module sets the pc at the syscall word and returns, exactly
  where the interpreter stops. Call sites now propagate the stop:
  `if (state.pc() != <link>) { return; }` after every direct call — a normal
  return sets pc to the link, so any other pc means a callee stopped at a
  service boundary.
- **CACHE decodes as a no-op hint** (the reference treats it the same way),
  with decoder, disassembler, interpreter and translator coverage; it is what
  blocked the next step of the 0x58ce48 call tree.

## Verification

- The new `ee_translation_thunk` test translates the tail thunk at
  `0x005b27f8`: it loads a word, jumps down into the BIOS trampoline at
  `0x005adcc0` (`addiu v1, 0x42; syscall; ...`) and both the native module and
  the interpreter must stop at the syscall word with every register identical:

```text
translated thunk 0x005b27f8 matches the interpreter on 5 input states,
both stopping at service 0x42
```

- The startup translation and the call-tree tests re-verify unchanged with the
  new call-propagation checks. CTest 20/20; Python 71 collected.

## The 0x58ce48 call tree, one gap at a time

The survey advanced through four recorded gaps in this session: `movn`
(done), `lwl` (done), the below-entry tail thunk with the syscall boundary
(done) and `cache` (done). It now stops at:

```text
ERROR: A branch targets a delay slot at 0x005b0fcc
```

A branch whose target is another transfer's delay slot is architecturally
legal but the current emission inlines each delay slot into its transfer, so a
second entry point into it cannot be represented. Handling critical edges
like this is the next structural piece of the translator, recorded here with
the address.
