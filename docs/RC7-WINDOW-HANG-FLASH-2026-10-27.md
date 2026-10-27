# RC7 owner report: unresponsive game window, desktop flashes and missing exported game logs

Owner report, 2026-10-27. Local investigation only; no GitHub issue, implementation, game launch or user-data mutation. These are open test blockers for rc7 release readiness, not confirmed shared root causes.

## Observations

- Owner reports the game window occasionally becomes unresponsive/white even though gameplay or video continues in the background; in an earlier occurrence it recovered. On the latest occurrence the owner chose Windows' close action. Screenshot shows `Gran Turismo 4 is not responding`, movie imagery and an overlay reading 60 FPS; a snapshot cannot establish the simulation rate throughout the hang.
- Owner reports occasional one-frame desktop flashes. No exact cause, timing, correlation with hang, or verified duplicate is established. GitHub #99 / TASK-127 also describes flashing and crashes on earlier versions, so investigate the relationship, not assume it.
- For the pictured hang, the owner confirms the RetroAchievements account was connected and the opening movie was playing. Owner also recalls a similar bug possibly happening with RetroAchievements disabled before; build, timing and reproduction of that earlier occurrence remain unverified. Do not blame RetroAchievements without an A/B reproduction.
- Installed launcher log and the owner's provided `GT4Recomp-logs-20261027-195224.zip` report original GT4 started 19:50:21 and ended 19:52:18 with code `0xCFFFFFFF`; the owner explicitly closed the unresponsive process, so do not classify that code as an independent spontaneous crash. Windows Application event 1002 at 19:52:17 names `gt4.exe` but does not provide a blocking stack.
- Local session `session-1793141421/game.log` identifies GT4's RetroAchievements set and shows GS process VSync stats through approximately 109.74 seconds with no explicit fatal cause; the coarse renderer telemetry is not a hang dump, and lack of errors does not rule out a problem. Latest session used Vulkan, borderless fullscreen, 3440x1440 and VSync enabled. No claim that the reported 60 FPS represents simulation during the hang.

## Diagnostic export defect confirmed

The provided ZIP included launcher logs and three `gt4-gs.ini` files but no session `game.log`. In the installed session directory all three most recent game logs fail strict UTF-8 decoding (first bad byte at offset 9634 for the relevant log). `launcher/crates/gt4l-tauri/src/log_bundle.rs::collect` uses `fs::read_to_string` and silently omits a file on error, even though the export contract says game logs should be included. This is a separate reproducible collection defect; its precise byte source is not established. Fix must preserve credential redaction and avoid including game data or secrets, and test non-UTF-8 input and a real exported bundle.

## Code evidence and hypotheses

The host window message pump is called from `on_vblank` in `native/window.cpp`; the same callback calls RetroAchievements evaluation/overlay and GS presentation. Any sufficiently long stall in the thread or its synchronous work could leave the window unresponsive, but no sampled stack or trace currently identifies such a stall. Renderer is a separate process, which can help explain a drawn image while the parent window is unresponsive, but the exact behavior must be measured.

The RA overlay uses an owned transparent layered window, ShowWindow/SetWindowPos/UpdateLayeredWindow and follows game-window position. It is a plausible candidate for a controlled A/B investigation of flickering or stalls when RA is enabled, not a proven cause. Fullscreen mode changes and desktop focus, external overlays, graphics driver/compositor and the GS child-window handoff are also candidates. Do not merge fixes or change display/saves based on speculation.

## Suggested next evidence

1. Safely compare original GT4 with RA enabled vs disabled under the same scene, monitor, renderer and settings; if owner can, repeat Spec II and rc6. Note whether movie, gameplay and audio continue, response recovery time, exact timing and whether toggling fullscreen or RTSS/Discord overlay correlates with flashes. Avoid requiring repeated forced closes or exposing credentials.
2. Capture a timestamped hang trace or thread stacks from a reproducible case and inspect main/window, RA work, GS IPC and renderer waits; handle game process and owner saves conservatively. Verify end-to-end responsiveness and any fix in both targets and A/B conditions.
3. Correct and test the log exporter separately, preserving redaction; write an intake issue/task internally only, per owner preference. Keep the user's screenshots, ZIP, logs and game data out of Git.

The owner has confirmed Rich Presence visible in Retail and Spec II but rc7 is not fully approved while these reports and the real RetroAchievements unlock remain under test. No decision to include a fix in a particular release is made here.
