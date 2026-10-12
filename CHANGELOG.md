# Changelog

## Unreleased

- New `scan` subcommand: recursively collects non-ASCII chars from C source
  string literals and syncs them into a config's `charset.chars`
  (`--src`, `--ext`, `--extra`, `--json`, `--prune`, `--out`, `--dry-run`).
- New optional `render.missing_glyph` config field: `error` (default) keeps the
  old fail-fast behavior; `box` renders codepoints missing from the font as
  hollow-box placeholder glyphs and prints a post-build warning listing them.
- Build errors now include the offending config path, so `build-all` failures
  (e.g. `charset must contain at least one range or char`, `missing glyph`)
  are attributable to a specific JSON file.
- `render.bpp: 1` now always renders through FreeType's monochrome path
  (`FT_LOAD_TARGET_MONO` hinting + `FT_RENDER_MODE_MONO` rasterizer) instead of
  thresholding an 8-bit grayscale bitmap, so thin strokes no longer drop out at
  small sizes. 1bpp glyph bitmaps and metrics (`adv_w`, box size and offsets)
  differ from earlier output, so regenerate 1bpp fonts; the format itself and
  2/4/8 bpp output are unchanged.

## 0.3.0

Initial public release of `font2c`.

Highlights:

- Windows single-file executable
- Unicode range + sparse character mapping
- `internal` and `external` deployment modes
- `.c/.h` and `.bin` export support
- MIT licensed
- GitHub Actions CI for Windows and macOS

## 0.2.0

Internal development release.
