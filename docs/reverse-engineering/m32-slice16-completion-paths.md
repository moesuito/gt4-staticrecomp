# M32, sixteenth slice — every completion path mapped (static)

Date: 2026-10-03. Inputs: the pinned CORE (disassembly) plus the
translated whole-program header (caller inventory). No probes, no runs,
no model change. This slice maps, by code reading, every path that can
retire a delay node — and proves only one of them can wake a worker.

## The unlink is pure list surgery (Confirmed)

`0x005b8120` rewrites prev/next links (plus the head word at
`[0x669308]`) and returns. No signal, no flags, no semaphore, no
descriptor touch. Unlinking alone wakes nobody.

## Four unlink callers, one signaling context (Confirmed)

Direct `jal 0x005b8120` sites in translated code, plus the walk:

1. The delay walk's dispatch (`0x005b8238`): unlink, then the
   dispatcher, then flags bookkeeping. **The only context that
   signals** (below).
2. Re-arm cancel (`0x005b8bdc`, inside the arm path `0x005b8b68`):
   unlinks a stale same-id node before re-arming. No signal.
3. Delete/cancel (`0x005b8588`): unlink, zero idbits+flags, return the
   node to the pool free list. No signal.
4. Update/refresh (`0x005b8828`): unlink after refreshing `acc` (its
   tail past the unlink, presumably re-insert, was not read — open).

Consequence: a cancelled wait leaks its waiter — the semaphore is
never signaled and the worker waits forever with no node. None of the
six suffered this (all five live nodes still linked with flags 3;
all five workers still waiting on their semas), so no cancellation
has touched them.

## The signal endpoint, and who can reach it (Confirmed)

`0x005aef58` wraps one syscall (`jal 0x005adcd0`, the `0x42`-family
signal, with the sema in `a0`) plus `sync`/`ei`. It is reached only
through the dispatcher `0x005b8ed8` (descriptor callback plus
free-list management at `[0x89c340]`), which itself has no direct
callers anywhere — only the walk's indirect `jalr`. So the six
completion semaphores can only ever be signaled via walk dispatch.
(The pump's `jal 0x005ae090` next door funnels through the syscall-stub
table near it; slice 50's "dispatcher" label for that address was
imprecise — the pump's real dispatch is its own table-driven `jalr`.)

## What this rules in and out

- In: the due test + walk dispatch remains the only signaling path in
  the codebase. The slice-14 paradox (band swept silently, poke fired
  below) is therefore a paradox about the walk's *inputs or runs*,
  not about hidden signalers — there are none.
- Out: cancellation as the poke-2/D9 explanation *for the signal*
  (cancels don't signal). Whatever consumed node `0x0088a000` with its
  sema signaled went through dispatch — so the due test passed then,
  with operands this slice could not recover statically.
- Open: the refresh tail of `0x005b8828`, and direct observation of a
  walk run's operands (all dumps are end-states; the test uses live
  values).

Next: slice 17 instruments the reference interpreter with a
pc-triggered trace at `0x005b822c` (temporary): every walk test logs
current, target, COUNT, overflow, and head — settling runs and
operands directly instead of by elimination.

## Verification

- Every claim above quotes disassembled bytes or grep results; no run
  was needed and none was made.
- No product-code change (docs only); full gates run on the final tree
  before commit.
