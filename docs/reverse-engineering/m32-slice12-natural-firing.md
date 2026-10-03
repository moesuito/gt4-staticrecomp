# M32, twelfth slice — the natural firing: thread 3's full lifecycle

Date: 2026-10-03. Inputs: the pinned CORE and ISO; legs D7–D9 from
`ckpt-880k.bin` (all limit-hit, all saved: `ckpt-980k/1080k/1180k.bin`)
plus a full service-mix tally of the firing leg. No probes, no model
change. The first naturally-matured delay fired, and its worker ran a
complete lifecycle.

## The firing signature, natural (Confirmed)

Mid-D9, exactly as predicted since slice 7: node `0x0088a000`
consumed one-shot (`prev → 0x0088a0c0`, idbits and flags → 0;
descriptor link cleared; `0x00889f80` the new tail), COMP reprogrammed
`0xfffffe40 → 0x240`, COUNT re-armed by game code (`→ 0x382c9540`).
Thread 3 went `wait 2/11482435` → running → `wait 1/0` asleep at the
SleepThread stub. Saved post-fire in `ckpt-1180k.bin`.

## The worker's lifecycle, resolved (Confirmed — service mix)

D9's 100,000 services: 99,977 × `0x100`, 19 × `0x2f` (GetThreadId),
2 × `0x32` (SleepThread), 1 × `-0x43` (iSignalSema), 1 × `0x41`
(DeleteSema). The story reads end to end: the dispatcher fired and
signaled the completion sema; thread 3 woke, ran its worker (repeated
self-identification, finding no work); deleted its one-shot sema (the
wrapper's post-wait cleanup); slept. A closed loop — sterile: no RPC,
no pad, no onward signals, no new frontier from thread 3.

## Mechanism note (Confirmed — code + observation)

The model redispatches after handlers when the interrupted thread is
not running (`deferred_return`: best-ready-thread dispatch). That is
how thread 3 ran post-fire — and it corrects slice 3's note: my probe
there signaled outside any handler, so no redispatch followed; inside
handler context the model dispatches on its own.

## Outlook (Hypothesis, measured)

Five nodes stand, all epoch-scale targets with current collapsed
post-wrap: ~3 legs per firing at the live rate, so the pattern likely
repeats five times (each firing retires one waiter into a sleeper).
Two live questions for slice 13: whether later workers produce anything
(thread 3 was sterile), and whether the system self-heals — new waits
posted with working delivery get sane, millisecond-scale bases instead
of pre-wrap epoch ones, and thread 3 (now sleep-waiting, still
wakeable via thread 2's ring) may yet be recalled.

## Verification

- D9 replayed deterministically for the tally (100,128 calls both
  runs); saves need clean Syscall stops and got them every leg.
- Scratch log deleted after tallying; no product-code change (binary
  is slice 10's); gates re-run on the final tree before commit.
