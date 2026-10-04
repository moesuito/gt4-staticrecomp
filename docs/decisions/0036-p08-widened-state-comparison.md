# Decision 0036: widen the differential to the whole semantic machine (P08)

Date: 2026-10-04. Slice 74. Status: accepted (implemented, green, uncommitted).

## Context

The driver/interpreter differential (`gt4boot --compare-interpreter`,
PLAN.md P08) compared the live EE registers plus one digest over the
main 32 MiB RAM window only. Three semantic areas never entered it:

- the 16 KiB scratchpad (0x70000000) and the GS block (0x12000000),
  both mapped RAM regions with their own guest traffic;
- the kernel tables: inactive threads' saved contexts, semaphores,
  handler registrations with their arguments, the pending queue, the
  deferred-call stack, SIF/RPC servers, the service-clock leftovers
  (accumulator, per-timer remainders), disc handles and block-cache
  cursors;
- the device banks (timers, INTC, DMAC, DMA channels, SIF, VIF/GIF
  windows) as the guest left them.

A divergence confined to any of those passed blind. The slice-69
incident proved the cost: the gate regex passed while the process
exited 1. P08 requires the comparator to cover all of it and to name
the first divergent component.

## Decision

Compare canonical snapshots, field by field, in one fixed order
(live registers, RAM regions, kernel, device banks), reusing the
existing snapshot paths and no parallel infrastructure:

- RAM from `GuestMemory::regions_snapshot` (MMIO windows are skipped
  there by construction), matched by base address.
- Device state from `registers_snapshot`/`register_value` storage
  reads only, never from a guest MMIO read through memory (a guest
  read could carry side effects elsewhere; the snapshot path has none).
- Kernel state from a new `Kernel::describe_kernel_difference`, a
  member that walks exactly what `save_kernel_state` serializes, so
  the two must grow together. Handler chains, the pending queue and
  the deferred stack keep their order (dispatch reads them in order);
  everything else compares by identity (threads/semaphores by id,
  maps by key, regions by base, banks by name, entries by address).
- Excluded from the semantic state by rule: host pointers the boot
  relinks (disc sources, device units, the service table, the DMA
  poll hook), diagnostic metrics (RPC pair/bind telemetry, DMA
  start/payload taps) and incidental container order.

The first difference wins and is printed with both values
(`state differs at ...`); the wrapper lines and the CTest gate
regexes are unchanged. Replies, DMA, JR, the clock and interrupts
are untouched: the comparator observes, it never changes behavior.

## Consequences

- New unit suite `ee_compare` pins the three old blind spots
  (scratchpad-only, semaphore-only, device-register-only changes are
  now named while the old main-RAM view stays equal) plus wakeup,
  handler-argument, queue, clock-leftover, block-cache-cursor and
  order-normalization legs.
- The 90k disc differential stays green under the wider eye
  (boundary, counts and interpreter steps unchanged), as do resume
  verification and the whole CTest/Python suites.
- Keeping `describe_kernel_difference` next to the blob codec is a
  standing sync obligation for every slice that adds kernel state.
