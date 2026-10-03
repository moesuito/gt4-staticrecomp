# M33, slice 25 — the lazy-init wrappers: USB-printer channels, none on the boot path

Date: 2026-10-03. Inputs: the pinned CORE and ISO, the whole-program
translation (3.5M lines), and a whole-text byte scan over the inflated
CORE records (entry `0x00100008`; text `0x00100000` +5,339,668;
`private/core-layout.pkl` recipe in the journal appendix — scratch
deleted). Five short `gt4boot` legs from `build/ckpt-1980k.bin`
(2,000 services, `--dump` only). No instruments, no model change, no
product-code change: docs only.

Charter: which of the four lazy-init wrappers the boot reaches for
first — what unparks main thread 1 or feeds its flag queue.

## Answer first

None of the four. The wrappers are USB-printer channel initializers,
fired by printer-peripheral events the parked boot never produces —
not steps on main's wake path. Two independent findings compose this:

1. Each wrapper has exactly one direct caller, all four callers are
   near-identical per-channel functions (F1–F4) with no callers and no
   data references anywhere in the image: the whole feeder subtree is
   indirect-only, event-driven work, and its stop-time state (one-shot
   flag 0, job global zeros) shows none of it ever completed.
2. Main's flag was posted exactly once, by early boot itself, and
   consumed: main now waits for a second post that has no source.

A reinterpretation follows (Hypothesis below): the `0x00587xxx`
"frame dispatcher" system hangs solely off printer init, so it may be
a print-render worker pool rather than a per-frame engine dispatcher.

## Exact addresses (Confirmed — whole-text byte scan for jal words)

Parsed the CORE records (`flags 0x0101`, raw DEFLATE; verified
`entry 0x00100008`, three records, exact end) and scanned all
5,339,668 text bytes for `0x0C000000 | (target >> 2)`:

| target | sole direct site | in function (entry) |
|---|---|---|
| sib1 `0x0019a7f8` | `0x001983a0` | F1 (`0x00198360`) |
| sib2 `0x0019a8c8` | `0x00197c88` | F2 (`0x00197c48`) |
| sib3 `0x0019a940` | `0x00197df0` | F3 (`0x00197db0`) |
| sib4 `0x0019a9a8` | `0x00197f58` | F4 (`0x00197f18`) |
| worker `0x0019a698` | 4 sites (the siblings' own calls) | — |
| one-shot `0x00101c50` | `0x001c9544` | — |
| setup `0x001c9468` | `0x0019a6d8` | — |
| poster `0x001092d0` | `0x00108c24` | — |
| head `0x00107f08` | `0x00100210` (startup flow) | — |

Entries confirmed by prologue disassembly (`addiu sp,sp,-0x80` after
each preceding `jr ra`). Control scans: the four tail targets
(`0x001ca6f8/740/778/7b0`) have zero direct sites (only `j` tails and
one triplicated module-overlap comment).

## The feeders are indirect-only (Confirmed — whole-image scan)

- `jal` to F1–F4 entries: zero hits in all text bytes (covers the
  ~0.5% the translator rejects, closing slice 24's gap hypothesis).
- F1–F4 and sibling addresses as 4-aligned data words: zero hits in
  the text record and zero in the data record (`0x00617A80` +779,132).
- So the feeder subtree is reachable only via runtime-computed
  pointers — the signature of event/peripheral-driven callbacks, like
  the flag dispatcher's own `jalr` table.

## The four wrappers serve printer channels (High confidence)

Stop-time string dumps at the F-functions' references:

- `0x006923E0` (F1's): `MPhotoRendererFace`, then `printout`,
  `cleaning`, `nozzleCheck`, `rend…`.
- `0x00693F60` (tail-name): `busy`, `no_printer`, `not connected`,
  `E-10x`, `PM-G7000` (printer-manager error tags).
- `0x0019A630` (sib2's arg) is code, not a name — no claim from it.

F1 calls sib1 unconditionally; sibs 2–4 are guarded (`[s0+0xEC]`,
`[[s0+0xA0]+0x3C]`) and can return 0 without touching the chain. All
four tails funnel into `0x001ca7b0` (mode in a3) / `0x001ca878`
(heap + string-table init via `0x005c1d18/1d20/1d30`). Channel-to-sib
mapping stays Hypothesis; the printer domain does not.

Chain-integrity check (Confirmed): `0x001c9468` runs branch-free from
its `0x001cb1d0` call through struct zeroing into `jal 0x00101c50`,
so flag 0 proves no sibling ever drove the chain to the one-shot.
Whether sibs 2–4 (or sib1 against a preset object) executed their
early-return paths is Unknown — and load-irrelevant, since neither
path builds anything.

## Main's wait, precisely cut (Confirmed)

- Main's node `[0x006186F8]` = `0x0081ED48` (its live s0); node
  `[+0x64]` = `0x00659E80`; callback entry `[0x00659EA4]` =
  **`0x001097E8`** — main's own sleep-chain head. Its wake path
  re-invokes its own chain, no wrapper involved.
- Main sleeps in `0x001097e8` while `[node+0x5C] != 0`
  (`0x00109804` test → `0x00109810` sleep); the producer side is the
  flag poster `0x001092d0` (sets `[0x006207F4]`, runs the table
  callback). Stop-time flag = 0: posted, then consumed.
- The poster's only caller is `0x00108bf8:0x00108c24`, whose chain
  head `0x00107f08` is called exactly once from startup
  (`0x00100210`, then `j 0x005a3140`, never returns). So main's first
  wake came from boot itself; the second post it now sleeps for has
  no periodic source in the parked machine (final-leg mixes: all
  `0x100`, zero SIF/RPC/animated traffic).

## Reinterpretation (Hypothesis — for slice 26 to test)

Slice 22/24 called `0x00587xxx` the frame dispatcher. Its creation
chain hangs *solely* off printer-channel init — the only direct-call
path to it anywhere in the image. It may therefore be a print-render
job pool (wake-by-table, join, signal: a spooler shape), in which case
there is no "per-frame heartbeat" to trace and M33's presentational
work correctly waits on M35-class peripheral events (USB printer
attach/print request), exactly as pad input does. Test: identify the
10-way switch's case handlers (`0x00589678` et al.) — render verbs
would confirm, engine-frame verbs would refute.

## Verification

- Every caller claim is a whole-text byte Hitachi, not a translated-only
  grep: single-site counts above are exact over all 5.3M text bytes.
- Dumps: flag `0`, node words, callback `0x001097E8`, printer strings —
  all from deterministic 2,000-service legs; five scratch logs plus the
  layout pickle deleted after extraction.
- Product code untouched (`git status`: only this doc plus the journal
  appendix); no instruments written, none to remove; no binary relink
  needed. Gates left for review.

Next (recommended): test the print-pool hypothesis via the switch cases
(`0x00587c08` table at `0x006CE970`, handlers `0x00589678`,
`0x00587f88`, `0x00588878`, `0x0058b210`) — one disassembly pass, no
runs. If confirmed, close the heartbeat line and return the
main-unpark question to M32's async-event framing (decision 0023):
the missing second flag post needs originating traffic (IOP/pad/USB)
only milestone work provides.
