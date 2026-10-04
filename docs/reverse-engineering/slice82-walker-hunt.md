# Slice 82: the walker hunt (D5) — who reads the boot-step table pointer

Date: 2026-10-04. Baseline: main at 50b426d (slices 80-81, clean tree).
Task (slice 82, D5 from draft decision 0037): find who reads the
boot-step table pointer — the walker that can invoke BUILD (table entry
#27) — statically (disassembly + whole-text scans, no game bytes in git)
and live (boot to 5M with temporary reverted instrumentation): does the
walker run, with what arguments, how many times, and what trigger would
invoke BUILD — or proof nothing invokes it on the current horizon.
PROHIBITED and kept: no behavior change, no fabricated traffic, no
conclusion without a discriminant. No commit, no push, no branches.

Method: whole-text scans over the pinned input
(`private/fingerprint-check/CORE.GT4` — only addresses, counts, handle
values and relations below) via `build/gt4disasm.exe` plus read-only
inflate+scan host Python scripts (zlib raw-deflate + record parse, same
layout as `src/executable/core_image.cpp`; text base 0x00100000,
1,334,917 words; data record base 0x00617A80). Scan scripts live only in
the approved host temp dir, not in git. Live legs use a temporary
env-gated (`GT4_WALKER_TRACE`) read-only pc/ra/a0/a1 log at driver
module entries and bridge steps, reverted via `git checkout` after
extraction, with neutrality triples. Confidence labels per project rule.

## (1) Static: the table is the TAIL of a 370-word run (Confirmed)

- Table re-verified word for word (full-text scan): 60 code-address
  entries [0..59] at 0x00617400..0x006174EC ([26] = 0x00548328 neighbor
  build, [27] = 0x005489E0 BUILD, [28] = 0x0054F708), terminator
  (0, 0xFFFFFFFF) at [60,61] = 0x006174F0/0x006174F4. Matches slice 81-E2.
- NEW: the table is not standalone. Preceded by a -1 gate word at
  0x00616F24 (itself preceded by a NULL at 0x00616F20) and 310 further
  code-address words at [0x00616F28..0x006173FC] (table1), with NO null
  between table1 and the 60-entry table2 — one continuous 370-word run
  ending at the NULL at 0x006174F0. (0x006174F8 holds 0x001008F0, the
  TEARDOWN twin of table1[0] — data after the terminator, not walked.)
- ALL 370 run words are a0=1 BUILD wrappers (full 370-target template
  check: prologue `addiu sp,sp,-0x10; addiu a0,zero,1; sd ra; jal DISP;
  ori a1,zero,0xFFFF; ld ra; jr ra; addiu sp,sp,0x10` — 370/370 match,
  each with its own subsystem dispatcher; 690 such wrappers exist
  game-wide, almost all in (BUILD, TEARDOWN=BUILD+0x20) twin pairs).
  TEARDOWN wrappers appear nowhere in the run.

## (2) Static: the walker 0x005BC4C8 (Confirmed by disassembly)

The walker reads NO pointer from BSS/argument/stack — correction of the
slice-81 E3 caveat's hypothesis. It materializes its cursor from
immediates and reads the gate/count from the static image:

- Entry 0x005BC4C8 (sole static caller chain below; `j 0x005BC4C8` at
  0x005BC5AC confirms the top): setup call, then
  `lui v0,0x61; addiu a1,v0,0x6F24` (a1 = 0x00616F24),
  `lw a0,0(a1)` ([gate] must be -1, else a0 is used as an explicit
  count), `lw v0,4(a1)` ([first] must be nonzero) — else skip to the walk.
- Count loop (0x005BC510-0x005BC534): scans words at base+8, +12, ...
  until the first NULL; a0 = words scanned including the NULL. With the
  file's layout (first NULL at 0x006174F0) the count is 370.
- Walk loop (0x005BC554-0x005BC568): s1 = 0x00616F24 + count*4
  (= 0x006174EC first), then per iteration `lw v0,0(s1); s1 -= 4;
  jalr ra,v0; count--` until count == 0 — 370 descending calls:
  table2[59..0] first, then table1[309..0]. (ra = 0x005BC564, the exact
  value in every live hit.)
- End: `lui a0,0x5C; addiu a0,-0x3BC8; j 0x005A2ED0` — the walker never
  `jr ra`s; the boot continues at 0x005A2ED0 (a fresh function prologue).
- TRUE-pair scans (reaching-definition, whole text): ZERO static
  materializations of any address in [0x00617000,0x00617600] (corrected
  scan window; an early naive lookahead falsely flagged five
  `lui a0,0x61` sites whose a0 is redefined by `lui a0,0x60` before use —
  recorded honestly, eff 0x00607408, unrelated); only the walker's own
  two reads touch [0x00616E00,0x00617500]; pointer-constant scans find
  the table base nowhere in text or file-backed data (except no
  self-slot — the table holds code addresses, not its own base).
  Consequence: no code computes the table address except the walker,
  and the walker computes it from immediates.

## (3) Static: one-shot guard + single feeder chain (Confirmed)

- Guard 0x005BC588 (disassembled verbatim): `lui v0,0x89;
  addiu a0,v0,-0x2838` (a0 = 0x0088D7C8, BSS one-shot flag);
  `lw v1,0(a0); bne v1,zero,return; v0=1; sw v0,0(a0); j walker`.
  First call walks, later calls return immediately.
- Sole static caller of the guard: 0x00107F18 (whole-text jal scan;
  zero jal sites to the walker entry itself). Sole static caller of the
  feeder 0x00107F08 (whose straight-line body calls the guard):
  **0x00100210** — the ELF startup path (entry 0x00100008: zero regs,
  SetupThread 0x3C -> sp, SetupHeap 0x3D, ... `lw a0,[0x006D6380];
  jal 0x00107F08; j 0x005A3140` — never returns).
- Guard-flag writers: the guard's own `sw` is the ONLY true pair
  touching 0x0088D7C8 in the whole text — nothing clears it.
  (Residual: indirect `jalr` callers of guard/walker/feeder cannot be
  excluded statically; the live 5M census below is the positive control.)

## (4) Live to 20k: the single BUILD pass (Confirmed, neutrality triple)

- Temporary hook (driver module-entry + bridge-step, watchlist pcs,
  stderr log, 2M cap; reverted after): leg B stats IDENTICAL to leg A
  (50985 module calls / 1254673 steps / 20000 services, boundary idle
  0x1604/0x100 — the slice-78 F20 triple, twice).
- 64 distinct watchlist pcs, each EXACTLY ONCE (61 bridge + 3 module):
  all 60 table2 entries descending (#59 at steps=21907 first, #0 at
  steps=23849 last) with ra=0x005BC564 throughout; BUILD wrapper
  0x005489E0 then dispatcher 0x00548950 (module leg, ra=0x005489F4,
  a0=1, a1=0xFFFF) then C-builder 0x00548458 (module leg); neighbor
  entries #25/#26; the init thunk 0x00101930/38 (ra=0x00577438 — NOT
  walker-called) then init 0x00548500 (module leg). TEARDOWN 0x00548A00:
  zero hits. Entry a0/a1 values are walker-register residue (wrappers
  overwrite both) — recorded, not interpreted.
