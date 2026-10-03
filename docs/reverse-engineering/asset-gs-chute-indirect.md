# GS setup chute — indirect all the way down
\nDate: 2026-10-03. Source: parallel scout (read-only), reviewed and
spot-verified by the owner-agent ([V] = re-disassembled today;
[S] = scout-reported, provisional). No product change. Standing
lesson reused: ELF string absence is inconclusive (live-only
strings confirmed twice now).
\n## Correction first: 0x004568F0 programs no GS register [V]
\nExact re-read: `sll v0,a0,2 | slti a0,a0,0x10 | beq-out |
lui at,0x84 | addu at,at,v0 | sw a1,0x65F0(at)` — i.e.
`if (a0 < 16) globals[0x8465F0 + a0*4] = a1`, a 16-slot table of
object pointers (the binder calls it with `a0=2`). Sibling
`0x4568C0` stores to the same base. The "GS setup" label of the
previous round was wrong; this is a **current-resource slot** (High).
\n## 0x00107BE8 = Tex1-object bind, not GS [S shape, V-adjacent]
\nChain `0x107B98 → 0x107B58 → 0x00498828 → 0x004A07F8 →
0x00499508/0x004A2038/0x004A4910/0x00499640`, where `0x00498828`
belongs to the **relocate family** (masks `0x3FFF`/`0xC000`,
64-bit field patching — same shape as the slice-44/45 fatal
`0x491798`), i.e. pointer fixup of the texture object, not pixel
format. Three call sites (`0x105F34/0x107C18/0x499778`) [S].
\n## 0x00105930 = generic field-setter family [S]
\nIndirect calls only, no format immediates; the `0x49ACxx` family
has three caller groups (`0x1059xx`, `0x107Dxx`, new `0x1FE7xx`
with constants `a1=0x80/1/4/5`, `a2=1`, `a3=0x7F` writing global
slots `0x839A40+`) — a generic set-field dispatcher the texture
path rides, not a format site. Reading `a1`=field-ID stays
Hypothesis.
\n## 0x004A07F8 = DMA/GIF pump poll, not TEX0 [V]
\nRe-read: `lui 0x7000`+`ori 0x2000` (`0x70002000` scratchpad),
`lbu 0x2C2`, state struct at `0x70002050+0x99…`,
`sd → 0x1010(0x1200…)` (FIFO `0x12001010`), `mtc0 PCCR` + `sync`.
State machine of the pump, not a TEX0 assembler.
\n## TEX0-constructor search: negative [S, with method]
\nText-segment scan for windows holding `sll *,14` + `sll *,20` +
`sll *,26` (TEX0 TW/TH/TBW idiom): 4 windows, all inside the
end-of-text data table (`0x616F28–0x617A14`) — data false
positives. Zero in real code (`sll14`=18, `sll20`=24, `sll26`=5,
`sll30`=3 whole-binary). Accepted as scout method + counts;
not independently re-run.
\n## Closing for the viewer (High)
\nChain is indirect end to end: binder → read-only 2D lookup
(`0x454EF8`; 3 callers, none a store — table filled by direct
writes at load) → global slot + bind/relocate → set-field family
→ DMA pump. **No PSM/TEX0/TEX1/TEXA/MIPTBP/CLAMP immediate in any
of these stretches.** The pixel format almost certainly **travels
as data** — texture-table entry fields (filled at .gpb load)
and/or prebuilt registers in the file records. The static road to
the format now runs through the **.gpb record grammar** (same open
problem as the page payloads), not more chute disassembly.
Optional next static (non-blocking): the 4 bind convergents
(`0x499508/0x4A2038/0x4A4910/0x499640`) may hold the GIF A+D
packet emitter (data/`reg=6` TEX0 pair would appear as a store).
Decisive main-line dump (queued): hook the lookup `0x454EF8`
(log `a0/a1/a2` + returned pointer + 64 Tex1 object bytes) —
object layout, dims, format and pixel pointer in one shot.
Anchors: `gt4disasm … 0x4568c0 120`, `0x107bb0 80`,
`0x105900 60`, `0x498800 80`, `0x4a07c0 100`, `0x1fe700 100`.
\n## Verification (owner-agent)
\n- `0x004568F0`/`0x4568C0` shapes re-read — match the correction.
- `0x004A07F8` neighborhood re-read (scratchpad poll, `0x70002050`
  struct, `0x12001010` FIFO store, PCCR+sync) — matches.
- Product code untouched; scout scratch owned by the scout.
