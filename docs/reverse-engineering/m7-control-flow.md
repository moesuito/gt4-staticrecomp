# M7 control-flow evidence — slice 1: classification and basic blocks

2026-10-01: BUILD/VERIFY passed for this slice. EXPLAIN lesson pending.

## Scope

Slice 1 answers two questions for a file-backed address: *how does the
instruction leave normal execution?* (`classify`) and *what is the first
straight-line block starting there, including delay slots?*
(`build_basic_block`). It does not follow transfers yet: CFG traversal, queued
work lists, reachability evidence and the function map are the next slices.

## Flow classification

| FlowKind | Encodings | Delay slot | Static facts recorded |
| --- | --- | --- | --- |
| FallThrough | arithmetic, loads/stores, shifts, LUI | no | continuation = next word |
| Branch | BEQ/BNE/BLEZ/BGTZ/BEQL/BNEL and the REGIMM family | yes | target, continuation = pc+8 |
| Jump | J | yes | target |
| Call | JAL (target), JALR (register) | yes | direct target when known; continuation = pc+8 |
| Return | JR with rs = ra | yes | none (dynamic) |
| IndirectJump | JR through another register | yes | none (computed) |
| Exception | SYSCALL | no | none (handler decides) |
| Unsupported | outside the decoded subset | unknown | none; the walk stops |

`jr ra` is treated as Return by the ABI convention (ra is register 31). This is
a documented heuristic, not proof that every `jr ra` returns from a function.
For calls, the continuation is pc+8 because the encoding writes that address as
the return value; the callee's return path is dynamic.

## Block walk rules

- The ending transfer's delay slot is consumed and belongs to the block, even
  if that exceeds the caller's limit by one.
- A limit stop or the end of file-backed text ends the block as FallThrough
  with `continuation` pointing at the resume address.
- An unsupported word ends the block; the block includes it and claims nothing
  about it.
- An unsupported delay slot keeps the transfer's static facts and is flagged
  (`delay_slot_unsupported`).
- A branch or jump inside a delay slot is architecturally undefined: the block
  ends as Unsupported with `reason=branch-in-delay-slot` and no successors.
- A transfer at the very end of file-backed text keeps its facts; the missing
  delay slot is reported as `reason=range`.

## Worked example on the real image

```powershell
.\build\gt4blocks.exe private/fingerprint-check/CORE.GT4 0x5a3140 40
```

Stdout (trimmed to the interesting lines):

```text
005a3140: 27bdffd0  addiu sp, sp, -0x30
005a3168: 12400011  beq s2, zero, 0x005a31b0
005a316c: 0080982d  daddu s3, a0, zero
```

Stderr summary:

```text
block=0x005a3140 end=0x005a3170 instructions=12 ending=branch target_known=1 target=0x005a31b0 continuation=0x005a3170 reason=branch delay_slot_unsupported=0
```

The delay slot `daddu s3, a0, zero` is inside the block; the branch target
`0x005a31b0` and the fall-through `0x005a3170` are the two static successors.
Twelve instructions, one control transfer, no guesses.

## Verification

- Unit fixtures cover classification of every FlowKind against literal words
  (including wrap-around targets and the REGIMM family) and block walks for the
  limit stop, range stop, branch, call, return, indirect jump, syscall,
  unsupported word, unsupported delay slot, branch-in-delay-slot, first-word
  transfer and the end-of-text edge, plus four malformed-argument rejections.
- The numeric target helpers moved from the formatter into `ee_decode.cpp` so
  classification and display share one implementation. The regenerated
  ten-region listing and the Ghidra comparison reproduced the previous result
  exactly: `matched=417 non_nop=352 unsupported=71 mismatched=0`.
- Optional Python CLI checks (needing the private CORE) assert the real block at
  `0x5a3140` (12 instructions ending at `0x5a3170`, target `0x5a31b0`) and the
  unsupported single-word block at `0x00100008`.

## Limits

Static block boundaries describe the encoding, not observed executions. A
branch target may hold data; a block may cross a return into neighboring code.
The CFG pass must not assume that every reachable address is a function entry.
Nothing here executes guest code.

Next: CFG traversal over successors with uniqueness and counts, then the
evidence-backed function map; the M7 lesson follows once the slice set is
stable.
