# GIF A+D emitter hunt — upload + context packets found
\nDate: 2026-10-03. Source: parallel scout (read-only), reviewed and
spot-verified by the owner-agent ([V] = re-disassembled today;
[S] = scout-reported, provisional). No product change.
\n## 0x004A4CA8 — texture-upload packet (High) [V shape]
\nRe-read `0x4A4D70–0x4DD0`: a tag word (`ori …,0x4` low) plus A+D
pairs whose register halves are the immediates **`0x50 / 0x51 /
0x52 / 0x53`** (`addiu *,zero,0x50…`, each `sd` at slot+0x8) —
BITBLTBUF / TRXDIR / TRXREG / TRXPOS, the GS **image-transfer**
registers (pixels up to VRAM, not context TEX0). The BITBLTBUF data
word is assembled as `a3<<16 | a2` (`sll v0,a3,0x10; or v0,v0,a2`)
— **the storage PSM rides inside that word**, fed from table-entry
args by the caller loop. Whole function `0x4A4C70–0x4A4E24` read
by the scout, incl. its jump-table dispatcher [S].
\n## 0x00499508 — context packet, NLOOP=6 (High) [V shape]
\nRe-read `0x4995B0–0x4995F0`: per-item `jal 0x4A4CA8`, then
`sw s7,[s0]` at **`0x4995D4`** (the tag slot), `s0 += 0x70` then
`+0x10` = **128 bytes/iteration = 1 GIFtag + 7 A+D pairs**, masked
data words (`and …,s6`). This is the packet that should carry TEX0
(`reg 0x06`) — but **no `0x06` immediate appears**; data comes from
`[s1]`-indirect words, i.e. from the texture entry. Confirming TEX0
needs one more level: what feeds `[s1+8/10]` [S for the exact
offsets; the indirection itself is visible above].
\n## The other three convergents [S, accepted]
\n- `0x004A2038` — Suspicious dispatcher, not emitter: cascaded `beq`
  (0/1/2 → `lui` constants, calls `0x4A1F60`) + hardware-poll tail
  (`cfc2 vi29/vi28`, `0x10003C00` VIF1_STAT, `0x10003020`
  GIF_STAT). Pump/DMA side.
- `0x004A4910` — Confirmed-unrelated: sets a busy/dirty bit, then a
  jump-table building 64-bit words by mask (`0x3F0/0xFC0F/…`,
  constants `0x140/0x130`) — PRIM-style word assembly, no
  data/address pair, no FIFO.
- `0x00499640` — Suspicious feeder: same loop shape as `0x499508`
  with per-item `jal 0x4A4CA8`, then FIFO priming
  (`0x5ADF20/0x4A0F08/0x4A1140(a0=2,a2=6)/…`); neighbor `0x499728`
  chains all three — the bind's topology in one place.
\n## Dump points for the main line (queued, no further static cost)
\n- `0x004995D4` (`sw s7,[s0]`): log 128 bytes at `s0` = **per-item
  context packet** (TEX0-as-`reg 0x06` should appear here).
- `0x004A4CD4–0x4A4DD0` (the `0x50–0x53` pair stores): log 80 bytes
  at `t3` = **upload packet incl. BITBLTBUF (PSM!)**.
- Fallback neighborhood: `0x4A0F08/0x4A0F38/0x4A0F68/0x4A1140`
  FIFO priming (`a0=2`) and callers of the pump store `0x4A07D0`.
Anchors: `gt4disasm … 0x4994d0 100`, `0x4a2000 100`,
`0x4a48e0 100`, `0x499610 100`, `0x4a4c70 110`.
\n## Verification (owner-agent)
\n- Upload-packet stores re-read: `0x50–0x53` immediates, slot+0x8
  placement, `a3<<16|a2` data assembly — match.
- Context-loop shape re-read: `jal 0x4A4CA8`, `sw s7` at exactly
  `0x4995D4`, `+0x70/+0x10` stride — match.
- Product code untouched; scout scratch owned by the scout.
