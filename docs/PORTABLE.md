# Portable copy

Status: portable build TASK-098 and self-starting game TASK-097, release 1.3.0. Code: `scripts/make_portable_zip.py`,
`installer/gt4recomp.nsi`, `launcher/crates/gt4l-core/src/{paths,config,launch}.rs`, `native/standalone.cpp`,
`native/game_main.cpp`.

The portable release asset, **`GT4Recomp-<version>-portable.zip`**, is the same launcher and game programs as the
installer, in a folder that carries its own data. Unzip it somewhere you can write to, start **`GT4Recomp.exe`**, and
the whole folder can be moved to another drive or PC. A copy inside `C:\Program Files` cannot save its data and the
launcher stops with a message that says to move the folder. The zip carries a `PORTABLE.txt` note with the short
version of this page.

| Path | Holds | Updated by an update |
|---|---|---|
| `GT4Recomp.exe`, `SDL3.dll`, `NOTICE.md`, `LICENSE.txt` | the launcher | replaced |
| `game\` | `gt4.exe`, `gt4-spec2.exe`, the runtime DLLs, `shaders\`, the licences | replaced |
| `user\` | your settings (`config\`), `controls.toml`, saves, replays, card backups | never touched |
| `data\` | the link to your disc image (the image is not copied), the prepared game files, the renderer, caches, sessions and logs | never touched |

## Requirements: Windows, a Vulkan card, and WebView2

The [README requirements](../README.md#requirements) apply (Windows 10 or 11 64-bit, SSE4.1, a Vulkan 1.2 card for
CoreGS 2.0). The portable copy adds one requirement the installer takes care of by itself:

**The Microsoft Edge WebView2 Runtime.** The launcher's window — Home, Settings, every page it shows — is a web page
drawn by WebView2 (`launcher/crates/gt4l-platform/src/webview2.rs`); without the runtime the window cannot open, and
the launcher shows a native message with a download link before any window exists. The Evergreen runtime ships with
Windows 11 and the vast majority of Windows 10 PCs already have it, but Microsoft does not guarantee it on either
("some devices might not have the Runtime pre-installed").

- **Installer (`GT4Recomp-Setup-<version>.exe`):** setup reads the runtime's version from the registry and, when it is
  missing, runs Microsoft's Evergreen bootstrapper silently (`installer/gt4recomp.nsi`). That needs an internet
  connection.
- **Portable copy:** nothing is installed and nothing is downloaded. When the launcher says WebView2 is missing,
  install the Evergreen runtime from Microsoft's page —
  [Download Microsoft Edge WebView2](https://developer.microsoft.com/microsoft-edge/webview2/consumer/) — and start
  `GT4Recomp.exe` again. The message's Yes button opens Microsoft's WebView2 download in the browser.

## Settings: the launcher writes them, the launcher reads them

Settings are changed in the launcher (Settings, the game pages, Controls). The files are not an interface: the launcher
owns them.

| File | Written by | Read by |
|---|---|---|
| `user\config\settings.toml` | the launcher, on every change | the launcher only |
| `user\controls.toml` | the launcher's Controls page | the launcher; the game as `GT4_CONTROLS_FILE` |
| `user\config\game.ini` | the launcher, on every change and before every Play | the game, on a direct start only |
| `user\config\gt4-gs.ini` | the launcher, when GSdx is installed | the GSdx process, on a direct start; a Play writes its own copy into the session folder |

At Play the launcher passes the settings to the game program itself: the arguments (`<elf> --ntsc --disc <iso>
--frames 0 --memcard <card> --misses <session>\misses.txt --window`) and the `GT4_*` environment variables of the
session (`launcher/crates/gt4l-core/src/launch.rs`). A Play always starts the game that way, so the copies in
`user\config` are not read at Play: the launcher writes them so that a direct start gets the same choices. Nothing in
`settings.toml` reaches the game any other way, and the launcher rewrites those files, so edits made to them by hand
are lost.

## Starting the game program directly

The launcher is the supported way to play: it prepares the game files, the session and the memory card, and writes the
log. `game\gt4.exe` (and `game\gt4-spec2.exe`) also start alone, in two ways that behave differently
(`native/standalone.cpp`, `native/game_main.cpp`):

- **Standalone start — no ELF path argument** (a double-click, or only `--` options such as `gt4.exe --fullscreen`):
  the game finds its files from its own folder, reads `game.ini`, `gt4-gs.ini` and `controls.toml` from the `user`
  folder above, and fills the rest in with built-in defaults (GSdx, 480p, the 16:9 hold, automatic ultrawide,
  fullscreen, vsync). A missing piece (prepared files, renderer) shows a native message. Because the launcher rewrites
  those files, a standalone start follows the launcher's choices.
- **Explicit start — an ELF path as the first argument** (`gt4.exe <...>\SCUS_973.28 --ntsc --disc <iso> ...`, the
  command line the launcher itself builds): the game uses exactly the arguments and the environment it is given and
  reads none of those files by itself.

So a `.bat` that repeats the launcher's command line skips every launcher setting: what the script does not pass falls
back to the game's built-in defaults, and the choices in `settings.toml` never arrive. To keep them, start
`GT4Recomp.exe` (from a shortcut or a `.bat` that calls it), or start `gt4.exe` with no arguments.
