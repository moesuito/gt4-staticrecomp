# Texture recon — .gpb container, runtime binder, candidates
\nDate: 2026-10-03. Source: parallel scout (read-only), reviewed and
spot-verified by the owner-agent ([V] = re-checked today: ELF string
offsets/counts via byte search of the 6,123,004-byte analysis ELF,
disassembly via `gt4disasm private/fingerprint-check/CORE.GT4`;
[S] = scout-reported, provisional). No product change.
\n## .gpb container vocabulary [V strings, S context]
\n- Format + error strings sit side by side in the ELF [V: offsets and
  single counts re-found exactly]: `%s/%s/%s.gpb` (file `0x598AA0`),
  `doesn't exist in GPB` (file `0x598ABB`), i.e. the loader builds
  `dir/sub/name.gpb` and looks records up **by name** inside it —
  indexed container (High [S for the loader-function mapping at
  `0x213E70` via `sprintf 0x0057DA20` + open `0x0048F320`]).
- API strings [V: present with matching counts]: `loadGpb` x5,
  `existGpbBinary` x2, plus `display.gpb` (`/race_display/%s/%s/`
  family), `getIconTexture` x1, `image/00no_texture.png` x1.
- The game sees the volumes as `/gt4.vol` + `/gt4l1.vol` [V: both
  re-found at file `0x58CB98`/`0x58CBA8`].
\n## Runtime texture binder [V shape]
\n- `0x00453E80` re-disassembled: it calls the 2D lookup
  `jal 0x00454EF8`, and on NULL loads `0x006AA9A0`
  (`lui a0,0x6b` + `addiu -0x5660`) — the `no texture. tag = %d,
  blurLevel = %d, texnum = %d` string [V: re-found at file
  `0x5AB9A0`, x1] — then calls the printf-like `0x0057D9C0`.
  Textures live in a runtime table keyed by
  (tag, blurLevel[, texnum]) with a named fallback — the .gpb→table
  parse sits elsewhere (Unknown).
- PNG/TGA = photo path, not texture format (High [S context, V
  strings]): `image/%s.png` + `00no_texture.png` fallback in a
  sprintf→open→retry function; `.tga` only near
  `copy_photo/printer`; zero `TIM2`/`.tm2`/`TEX0`/`TEX1`/`PSM` hits
  (absence inconclusive alone — but the tag/table model above says
  proprietary format, High).
- `clut` x1 [V: file `0x59A210`] beside font/text symbols —
  palettes belong to the font path first (kept from the roadmap).
\n## .gpb candidates [S — not independently re-resolved]
\n- 64 MB window scan (XOR-0xFF): 102 `.gpb` names; BFS resolved exact
  paths for 4 menu files (`PopupShell`, `LaunchRoot`, `CarRoot`,
  `Message` — entries `@0xA1D4…@0xAADC`, all `value=16`).
- `value=16` on every menu .gpb ⇒ for .gpb, `value` is NOT a byte
  size (High): flags/version/kind TBD; payload sizes/offsets wait on
  the record grammar (slice-19 open problem).
- Easier-first alternative (High): small `menu/*.pmb|*.strb` UI panels
  (`pause1.pmb` 14,720 B + `pause2.pmb` as control pair) whose
  `value`s look like real sizes — try before .gpb if the grammar
  stalls.
- Probable .gpb record shape (Hypothesis): header with
  name→{offset, packed} index (the GPB error proves named lookup),
  texture records keyed `{tag, blurLevel, texnum?, dims, format,
  pixel offset, optional CLUT}` — dims/format/CLUT still anchorless
  (Unknown).
\n## Standing pixel-format question
\n- No PSM-like constants in strings; TEX0/TEX1 values would be
  numeric in code. Optional next static step (non-blocking — the
  format will come from the .gpb records once the grammar opens):
  directed disassembly of the GS setup chute at `0x4568F0`.
\n## Verification (owner-agent)
\n- All 11 string offsets/counts above re-found byte-exact in the ELF
  (incl. `MPhotoRendererFace` x0 — third confirmation it is
  live-memory-only, per the standing lesson).
- `0x00453E80` shape re-read: `jal 0x00454EF8`, NULL path builds
  `0x006AA9A0`, `jal 0x0057D9C0` — matches the report.
- Product code untouched; scout scratch owned by the scout.
