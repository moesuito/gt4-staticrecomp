# Persistent project instructions

This file is auto-loaded at the start of every agent session. It is the working
agreement for GT4Recomp in this fork and the entry point to persistent project
knowledge. Keep it current whenever the rules change.

## Mission and context

GT4Recomp statically recompiles the pinned Gran Turismo 4 (USA) v2.00 R5900
code ahead of time into C++20 for Windows x86-64, supplying the PS2 services
execution requires. Learning and evidence quality take priority over speed.
This repository (github.com/moesuito/gt4-staticrecomp) is the working fork.
Upstream (github.com/Kaezzey/gt4-staticrecomp) is a read-only reference: we do
not push or open pull requests there.

## Roles and autonomy

- The agent is the engineer and decision maker for technical work: it plans,
  implements, verifies, documents, commits, branches and pushes to `origin`.
- The human owner sets goals and priorities and reviews plain-language
  summaries. Never require code knowledge from the owner.
- Commits, branches and pushes are authorized and expected. Keep `main` green
  and pushed. Do not rewrite pushed history.

## Documentation is part of the work

Assume the chat may be compacted or lost at any moment; nothing important may
live only in chat. Write knowledge into the repository as it is produced, not
at the end of the session.

- `docs/STATUS.md` — read this first in a new session: current state,
  environment, open items, next actions. Update whenever the state changes.
- `docs/journal/YYYY-MM-DD.md` — append-only session log: what was done, what
  was discovered, what failed and why, what comes next.
- `docs/decisions/` — numbered records for choices with trade-offs, continuing
  the existing sequence.
- `docs/reverse-engineering/` — evidence documents per technique or milestone:
  hashes, commands, observed results and limits.
- `docs/lessons/` — worked explanations (EXPLAIN) per milestone. Tracked in
  git in this fork; upstream kept them local.
- `docs/inputs/` — only hash-pinned, distributable metadata; never payload bytes.

A change is only done when its documentation is updated. Before ending a
session: update `docs/STATUS.md`, append the journal entry, commit and push.

## Evidence discipline

- Label claims Confirmed / High confidence / Hypothesis / Unknown.
- Record input hashes, guest addresses or file offsets, tool and version,
  observed state, comparison method and the next experiment
  (see `docs/requirements.md` for the full convention).
- Prefer independent verification (Ghidra, a second tool, a hand-computed
  value). A self-written implementation can share a bug with itself.
- Never guess in code: unsupported instructions and services stop with useful
  context; returning success cannot establish correctness. No silent fallbacks.
- Pin versions and hashes; reject changed inputs.

## Inputs and repository hygiene

- Game payloads, BIOS, extracted files, captures and translated code live only
  under ignored directories (`private/`, `iso/`, `generated/`), never in git.
- Verify inputs against `docs/inputs/` manifests before use; verification never
  updates a manifest.
- Check `git status --short` before every commit; never stage local game data,
  generated output or tooling.

## Code standards

Explicit owner requirement, recorded 2026-09-13:

All code written for this project must be HUMAN READABLE. Always assume a
human will read, review, and learn from it. Never justify obscure or compressed
code by assuming it will not be inspected by a person.

Use descriptive names, straightforward control flow, and clear formatting.
Prefer explicit, understandable steps over clever expressions or unnecessary
abstractions. Explain non-obvious intent and assumptions where they matter.
This requirement also applies to generated C++ and project tooling scripts.

The owner's exception permits assuming that test files and test scripts may
not be read by a human. This does not extend to general project scripts.

Additional standards:

- C++20, MSVC x64, Ninja, CMake 3.24+; keep the build warning-free and green.
- New behavior needs automated tests; extend the existing CTest and Python
  fixtures instead of adding parallel harnesses.
- Keep guest state explicit: fixed-width integers, controlled guest addresses,
  defined wrapping; no host undefined behavior as a stand-in for guest
  semantics.

## Git workflow

