# TASK-123: native Driving Force Pro wheel research for 1.3.1

## Assignment and limits

You are the owner-run OpenCode research agent. Research and prepare an implementation plan for
TASK-018; do not implement it. The owner is Joao Alano. Speak plain Brazilian Portuguese to him;
write repository documents, prompts, comments and reports in en-US.

- Worktree: `C:\Antigravity\gt4-wt-123-dfp-wheel-study`.
- Branch: `task/123-dfp-wheel-study`. Verify its live HEAD/status before starting.
- Preparation base: main `38e51677bdb5a01d309d3efa66cc8bbd8c3c3cd2`; the documentation-preparation
  commit will follow it. Record the exact research starting commit, not just this historical base.
- Frozen modern reference: `v1.3.0-rc6`, commit `58322229c0bd63a0652b0550c5ad2154bd907039`.
  Inspect files with `git show <commit>:<path>`; do not move or edit an active modern checkout.
- Exactly one writer: you own only this research worktree. Do not spawn subagents or message agents.
- Assets root, read-only: `C:\Antigravity\gt4-staticrecomp`. Never link its `private/` into the worktree.
- Allowed tracked changes: the research report below, TASK-123/018 records in TASKS, STATUS and
  the current journal. No code, tests, configuration, pins or release-note changes.
- Temporary analysis helpers and results may use a new ignored `private/task123-dfp-research/`
  folder in this worktree. No game data may be committed or put in shared memory.
- No builds, game/installer execution, physical input/output, haptic pulses, benchmarks, owner
  settings/cards/saves access, installation changes, PRs, merges, releases or history rewriting.
  Static analysis of the owner's pinned game inputs is allowed. Request a later bounded probe
  when static evidence cannot answer a question; do not silently start it.

The owner prefers DeepSeek in OpenCode or Gemini for execution. Codex orchestrates and reviews.
Do not start a paid provider, another agent or an unattended job from this brief.

## Owner goal

The virtual guest device is **Logitech Driving Force Pro (DFP)**, with native 900-degree steering
and the game's own wheel force-feedback output. Earlier task text saying Driving Force GT/DFGT
was a naming error. A physical DFGT can still be a host SDL3 device.

The desired paths are:

1. Host wheel/pedals/buttons -> SDL3 -> normalized wheel state -> the DFP device contract the game reads.
2. Original game wheel commands -> protocol/effect translation -> SDL3 haptics -> selected host wheel.

The current PR #101/#109 integration instead maps wheel inputs into DualShock 2 pad state and
adapts gamepad rumble plus a fixed centering spring. It is groundwork, not native DFP support.
Keep ordinary keyboard/gamepad use available, but do not route native wheel driving through pad axes.

Planned release is **1.3.1**, subject to research and owner acceptance. 1.3.0 stays focused on
Spec II and performance. The private 1.2.5 draft is held from publication. Do not assume its wheel
page must be exposed in rc7. Broader roadmap/version rules remain the owner's decision.

## Read first

1. AGENTS.md in full, then TASKS entries 018, 120, 121, 122 and 123 and current STATUS/journal.
2. ai-memory: workspace `default`, project `kaezzey-gt4-staticrecomp`; recent, then query for
   DFP/wheels/PDIUSB/LGDEV. Read full `concepts/current-state`, `_rules/project-rules`,
   `decisions/release-plan-1x`, `decisions/input-layer-sdl3` and the DFP research decision if present.
   Treat memory as history and verify against the live request and repository. If unavailable,
   record that and continue the static research from canonical files.
3. `docs/MENUS-AND-WHEEL.md`, `docs/IOP-MAP.md`, `docs/WHEEL.md`, `docs/INPUT-SDL3.md`,
   `docs/CONTROLS.md`, `docs/PR-100-101-INTEGRATION.md`, `docs/ARCHITECTURE.md` and relevant BUILD sections.
4. Existing implementation: `native/wheel.hpp`, `wheel.cpp`, `wheel_fixture.cpp`, `host_io.*`,
   `pad_mapping.*`, `window.cpp`; launcher wheel model/commands/UI and `sdl_wheels.rs`.
5. Target comparison: rc6's Spec II mapping and IOP documentation/source via the frozen commit.
   Never assume v2.00 addresses, IRX identities or hooks also fit Spec II.

Discover paths with `rg` rather than assuming an old document's file exists. Record renamed/missing
paths. The main checkout is stale and dirty with owner leftovers; never pull/reset/clean it.

## Sources, licenses and clean-room separation

- Prefer our MIT runtime/adapter, pinned donor, game/decomp facts, USB/HID/device documentation
  and official SDL documentation. Record revision/hash and exact location for every source.
- Official PCSX2 configuration docs provide a functional reference: Driving Force Pro rev11.02
  is listed as 900-degree/FFB-capable:
  https://pcsx2.net/docs/configuration/controllers/#wheel-compatibility
  That table does not prove which descriptor/revision either GT4 target accepts.
