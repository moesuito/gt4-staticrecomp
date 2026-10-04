# M32, thirty-second slice — service-surface inventory: fully covered, nothing to implement

Date: 2026-10-03. Inputs: the pinned CORE (whole-text syscall
scans over the inflated records), the kernel/tool service tables,
two boot legs (fresh 20k service tally; resume patch-table dump),
and disassembly spot-checks. Read-only throughout: no product
change, no instruments — per the charter, the pivot verdict
replaces an implementation.

Charter: inventory unanswered-but-reachable BIOS services, rank by
reachability, implement the first unit — or, if the surface is
fully covered, prove it and pivot to the jump-table dispatch
state instead.

## Verdict first: fully covered (Confirmed, three independent ways)

1. **Whole-text scan**: every `syscall` word in all 5,339,668 text
   bytes names its number in the immediately preceding instruction
   (`addiu v1,zero,imm`, both signs — 144 distinct numbers plus 41
   negative-immediate sites, zero computed numbers). Of these, 69
   are kernel-registered, 3 tool-side (`0x3D/0x64` boot services,
   `0x100` patch-return), and 13 sit in the game's patched table —
   leaving 76 stub-only numbers with no direct caller anywhere.
2. **Firing set ⊆ covered set**: all 39 services observed across a
   fresh 20k boot check out programmatically against
   kernel ∪ tool ∪ patched — zero uncovered (list in §below).
3. **Whole-program property**: the driver stops loudly on any
   unhandled, unpatched syscall; no recorded leg (fresh to
   95k/243M, resumes to 1980k/243M) ever stopped that way. Nothing
   reachable is unanswered.

## The 13 patched services (Confirmed — stop-time table dump)

`[0x1000 + i*4]` holds token `0x80010000 + i` except: `0x08 →
0x008766C0`; `0x54 → 0x005B9E40`; `0x55–0x59 →
0x80075038…` (KSEG0 game code); `0x5A → 0x005B98D0` (Copy);
`0x5B → 0x80075000`; `0x83 → 0x005B73C8` (FindAddress);
`0xFC–0xFF → 0x80076440/0x800762A0`. Observed firing among them:
`0x5A ×3`, `0x5B ×12`, `0x83 ×2` — all guest-handled, no kernel
handler needed by design.

## OSD units: covered, list is stale (Confirmed)

`0x4A/0x4B` (ConfigParam) and `0x6E/0x6F` (ConfigParam2) are all
registered with unit tests (`ee_kernel` OSD blocks). The AGENTS.md
"OSD configuration services" line predates M30 slices 6/8
(decisions 0008/0010) and was never retired — recording that here
so it stops resurfacing.

## Jump-table dispatch state: complete, remainder is perf (Confirmed)

M28 delivers the semantics (per-module entry tables, `jalr` link
handling, computed-`jr` dispatch, unknown-target boundary), all
differential-verified. The only open line ("computed `jr` into
local blocks", old STATUS Performance item) is an optimization —
fewer bridge round-trips — explicitly out of scope per this
slice's "do not implement for its own sake" rule.

## Method notes (for the next inventory)

- A naive "sites per service" count lies: 130+ of the hits are the
  stub tables themselves (SDK band `0x005AD880–0x005AE130` plus the
  game's `0x005B73xx–0x005B9Axx` copies). Count only non-stub
  sites, then confirm each by disassembly — one scan generation
  falsely attributed `0x5A` wrappers to service `0x90` (no such
  service sites exist; strict adjacent-pair scan closed it).
- v1 immediates cover both signs; nothing computes a service
  number at runtime (41/41 non-`addiu` predecessors are the
  negative-immediate form).

## Grades

- Confirmed: the three coverage proofs above (byte scans, table
  dump, firing-set program check, OSD registration+tests).
- High confidence: no reachable gap exists anywhere in the
  service surface (three independent evidences converge).
- Unknown: none load-bearing for this slice (what future game
  phases may call is bounded by the same scan — rerun it then).

## Verification and hygiene

- No code touched, no instruments written, no rebuild needed.
- Scratch deleted: 3 run logs + the `.cmd` runner.
- Gates unaffected (nothing to gate); review + commit per rules.

Next (recommended): with the service surface closed and the
tripwire armed, the main line's remaining honest work is
milestone event traffic per decision 0023 (IOP/pad/USB) — the day
the census moves, the parked waiters' announcers resolve one by
one, each a specified slice like 26/30.