- `main` always builds and passes `ctest`; push it to `origin` regularly.
- Work in small verified slices. For multi-step work use a short-lived branch
  (e.g. `m7-control-flow`) and merge to `main` when green.
- Commit style follows the project history: short imperative subject lines with
  the evidence numbers that justify the change. One logical change per commit;
  code, tests and docs travel together.
- Fetch `upstream` occasionally; merging it is a deliberate, documented
  decision, never automatic.
- Push every verified, documented commit to `origin`.

## Quick reference

Live state: `docs/STATUS.md`. As of 2026-10-04:

- M0-M30 slice 50, M32 slices 1–22 + slices 29–34 (0026 live, tripwire armed, poster a confirmed fossil), M33 slices 23–28 (closed), M35 slice 35 (pad surveyed, spec deferred) BUILD/VERIFY complete; decoder covers 349 operations; the
  only unsupported words left in the real code region are two DMA-dependent
  BC0F and two unassigned encodings inside the exception handler (the text's
  trailing 700 words are a data table). The translator handles 99.5% of the
  direct-call targets, and `--all` generates the whole game as one module
  (15,068 functions, 924,991 instructions, 146 MB, MSVC syntax-checked). The
  boundary driver executes translated modules as programs; the BIOS service
  layer models SetupThread, SetupHeap, FlushCache, the thread/semaphore
  scheduler, kernel patches, the timer registers (now ticking at idle), the
  OSD configuration, the DMAC/SIF register banks, the SIF services with a
  model IOP that answers the SIFCMD init handshake and the RPC
  bind/call/version/reset protocol, the GS and extended OSD services, the
  peripheral windows, and the DMA channels (a started transfer completes at
  once and raises its cause); the driver injects interrupts (SIF DMA
  completions, DMA channel completions and, at idleness, VBlank and the
  timer compares with the registered handler chains); the step-by-step
  interpreter bridges the boundaries a module cannot pass. `gt4boot` runs
  the whole game as one module from the ELF entry through the whole init
  chain, the game's thread creation, the SIFCMD handshake, the IOP reset,
  the RPC initialization and the game's runtime threads (the cooperative
  scheduler and the injected interrupts exercised end to end) with the state
  identical to the interpreter (7,573,241 instructions at the 3,000-service
  comparison point; injected handlers run with no nesting and no preemption
  per decision 0013, the version queries answer the game's compatibility
  constants, Deci2Call is accepted and the model IOP holds 80 RPC servers
  per decision 0014, and the register mirror and the liblgdev device sync
  of decision 0015 clear the command-layer spin and the device library's
  trap — so the boot binds the disc subsystem, negotiates the fileio/CDVD
  versions and runs its **device polling round to the 1,000,000-service
  limit**; the **service clock** of decision 0016 then advances the model's
  time base by one millisecond of BUSCLK ticks per handled service (called
  identically by both engines), so the delays expire and the run ends at a
  service boundary with the worker threads ready (1,193,971 module calls,
  32,878,366 interpreted steps); the **disc image** of decision 0017 then
  backs the file service, so the boot walks its IOP module list (SIO2MAN,
  MCMAN, MCSERV, SIO2D, DBCMAN, DS2U_D, LIBSD, USBD, ...) with the disc's
  real sizes (`gt4boot --disc <iso>`); the archive path's reconnaissance of
  decision-free slice 17 then shows the boot at its **movie phase**
  (`/mpeg`) with the game's own PCDV CD path and the GT4.VOL archive's
  header, XOR-0xFF name table and directory tree documented; the **GT4.VOL
  reader** of decision 0018 then parses that archive lazily and with
  validation, verified against the pinned volume (the 22 root categories,
  the `mpeg/gt4` chain and `mv0010`'s 18,874,372 bytes) with the file item
  records recorded as not yet pinned; slice 19 then probes those records
  (97 clean three-word `mv0011`..`mv0107` entries with position and packed
  size, then a mix of kinds; three candidate grammars tested, none closes,
  so no parser shipped — the next slice pins the boundary from the game's
  own consumer); the **CD read service** of decision 0019 then answers the
  game's own driver from the disc image's raw sectors (`{LBA, size, EE
  destination}`), so the boot walks the ISO (LBA 0x10 the volume
  descriptor, LBA 0x105 the root directory) with the external GT4FS
  reference corroborating the archive format family; slice 21 then traces
  that walk (the driver reads the ISO's volume descriptor and root
  directory and stops, and the pinned volume is confirmed as the
  uncompressed 2.2 variant the GT4FS packer also writes); slice 22 then
  finds the disc's **two volumes** (the live cache's blocks prove a second
  ISO9660 volume at logical block 0x1418C0, stored sixteen blocks early in
  the image) and answers the CD driver's volume protocol (RPC 2 registers
  the descriptor with an index-weighted checksum; RPC 4 answers the
  registered volume's size), so the boot mounts both layers, reads the
  inner archives (version 3.1) and stops at an unaligned guest access while
  parsing them; slice 23 then diagnoses that fault (the reference
  interpreter faults at the same address, so it is guest data, not
  translation: a pointer relocation called on a structure at an odd address
  in the engine's static name buffer); slice 24 then finds what the engine
  was building there (a stream of 13-byte records with a cursor counting
  down by 13, whose odd position leaves the static object's data pointer
  odd; the console's same-class object is even, and no file read precedes
  it); slice 25 then names the builder (the engine's sound library at
  0x00462xxx: its init 0x00463000 sets the stream at 0x008475C0 with
  13-byte records and assigns `/sound/roadnoiz.es` to the static object
  with the relocating flag 1, so the object's odd position is the stream's
  position after three records); slice 26 then pins the writer (the assign
  function's own memcpy — the stream holds blobs, not names — and the
  faulting path relocates the destination in place at the odd stream
  position, with 39 bytes copied before it against the console's 32);
  slice 27 then reconstructs the stream's content (13-byte records, the
  first three identical and the fourth different) and shows the assign
  formats its source through 0x0044D740 (arena allocated) before copying —
  the records are formatted objects, not names; slice 28 then maps every
  writer by call site (the assign chain plus the SDK string code reached
  through the patched syscall stubs, 148 writes from pc 0x00100008) and
  records that the watch's argument registers are stale for inner calls;
  slice 29 then proves the register reads are stale on translated code (a
  state-pointer instrument captured zero copies), maps the assign chain's
  formatter (the SDK printf 0x0044D740) and shows the bank assignments
  advance the stream by 13 bytes each — the copied objects are not the
  names; slice 30 then completes the stream's reconstruction (four 13-byte
  records, nothing else — the small fields are not stream offsets) and
  identifies each record as the head of a serialized object the flag-1
  assign relocates in place, faulting on the odd position; slice 31 then
  finds the records' source is the **null pointer** (the assign chain's
  resolver 0x0044D740 returned 0 on a failed parse, so the copies read
  addresses 0x0..0xC — the low memory), which is the root of the fault
  chain; slice 32 then shows the resolver's parse is a handler-registry
  dispatch (the list at 0x006318B0 matches the console's) and the failure
  is the handler's path-prefix state: the live handler carries +0xAC = "/"
  while the model's points its prefix list (+0xF4) at the `/mpeg` global;
  slice 33 then finds the registration code (the game's own early init
  0x00100D30 constructs the handlers, the first with t0 = 0x00617AA8) and
  **corrects slice 32**: the field comparison shows the model's handlers
  match the console's (vtable, +0xAC = "/", the archive bindings), so the
  match chain reaches the layer-0 archive handler and the failure is inside
  its open method 0x004B1730; slice 34 then maps that open's flow (it
  allocates a stream, builds the path from the handler's "/" prefix,
  enqueues the stream to the handler's worker under a condition wait, and
  the result lives in the stream's +0x94; the worker searches a sorted tree
  at the handler's +0x58 — the archive's page tree); slice 35 then rules out
  the stream allocation (its free list at 0x0084B528 is identical to the
  console's) and shows the enqueue is a plain list append (0x0057CB00), so
  the failure is the worker leaving the stream's +0x94 at 0; `--threads`
  prints the kernel's thread table, the handler tables, the DMA/timer state
  and the deferred-call counts after a run, and `--dump ADDRESS LENGTH`
  prints stop-time guest memory (also when the guest faults). Slice 41 then
  reads the handler class's full vtable and its constructor (0x004AD1C8:
  the mutex +0x10, the lists +0x40/+0x4C/+0x58, the worker's condition
  +0x64, the result +0x94 — no thread) and **corrects slices 38–40** with a
  stop-time dump: at 83,782 services the formatter's context has **state 3**,
  the handler's three lists are **empty** (the worker loop 0x004AD8F8 ran,
  drained +0x4C through 0x004AD9D0 and the tree through 0x004ADBB8, and the
  **stream's** completion 0x004AF4A8 set state 3), and the context's result
  +0x94 = 0 — the value the formatter returns; the differential over the
  whole boot to the fault's doorstep (24,114,381 interpreter instructions)
  is **identical** — the lookup work 0x004B0B48 copies +0x94 from the
  search request's +0x10, which 0x004B1F90 never writes. Slice 42 then
  finds the root cause — the game's own **block cache server**
  (sid 0x53545250, "PRTS"; the cache client's bind at 0x00550D00) was not
  modeled, so its zero reply left the open's file object unbuilt — and
  fixes it: `answer_prts_read` (RPC 3, `{LBA, size, flags}` → a fresh
  handle, a cache of at most eight blocks) and `answer_prts_copy` (RPC 4/7,
  `{handle, destination, size}`) serve the blocks from the same image as
  the PCDV reads (decision 0021). The writer of the result is the
  descriptor callback 0x44D540 (second drain → 0x4ADC40 → 0x4AF780 →
  **0x44D6BC `stream+0x94 = the file object`** after the header read and
  its magic check). The boot now runs **past the sound phase** to the step
  limit (3,648,011 services handled where the old run faulted at 83,783);
  the differential at 100,000 services is identical; CTest 35/35
  (`gt4boot_services` pins 90,000 services with the disc, about 22 s).
  Slice 43 then looks past the step budget (now a `gt4boot --steps N` flag,
  CTest 36/36 with `gt4boot_steps`): the 200M stop is mid-copy in the
  per-packet routine 0x0055e6d0, the service mix is stationary and the
  threads are healthy, but only 8 disc reads occur in 1.5M services — and a
  10x run finds the next wall, an **unaligned fault at pc 0x00491798** (a
  move-relocation routine) on 0x009cf08f after 15,010,045 services,
  deterministic across two runs; the direct caller is 0x0048fb94.
  Slice 44 then shows the odd value was never stored (a write watch over
  the fault object: allocator-zeroed, pattern-filled by a transforming
  copy, no disc region matches) — the relocate family derived it by
  walking data as pointer tables — so the lookup designated the wrong
  object: model-fed divergence upstream (high confidence), no model change
  shipped, pinning the divergent lookup is next. CTest 36/36 and Python 73
  (67 run, 6 skip).
  Slice 45 then captures the fatal query itself — **/fonts/system.fnt**
  (14 hook lines in 15M services; the file is in the archive's fonts table
  on both layers) — and completes the mechanism: the even FT01 object's
  `0x30/0x58` fields relocate into self-pointers, walked as offset
  tables, and an entry turned absolute faults exactly at `0x009CF08F`
  (low memory all zeros). Next is the font file's load path. CTest 36/36
  and Python 73 (67 run, 6 skip).
  Slice 46 then traces the font's load (3 late PRTS reads, 186 KB last, no
  fileio) and finds the model re-serving chunk zero for every chunked
  copy-out (the request has no offset): a per-handle cursor fixes it and
  the boot sails past the 15M fault to 41,919,339 services, stopping
  cleanly. CTest 36/36 and Python 73 (67 run, 6 skip).
  Slice 47 then finds no wall through 243,711,723 services (exit 0, ~6x
  past the record): the machine goes fully idle — boundary
  no-runnable-thread at 0x00001604, 245,036 interrupts pending — and the
  14 GB scratch logs get deleted post-evidence. CTest 37/37 and Python 73
  (67 run, 6 skip).
  Slice 48 then proves the stop is event starvation, not deadlock (3,123
  injections, zero unblocks, zero signal/wakeup/release calls; VBlank +
  timer-2 chains effect-free; no cycle among 10 sleepers and 7
  sema-waiters), with the resumed leg reproducing the original ending
  bit-for-bit. CTest 40/40 and Python 73 (67 run, 6 skip).
  Slice 49 then maps every waited semaphore to its subsystem (46 live;
  all 7 waited ones count 0 with one waiter each: SIF/RPC on ancient
  counting sema 11, engine workers on late binary semaphores sharing one
  creator) — no cycle, no in-model signaler — and ranks the missing
  events (async IOP drought, then input, then GS-side). CTest 40/40 and
  Python 73 (67 run, 6 skip).
  Slice 50 then exhausts deliverable stimuli (INTC 0/5 buried and
  effect-free; SSUP unasked; handler inventory without a wakeup path):
  the stop needs originating traffic only milestone work provides. CTest
  40/40 and Python 73 (67 run, 6 skip).
  M32 slice 1 then inventories the 24 bound SIF servers (no PADMAN, so
  M35 waits), maps the arrival path (provably empty pump queue, SIFCMD
  table, thread-2 dispatch loop) and frames the async work in decision
  0023; `--threads` lists SIF sids permanently. M32 slices 2–3 then map
  thread 2's job ring end to end (registers, 512 slots all `{0,3}`,
  control block, sole global handle, SDK creator, op map) and prove the
  wake button with two probe legs (replay re-parks in 2 services, a
  sleeper wake flickers out in 8): stable-parked, needs new input.
  M32 slice 4 then unwinds all six workers to one waiter (one-shot
  wait-then-delete over delay-library work, timer-conditioned, handlers
  paired, binds exonerated): the stall is the never-firing delay
  dispatcher.
  dispatcher.
