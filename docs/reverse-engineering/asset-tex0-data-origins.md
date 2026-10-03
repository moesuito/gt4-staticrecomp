# TEX0 data origins — record flow to the table entry, static stop rule
\nDate: 2026-10-03. Source: parallel scout (read-only), reviewed and
spot-verified by the owner-agent ([V] = re-disassembled today;
[S] = scout-reported, provisional). No product change.
\n## Record flow into the packets (Confirmed) [V]
\nRe-read `0x499580–0x4995F4`: `ori s7,s7,0x6` (the NLOOP=6 tag
stored later at `0x4995D4`), `lw v0,0x1C(s5)` (upload-record list
base off the entry object), stride walk `s1 = base + i*0xC`
(`addu s1,v0,s4; addiu s4,s4,0xC`), per-record `lhu t0,[s1+8]` with
delay-slot `lhu t1,[s1+10]`, and `a1 = s1+4; jal 0x4A4CA8`. The
upload packet's fields resolve as:
\n- `[t3+0x10]=0`, `[t3+0x14]=[rec+4]`, reg `0x50` — BITBLTBUF with
  low32=0 (CT32 source) and high32=`[rec+4]` carrying DBP/DBW/DBPSM
  (so `DBPSM = ([rec+4]>>24)&0xF`, High) [S field split, V flow].
- reg `0x51` TRXDIR=0 host→local; reg `0x52` TRXREG = W|H<<32 from
  `[rec+8]/[rec+10]`; reg `0x53` TRXPOS=0; tag `0x10000004` + `0x0E`
  (NLOOP=4, High) [S].
- The context packet's data words derive from the upload helper's
  return ORed with `0x30000000` (`or a0,v1,fp`, `fp=0x3000…`
  visible at `0x499584`) — whether that word is TEX0-with-TW or
  another register stays Unknown without live bytes [S reading,
  V-adjacent idiom].
\n## Two levels up: generic material path, loader out of reach [S]
\n- Level 1: `0x499508`'s object = its `a0` — in `0x499728` passed
  straight through, in the `0x105F1x` variant through
  `0x4AA6B8/0x4AA558` pre-passes then relocate→ctxpkt→pump→prime.
  Two bind variants, same convergence.
- Level 2: `0x499728` has 34 callers (generic material path, not
  texture-only); `0x499508` direct only from 3.
- Who fills `[obj+0x1C]` (the upload list)? **Unknown statically**
  — nothing read writes there; it is the material/.gpb loader,
  beyond the two-level stop rule. **This is where the static side
  correctly stops.**
\n## FIFO-priming fallback: answered and discarded [V shape]
\nRe-read `0x4A1140`: channel-indexed DMA-descriptor setup
(`sll a0*4; lui 0x63; lw -0x6238` = table `0x630000-0x6238+a0*4`)
+ `jal 0x4A0F68`, siblings same kick signature. DMA priming
confirmed; values are count/channel/flags, no pixel data —
**not an alternate PSM route** (hypothesis discarded).
\n## Trailheads for the main line (one shot each, queued)
\n1. `0x4995D4` (`sw s7,[s0]`), 128 B at `s0` — per-item context
   packet: is TEX0/`reg 0x06` there, with what PSM?
2. `0x4A4CD4–0x4A4DD0`, 80 B at `t3` — upload packet (BITBLTBUF
   with `DBPSM=[rec+4]>>24&0xF`, TRXREG with W/H).
3. Write-watch `[obj+0x1C]` of the object reaching
   `0x499780`/`0x105F3C` — the writer is the material loader
   (.gpb or runtime table fill).
With (3) answered, the viewer can start as a consumer of dumped
packets even before the .gpb grammar opens. Anchors:
`gt4disasm … 0x105f00 70`, `0x4a1100 70`.
\n## Verification (owner-agent)
\n- Record-walk shape re-read (`0x1C` base, `+0xC` stride, `+8/+10`
  halfwords, `+4` helper arg, NLOOP=6 tag build) — matches.
- `0x4A1140` descriptor-table + kick shape re-read — matches.
- Product code untouched; scout scratch owned by the scout.
