# M33, slice 26 — the switch case handlers: domain-neutral plumbing, verdict open

Date: 2026-10-03. Inputs: the pinned CORE (whole-text byte scans over
the inflated records), `gt4disasm` reads, and the whole-program
translation (call/string inventories per handler). Disassembly only:
no runs, no instruments, no model or product-code change. Docs only.

Charter: name what the 10-way switch at `0x00587c08` (table at
`0x006CE970`) dispatches to — render/print verbs CONFIRM the
print-pool reframing, engine-frame verbs REFUTE it.

## Verdict first: neither — the handlers are domain-neutral (Confirmed)

The confirmation test fails (zero print verbs, strings, USB, or
spooler structures anywhere in the complex) and the refutation test
fails equally (zero VBlank, DMA/GIF, or scene-graph verbs). What the
cases actually contain is generic engine job plumbing: request
validation, kernel-trap service calls, thread sync and priority, cache
flush, MMI copies. The print-pool hypothesis is therefore **not
confirmed and not refuted by contents**; what stands is the narrowed
claim: a generic job pool whose only observed first-user is printer
init. The heartbeat question stays open regardless.

## Exact case list (Confirmed — table read from the file-backed record)

`0x006CE970` lies inside the data record (`0x00617A80` +779,132), so
its initial contents are pinned image bytes, read directly:

| cases | target | body |
|---|---|---|
| 0–3 | `0x00587c50` | inline: `jal 0x00589678`, priority set, `jal 0x00587f88`, `jal 0x00588878` |
| 4 | `0x00587ccc` | `jal 0x0058b210`, then `jal 0x00587f88` |
| 5–9 | `0x00587d0c` | `jal 0x0058b210`, 0x40-byte unaligned struct copy to the frame, `jal 0x0057f260`, … |

The `jr` at `0x00587c48` indexes `[0x006CE970 + kind*4]` (kind =
request byte, `< 10`, else `0x00587f48` reject); `table[0]` points at
the fallthrough, which is why case 0 reads as inline code.

## Per-handler domains (Confirmed — calls, services, address refs)

- `0x00589678`: validates the request kind, error `0x8105902c`,
  calls `0x00588960` / `0x00588b10` / `0x00588f20` / `0x00589128` /
  `0x005b49d8` / `0x005b9548` (an `0x8107xxxx` error-code mapper),
  plus DIntr/EIntr. One break-trap (`0x0058b1c8`). No strings, no
  device regs. Domain: request validation + engine-service fan-out.
- `0x00587f88`: the big executor (0xd0 frame, `sq` saves): checks
  `[s3+0xA]`, error `0x8105902f`, six break-trap entries
  (`0x0058b1d0/268/278/290/2a8/2b0`), `ChangeThreadPriority`,
  `FlushCache` (`0x005adf20`, service `0x64`), MMI strlen
  (`0x0057f260` ×4), MMI copy (`0x005a4724`), `0x00589a70`,
  one `lui 0x6d` building `0x006CE998` (a control struct 0x28 past
  the jump table) for `0x005b0750`. Domain: guarded execution with
  cache discipline — no print, no frame verbs.
- `0x00588878`: checks `[s0+0xA]` bit/mode fields, error
  `0x00589035`, two break-traps, `0x00589a70`, DIntr/EIntr.
  Domain: mode-gated trap fan-out.
- `0x0058b210` (and the `0x0058b1c0+` run): a `break 0x1` table with
  per-entry codes `0x09`–`0x2D+` — the engine's private break-trap
  service layer (a0 carries the request; the code selects the op).
- Supporting cast: `0x005a48d8` (MMI vector copy), `0x005b0750`
  (arg-saving DIntr-guarded dispatcher via `0x005B05C0`),
  `0x005b49d8` (big-frame, reaches the `0x005b27c8` thunk region).

Whole-text `jal` scans prove the executors are called **only** from
inside `0x00587c50–0x00587f28` (2/3/3/2 sites respectively): the
complex is self-contained. Service inventory over dispatcher, loop,
switch and handlers: interrupt-gate check, GetThreadId, Poll/Wait/
SignalSema, WakeupThread, ChangeThreadPriority, FlushCache — plus the
break-traps. No Sleep, no SIF/RPC, no USB, no DMA, no GS.

## Address-reference sweep (Confirmed)

`sweep of lui bases 0x69/0x6d/0x88/0x12/0x70` over the dispatcher, the
loop, the switch and all three case handlers finds exactly three
materialized addresses in the whole complex: the job global
`0x0087E180`, the jump table `0x006CE970`, and the control struct
`0x006CE998`. No strings, no peripherals, no graphics registers.
A pool that touches nothing but its own control words cannot be typed
by contents — this is the load-bearing negative result of the slice.

## Grades and limits

- Confirmed: case list (image bytes), per-handler call/service lists
  (disassembly + whole-translation inventory), self-containment
  (whole-text `jal` counts), the three-address sweep.
- High confidence: the pool is generic engine infrastructure, lazily
  built by its first user — printer init is the only *observed*
  first-user, not necessarily the *intended* one.
- Hypothesis (carried, narrowed): print-render pool vs frame pool —
  undecidable from contents; needs the runtime waker.
- Unknown: who writes `[0x0087E180+0x10]` and wakes the loop thread;
  what the break-trap codes `0x09–0x2D` each do; which workers the
  dispatcher's two WakeupThreads target.

## Verification

- No runs, no instruments, no binary or product-code contact at all.
- Table bytes re-derived from the pinned CORE independently of the
  translator (parse recipe in slice 25's doc); case bodies quoted by
  guest address; the single `lui 0x6d` verified at `0x005880d0`.
- Gates left for review (no commit per slice rules).

Next (recommended): stop typing the pool from contents. Run one short
leg with a temporary write-watch on the flag word `[0x006207F4]` (and,
in the same leg, on `[0x0087E180+0x10]`): any poster at all proves a
rare source exists and names its writer pc; continued silence proves
the park is total and returns the main-unpark question to M32's
async-event framing (decision 0023), where originating IOP/pad/USB
traffic — milestone work — is already the ranked answer.