- Next: **the lessons backlog (slice 40)**:
  four lessons committed and owner-checked; next the
  BIOS-services + bridge/driver lesson (slices 1–2 + decision
  0004). The
  curriculum's remaining units (the OSD configuration services, a
  counting timer with interrupt delivery, the remaining BIOS services
  and the jump-table dispatch) stay listed in `docs/requirements.md`.
- Build (VS Developer PowerShell):
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=cl`
  then `cmake --build build` then `ctest --test-dir build --output-on-failure`.
- Python tests:
  `private/tooling-venv/Scripts/python.exe -m unittest discover -s tests/python`.
- Local inputs: ISO at the repository root; `private/fingerprint-check/CORE.GT4`;
  analysis ELF at `private/reconstructed/SCUS_973.28.elf`.
- Ghidra verification: `private/tooling/jdk-21` and
  `private/tooling/ghidra_12.1.3_PUBLIC`; full command in
  `docs/reverse-engineering/m6-disassembly.md`.

## Subagents

- Use subagents for exploration, research and parallelizable work. They start
  with fresh context: include every relevant path, constraint and expected
  output in the prompt.
- Review findings before acting or committing; never propagate an unverified
  claim.
- Persist knowledge that must survive: if a subagent produced something worth
  keeping, write it into the matching document.
- Only one writer at a time: serialize repository mutations across agents.

## Communication with the owner

- Speak Portuguese with the owner, in plain language, without unexplained
  jargon.
- Lead with what changed and what it means for the project. When a decision
  matters, state options, trade-offs and a recommendation.
- Do not ask the owner to review code; summarize behavior and evidence.
