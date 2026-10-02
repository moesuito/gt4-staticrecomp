# 0006 — Kernel patches: a synthetic syscall table and guest handlers

Status: implemented 2026-10-02 for the M30 fourth slice
(`docs/reverse-engineering/m30-kernel-patches.md`).

Context: the boot reaches SetSyscall (0x74) in the SDK's
InitTLBFunctions-equivalent at 0x005B7450. The code patches syscall 0x83
(FindAddress) to the game's scan helper 0x005B73C8 and syscall 0x5A (Copy) to
0x005B7390 (the globals at 0x658368 hold exactly these pairs), then locates
the kernel's syscall table by searching the low 512 KiB of RAM (KSEG0
0x80000000..0x80080000) for the handler values it just installed, derives the
table base, and stores it at 0x658360. In this model the host **is** the
kernel, so a patch that rewrites kernel code has no object to rewrite; the
decision is how to represent it without guessing.

Decision:

- **A synthetic syscall table** lives at physical 0x1000 (256 entries) in the
  zero-filled low RAM the search scans. Every entry starts as an opaque
  token in the kernel segment (0x80010000 + number * 4); `SetSyscall` writes
  the guest handler into its number's slot and records it. The game only
  derives the table's address by searching, so the exact location is free.
- **A patched syscall transfers control to the guest handler** with the
  syscall's argument registers: the service table entry for that number is
  replaced by a dispatcher that saves the caller's `ra` and the resume
  address (pc + 4), points `ra` at a return stub, and jumps to the handler.
  The stub (physical 0x1600) issues the model's **private return service
  0x100**, which restores the caller's `ra` and resumes at pc + 4. This
  mirrors the real dispatcher's return through EPC without executing kernel
  code, and works for both wrapped and inline syscalls.
- **GuestMemory gains an explicit KSEG0 alias** (`0x80000000 + physical`
  reads and writes the same bytes), enabled only by the boot tool. The
  default stays strict: nothing outside the region is mapped.
- **Copy (0x5A) is not modeled**: the boot patches it to its own
  implementation before any use. If some path calls it before the patch, the
  syscall stays a boundary.

Alternatives considered:

- **Map and execute the BIOS ROM** for the search and the handlers
  (rejected: the search targets the kernel's RAM tables, and executing BIOS
  code would replace this project's model with the original kernel).
- **Emulate the search predicates natively** (rejected: running the game's
  own helper through the driver is stronger evidence — it verifies the
  patched dispatch, the return, and the aliased reads together).
- **Treat SetSyscall as a no-op** (rejected: the subsequent search would
  fail and the boot would loop, as observed before the stub existed).

Consequences and limits:

- The real kernel also saves and restores the user's caller-saved registers
  across a patched call; the model does not. That is unobservable to
  o32-compliant callers (the handlers preserve `ra` and the callee-saved
  registers themselves) and is recorded as a model choice.
- The private return service 0x100 and the stub address 0x1600 are model
  mechanisms outside the ABI; a game issuing syscall 0x100 directly would be
  intercepted, which the ABI reserves.
- Only the 167 syscall sites in the text were surveyed; ~140 sit in the
  SDK's wrapper cluster around 0x5AD900, the rest in the crt0 and two small
  clusters. Wrapper-style returns are the norm; the stub handles inline
  syscalls identically.
- The kernel's own search helper runs interpreted (it is not a `jal`
  target, so the whole-program module has no entry for it); the gaps stay
  visible in the stats.
