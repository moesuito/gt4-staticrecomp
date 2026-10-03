# Asset viewer roadmap (offline GT4.VOL viewer)

Date: 2026-10-03. Source: parallel recon scout (read-only; report
reviewed and spot-verified by the owner-agent — verification notes
inline as [V] verified / [S] scout-reported, not yet independently
checked). Nothing here changes the boot line; the viewer starts as a
consumer of recon plus, where static analysis stalls, of
instrumented-boot dumps from the main line.

## What the formats are ([V] = re-checked today)

- No TIM2 anywhere: zero `TIM2`/`.tm2` hits in the 6,123,004-byte
  analysis ELF [V]. Textures revolve around `Tex1` objects and the
  `mImage`/`mImagePS2` class (`imageWidth/imageHeight/imageScale`,
  `allocateImageBuffer`, `no texture. tag = %d...` at `0x5AB9A0`) [V:
  strings `Tex1` x1, `mImage` x6 present].
- `clut` appears once, next to font/text symbols (`0x59A210`) [V]:
  palettes belong to the font path first, not necessarily textures.
- PNG/TGA strings exist (`image/%s.png`, `00no_texture.png`,
  `SCEI_copy_photo.tga`) [V: `image/%s.png` x1] but sit in
  photo/printer/panel context — fallback and photo formats, not
  evidence about on-disc textures [S for the context call].
- Fonts: closed vocabulary (`menu.fnt`, `system.fnt`, `tw_system.fnt`,
  `kr_system.fnt`, `/font/`, `LoadKanjiFont`, `mTextFace`) [V:
  `menu.fnt` x1, `system.fnt` x3, `LoadKanjiFont` x1]. `FT01` is the
  in-memory object (slice 45), with zero hits in ELF and sampled disc
  bytes alike [V: `FT01` x0 both].
- On-disc bytes are packed/encrypted, not raw pixels [S, corroborated]:
  the font's PRTS blocks reportedly open with `C5 EE F7 FF` plus
  high-entropy bytes, no `FT01`, no valid zlib.

## Volume map ([V] where re-checked)

- Outer `GT4.VOL;1` (extent 105879) reads magic `0xACB990AD`, ver
  `0x00020002` at its extent [V]; nested v3.1 archive at file offset
  `0x10AC800` reads the same magic with ver `0x00030001` [V].
- v3.1 page table fully re-verified [V]: `page[i] = enc[i] ^
  ((i+1) * 0x14AC327A)`, 179 pages, offsets `0x6A0 → 0x2A406`
  monotonically increasing, table at `+0x40`, 32 zero bytes at `+0x20`.
- Page payloads stay closed [S, accepted]: raw/xor/zlib sweeps found
  nothing — extra cipher or per-page keys (Unknown).
- `mv0011..mv0042` are sibling entries (children of `mpeg/gt4`, 12-byte
  stride from `0x4FAC`), each carrying its own `{count, value}` [S].
  `gtloading.img` (`@0x504`, 8,608 bytes) shows a rising offset table
  at `@0x6B4` and references to sibling files [S] — Hypothesis:
  playlist/multipart `.img`.

## Shopping list (scout proposal, kept)

1. Reuse `Gt4Volume` as the index; nothing to do there.
2. Break the v3.1 page cipher: (a) fetch GT4FS sources out-of-repo and
   read `TocHeader`/keys; (b) known-answer attack via the game's own
   reader near `RoFS` strings; (c) instrumented boot dumping already-
   decrypted pages (main line helps here).
3. First targets: `advertise/gtloading.img` (8,608 B) and
   `font/jis2uni.dat` (36,672 B, expect readable bytes — good oracle).
4. Fonts (`menu.fnt` 44 KB → `system.fnt` 432 KB): render glyphs as images.
5. Textures via `.gpb` (163 files; `Tex1`/`mImagePS2` likely inside).
6. Skip: `.ads`/`.mv00xx` (movies), `.ico`, `c00xx` (Unknown).

## Standing questions for the viewer track

- Page-payload cipher (Unknown) — the single gate for everything small.
  Static side exhausted (grid over xor/inflate/skips/pages: zero hits);
  the reader itself is mapped (`0x4B38A0` dual-magic check, `0x4B3938`
  keyed table read, `0x4B36E0` xor55 loop, `0x4B39B0` page fetch with
  virtual inflate — all owner-verified in disassembly). Decisive
  experiment specified and queued for the main line: Hook A (log
  `0x4B36E0` buffers) then Hook B (log `0x4B39B0` in/out pairs).
  Full evidence: `docs/reverse-engineering/asset-page-cipher-static.md`.
- Font entry addresses/sizes (`@0x1308` etc.) [S] — re-verify when the
  viewer first opens the fonts table.
- Whether PRTS-block transform (`FFF7EEC5`...) equals the page cipher
  or a second layer (Unknown).
- Pixel format (new standing answer, High): travels **as data**, not
  as code immediates — the GS chute is indirect end to end
  (`docs/reverse-engineering/asset-gs-chute-indirect.md`). The packet
  emitters are now mapped too (`0x4A4CA8` upload with BITBLTBUF,
  `0x499508` NLOOP=6 context — `docs/reverse-engineering/asset-gif-emitter-hunt.md`):
  the static road is the .gpb record grammar; the decisive
  experiments are main-line packet dumps (128 B context at
  `0x4995D4`, 80 B upload at `0x4A4CD4`).
