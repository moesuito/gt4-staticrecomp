# Open GitHub issues and pull requests: 2026-10-27 snapshot

Source: `moesuito/gt4-staticrecomp`, queried via `gh issue list` and `gh pr list` on 2026-10-27. This is an intake snapshot, not a code review, compatibility assessment, task plan, merge approval, release commitment or proof of a fix. Issue and PR numbers share GitHub's namespace; do not confuse them with TASK identifiers. Mergeability is GitHub's current comparison against each PR's own base and can change. No GitHub comments, labels, merges or closures were made.

## Pull requests open: 8

Six are from `luanantony` on their fork and target `main`. Their titles and descriptions state the intended fixes; all require independent review, current-base analysis, tests and owner approval. A mergeable status is not a quality or release gate. Current `main` is the 1.2.5 stable program line; rc7 derives from the separately published rc6. Do not assume a direct main-targeted PR is compatible with rc7, or merge unrelated versions wholesale.

| PR | Head at snapshot | Reported scope | Related issue/task | GitHub mergeability |
| --- | --- | --- | --- | --- |
| [#122](https://github.com/moesuito/gt4-staticrecomp/pull/122) | `1594f31a` | Startup diagnostics and vcruntime140_1.dll | #107 / TASK-124 | Conflicting |
| [#121](https://github.com/moesuito/gt4-staticrecomp/pull/121) | `17f39aef` | Forward keyboard messages to GS child for ReShade | #102 / TASK-126 | Conflicting |
| [#120](https://github.com/moesuito/gt4-staticrecomp/pull/120) | `36e6b6a5` | Save-chain capacity for PCSX2 card import | #86 / TASK-104 on 1.3.0 line | Mergeable |
| [#119](https://github.com/moesuito/gt4-staticrecomp/pull/119) | `622cac78` | Silent fallback when no output device | #93 / TASK-129 | Mergeable |
| [#118](https://github.com/moesuito/gt4-staticrecomp/pull/118) | `a7bf111a` | Faint GT Auto audio loop | #104 / TASK-125 | Mergeable |
| [#116](https://github.com/moesuito/gt4-staticrecomp/pull/116) | `130bb48f` | Deleting active save and stale-card return | #98 / TASK-025 | Mergeable |

Two owner-authored PRs remain open:

| PR | Head / base | Status |
| --- | --- | --- |
| [#113](https://github.com/moesuito/gt4-staticrecomp/pull/113) | `15b7bf39`, task/082-render-skip -> task/116-117-flare-perf | Draft, conflicting. Frame skipping deferred by owner for CoreGS 2.0; excluded from rc7. Do not merge as part of this intake. |
| [#110](https://github.com/moesuito/gt4-staticrecomp/pull/110) | `5f0d3d6f`, task/116-117-flare-perf -> main | Conflicting. Published rc6 lineage; a future integration needs separate base, scope and approval. |

## Issues open: 19

Five newer reports below have no comments and no owner response at snapshot. Potential links are for triage only; do not mark duplicates without evidence or assign new task numbers without checking the current registry and owner scope.

| Issue | Report | Mapping / unknowns |
| --- | --- | --- |
| [#129](https://github.com/moesuito/gt4-staticrecomp/issues/129) | Crackling and skipping audio, especially engine, SteamOS / Proton GE | New report; version, hardware, timing and cause unknown. Related symptom to audio/performance reports but not proven duplicate of #93 or #104. No task assignment verified. |
| [#128](https://github.com/moesuito/gt4-staticrecomp/issues/128) | Request European PAL SCES-51719 support; US disc works | New regional-target request. Distinct from TASK-128 (external ISO path); feasibility and scope undecided, no task assignment verified. |
| [#126](https://github.com/moesuito/gt4-staticrecomp/issues/126) | Performance worsened after update | Insufficient build versions, hardware, settings and scene. Potentially related to existing TASK-041/performance reports #97/#112, not proven duplicate. |
| [#123](https://github.com/moesuito/gt4-staticrecomp/issues/123) | Controller cannot reliably focus/change launcher toggles, Steam Deck preview | UI navigation/controller issue. Existing launcher controller navigation should be checked for overlap; no exact task assignment verified. |
| [#115](https://github.com/moesuito/gt4-staticrecomp/issues/115) | Game fails to start, screenshot and log bundle provided | Cause unconfirmed; may be related to TASK-124/#107 but neither screenshot nor attached logs were reviewed here, so do not declare duplicate. |

Previously mapped open issues, preserving their existing tasks; the full historical intake and evidence limitations are in `docs/ISSUE-TRIAGE.md` on main:

| Issue | Existing mapping | Notes |
| --- | --- | --- |
| [#112](https://github.com/moesuito/gt4-staticrecomp/issues/112) | TASK-041 / performance work | iGPU slowdown; D3D11 follow-up reports improvement and remaining stutters. |
| [#107](https://github.com/moesuito/gt4-staticrecomp/issues/107) | TASK-124 / PR #122 | Startup failure; reporter specified rc5 and original GT4. PR claims fix, unreviewed. |
| [#104](https://github.com/moesuito/gt4-staticrecomp/issues/104) | TASK-125 / PR #118 | GT Auto noise continues until restart per reporter. |
| [#102](https://github.com/moesuito/gt4-staticrecomp/issues/102) | TASK-126 / PR #121 | ReShade Home key; reporter identified rc5/fullscreen. |
| [#99](https://github.com/moesuito/gt4-staticrecomp/issues/99) | TASK-127 | Flashing and crashes; cause not established as shared with other startup/graphics reports. |
| [#98](https://github.com/moesuito/gt4-staticrecomp/issues/98) | TASK-025 / related TASK-058 / PR #116 | Save persistence; contributor says a fix exists, not independently reviewed. |
| [#97](https://github.com/moesuito/gt4-staticrecomp/issues/97) | TASK-041 / performance work | Low-end performance; related backlog to #112, not proof of same bottleneck. |
| [#95](https://github.com/moesuito/gt4-staticrecomp/issues/95) | TASK-053, TASK-128 | Texture path / optional external ISO path; decision and separate scope apply. |
| [#93](https://github.com/moesuito/gt4-staticrecomp/issues/93) | TASK-129 / PR #119 | No Windows audio device; do not conflate with issue #129. |
| [#92](https://github.com/moesuito/gt4-staticrecomp/issues/92) | TASK-079/080 and TASK-130 | Aspect and split-screen display. |
| [#89](https://github.com/moesuito/gt4-staticrecomp/issues/89) | TASK-105 on 1.3.0 line | Crash on integrated graphics; reporter verification remains relevant. |
| [#88](https://github.com/moesuito/gt4-staticrecomp/issues/88) | TASK-018 / research TASK-123 | Native DFP/FFB planned separately. |
| [#87](https://github.com/moesuito/gt4-staticrecomp/issues/87) | Existing TASK-057 disposition | Cheats remain not planned. |
| [#86](https://github.com/moesuito/gt4-staticrecomp/issues/86) | TASK-104 on 1.3.0 line / PR #120 | PCSX2 card import; contribution requires review. |

## Suggested next review sequence

Review save integrity (#120 and #116) before risky data changes, then startup/audio (#122 and #119), then ReShade and GT Auto (#121 and #118), subject to owner priority. Each review should establish exact head and base, changed files, existing comments/checks, potential regressions, applicable stable/rc7 integration path, and required real-world testing before any decision. Do not resolve conflicts or merge as part of this inventory.