- Stop-time dumps: gate [0x00616F24] still FFFFFFFF after the walk.

## (5) Live to 5M: no second pass (Confirmed)

- Big watchlist (385 sorted addresses: all 370 walk targets + walker
  entry/loop/guard/feeder/startup/dispatcher/wrappers/builders/init;
  binary search) + same hook; 5M leg with `--steps 400M`:
  boundary iSignalSema 0x005adcd4 (the standing in-cycle stop),
  **11720710 module calls / 271457654 steps / 5000000 services** —
  the exact slice-78 F5M triple, neutrality preserved end to end.
- Census over the whole 5M: **747 hits; all 370 walk targets exactly
  once** (zero missing); the walk-loop pc 0x005BC558 exactly 370x;
  8 non-target extras once each (startup jal site, feeder, thunk pair,
  C-builder, init, dispatcher, loop); dispatcher 0x00548950 once
  (a0=1 BUILD, ra=0x005489F4); TEARDOWN 0x00548A00 zero. First hit:
  startup 0x00100210 (calls=136, steps=18907); walk table2[59] at
  steps=21907 through table1[0]; last hit: init 0x00548500
  (calls=8664, steps=272035). No watchlist address re-fires in the
  remaining ~4.98M services.
- Stop-time dump: guard flag [0x0088D7C8] = 1 (spent, never cleared);
  gate [0x00616F24] still FFFFFFFF (20k dump).
- Mechanism footnote (positive control intact): the translator follows
  direct jumps, so the walker + its continuation live INSIDE translated
  `function_005bc588` (header lines 3498502+, feeder line 71522 calls it
  as an internal dispatch) — the guard/walker entries never surface at
  the driver hook, while every indirect entry-call exits to the bridge
  (all target hits are the bridge leg) and the loop body re-enters it
  370x. The counts, not the legs, carry the proof.

## Verdict (D5 closed; trigger named)

- The walker is 0x005BC4C8. It takes no arguments (a0/a1 overwritten
  before any read); it feeds itself the cursor 0x00616F24 from
  immediates — not from BSS, not from an argument, not from the stack
  (correction of the slice-81 E3 caveat hypothesis). The only BSS is
  the one-shot guard flag [0x0088D7C8].
- The trigger that invokes BUILD (entry #27, and all 369 siblings) is
  the single early-boot pass: ELF startup 0x00100210 -> feeder
  0x00107F08 -> guard 0x005BC588 -> walker, 370 descending BUILD calls.
- Proof nothing re-invokes it here: one static caller per hop, a
  one-shot guard with no static clearer, and the live 5M census (every
  watched address at most once, TEARDOWN zero, flag spent).
- Consequence for draft 0037 candidate A: the BUILD re-run has a named
  trigger that provably already fired and provably cannot re-fire in
  this boot — the reference's menu-phase fresh ids must come from a
  route our boot never takes (H1/H2 still reference-side; D2/D4 armed).

## Gates and hygiene

- Static only in git: this doc + journal (+ STATUS line). Scans/scripts/
  logs in temp or ignored build dirs. No payload bytes (addresses/counts
  only). The 14 GB-class scratch of slice 47 was not repeated: trace
  logs stayed under 100 KB (747 lines at 5M); no checkpoints taken.
- Full gates on the reverted tree (VsDevCmd `-arch=amd64` chained):
  configure+build green; **CTest 53/53**; **Python 73 collected — OK
  (skipped=6)**. Instrumented legs were neutrality-tripled (20k leg
  identical with/without the hook; 5M leg reproduces the slice-78 F5M
  triple exactly).
- No commit, no push, no branches.
