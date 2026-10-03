# M33 recon, first slice — the pipe is dry (slice 23)

Date: 2026-10-03. Inputs: the pinned CORE and ISO; a temporary transfer
log in the DMA channel write path (since removed) over a 12k leg from
`ckpt-1980k.bin`, plus a 400-service boot control. No model change. The
binary answer slice 22 asked for: the engine emits nothing.

## Zero transfer starts in 12,000 services (Confirmed)

Every GIF/VIF/SIF/SPR transfer the game starts must write its CHCR
with STR set through `DmaChannel::write_register` — the temporary log
there recorded **zero** starts while the leg ran its usual shape
(limit-hit, lockstep). All six channel controls read idle at every
stop (STR clear; SIF0's stuck bit is the model's own handshake write,
known since slice 4). No MADR/QWC/TADR was ever programmed: no source,
no count, no packets. The pipe is dry — confirmed deep park, not a
headless-but-alive engine.

## Control and caveats (Confirmed / noted)

- A 400-service fresh-boot leg also logged zero starts. That control
  is weak by itself (early boot leans on syscalls), but it rules out a
  broken probe only insofar as the 12k result already stands on the
  CHCR-stop reads: two independent evidences, same answer.
- SIF caveat (noted, moot here): SIF transfers issued via the
  `SifSetDma` syscall stay inside the model and would bypass a
  register-level log — but final legs issue no SIF syscalls at all
  (service mix tallied: all `0x100`), so nothing hides there either.

## What this closes and opens

- M33's premise (packets to capture) has no object yet: there is
  nothing to draw because the game sends nothing to draw with. The
  graphics work stays queued behind the parked engine, correctly.
- The engine's silence is now measured, not assumed — and it sharpens
  slice 22's table: the missing heartbeat (who calls the frame
  dispatcher per frame?) is the single upstream cause of both the
  parked workers and the dry pipe.

Next: slice 24 traces the heartbeat — what invokes the frame
dispatcher (`0x00587b30` region) per frame on a running engine, and
which of its links is cut here (VBlank tick? main loop? SIF
completion?).

## Verification

- Probe instrument removed (grep-clean); product code untouched.
- Scratch logs deleted after tallying; full gates run on the final
  tree before commit.