- Read PCSX2 USB/wheel implementation only if necessary for a specifically stated protocol fact.
  If you do, record the exact revision/files and mark yourself PCSX2-tainted immediately. GPL
  source must never be copied, ported, linked or translated into our MIT executable. Deliver
  protocol/behavior evidence in prose, with provenance; do not disguise derived implementation as
  an independent design. Identify anything that still needs a license-independent specification.
- Never inspect `native/hw_gs/`, TASK-027/090 implementation worktrees, or renderer internals.
  A PCSX2-tainted agent cannot participate in those clean-room implementations later.
- No JIT, no physics/timing changes and no promises of compatibility with every host wheel.

## Research questions

### A. Guest identity and input transport

Map both USA v2.00 and Spec II separately:

- Actual USBD/PDIUSB/LGDEV loading, imports/exports, EE-to-IOP RPCs, buffers, callbacks, threads,
  polling cadence and the point where the game decides that a supported wheel is present.
- DFP descriptor/identity, revision, negotiation and mode/range switching. Distinguish USB
  enumeration from a higher-level service's returned identity. A profile name is not evidence.
- Steering/pedal/button report layout, ranges, precision, neutral values and connection state.
  Find where the game reads that state and selects wheel-specific driving/settings behavior.
- What 900-degree mode means in the guest contract and for host rotation/calibration. Preserve
  source precision until encoding the proven guest report; do not inherit a pad conversion by habit.
- Whether HLE at PDIUSB/LGDEV can preserve all observable behavior, or a USB/OHCI model is required.
  Compare approaches using concrete call sites and contracts, not only expected effort.

### B. Original force-feedback path

- Locate actual game-generated output, its destination and transport. Distinguish native wheel
  effects from pad rumble; identify the effect set actually used by each game target.
- Document command encoding, sign/units, magnitude, duration, gain, effect slots, update/play/stop,
  centering/900-degree mode commands, reset and unknown-command handling where supported by evidence.
- Map proven commands to SDL3 capabilities. Mark missing/approximate translations explicitly;
  unsupported effects must not silently become an unrelated rumble effect.
- Account for persistent effects and concurrent slots, focus loss, device removal/replacement,
  pause/reset, shutdown and a driver that supports only a subset. Forces must stop deterministically;
  merely setting the latest gamepad rumble word to zero is insufficient.
- Explain which work belongs on which host/guest thread, and how ordering and timing stay intact.

### C. Reuse audit

Classify existing code as reuse unchanged / adapt / replace / reserved or unsupported. Cover:
SDL enumeration and identity, auxiliary pedals, capture/calibration, raw precision, profiles,
wheel.toml schema/migration, launcher UI/i18n, device ownership, lifecycle stop logic, pad-state
mapping and rumble-derived effects. Presets are templates, not tested hardware certificates.

Do not assume clutch or direct H-pattern gears become supported because an input exists. Find
whether native GT4/DFP contracts expose them; list real limits rather than inventing features.

## Deliverables and acceptance

Write `docs/research/DFP-WHEEL-STUDY.md` with a short conclusion and these evidence tables:

| Table | Required content |
| --- | --- |
| A. Sources | revision/hash, path/URL, exact line/address/offset, method, license and limits |
| B. Guest path | target, module/call/buffer, identity/report contract, proposed boundary, evidence |
| C. FFB | observed command/effect, payload semantics, SDL mapping, unsupported cases, evidence |
| D. Reuse | file/symbol, disposition, reason, required interface change, regression control |
| E. Implementation packages | dependency order, owned files, pre/postconditions, tests, stop gates |
| F. Validation | expected independent result, automated fixture/probe, owner hardware test, target coverage |
| G. Unknowns | unresolved fact, impact, smallest next experiment, required permission/input |

Use Confirmed / High confidence / Hypothesis / Unknown. Every number, address, compatibility claim
and protocol fact needs a real source location. Use NOT MEASURED or UNKNOWN when evidence is absent.
An agent's own generated table is not an independent source. No invented timings or completion dates.

The staged plan must first prove native recognition and a correct input contract, then the FFB
translation and reusable UI integration. Include negative controls showing that pad mode is not
being mistaken for wheel mode. Specify future tests for effect ordering/stop, malformed commands,
reconnect/ownership, no-wheel gamepad regression, both game targets and coexistence with frame skipping.
Actual 900-degree travel, steering feel and force quality require owner hardware acceptance.
Separate a safe automated fake-output test from a later physical force test.

Update TASK-123, STATUS and journal truthfully, review the diff, commit/push only owned documents:
`git -c user.email=57041838+moesuito@users.noreply.github.com -c user.name="João Alano" commit ...`
Preserve CRLF in TASKS/README and stage
with `git -c core.autocrlf=false add <explicit paths>`.

Return a concise Portuguese handoff: research commit, report path, chosen/proposed boundary with
confidence, reusable components, unresolved blockers, and the first bounded implementation package.
Stop for Codex review and the owner's implementation decision. No implementation or release follows
automatically from completion of this research.
