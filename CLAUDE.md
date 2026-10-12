# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`font2c` is a standalone CLI tool (pure C11) that converts `ttf`/`otf`/`ttc` fonts into bitmap fonts for embedded firmware: either `.c/.h` source files (internal mode) or `.c/.h` + `.bin` blob (external mode, for flash/file storage). The only external dependency is FreeType (found via `pkg-config`); JSON parsing is hand-written in-tree.

## Build

Windows (requires MSYS2 MINGW64 with `mingw-w64-x86_64-freetype` and `pkgconf`; the script auto-detects MSYS2 via `MSYS2_LOCATION` env var, then `C:\msys64`, `D:\msys64`; override with `CC`/`PKG_CONFIG`/`WINDRES`):

```bash
cmd /c builder\\build_win.cmd
```

Produces a statically linked `windows\font2c.exe`, with `builder/icon_64x64.ico` embedded via windres.

macOS / POSIX (produces `font2c` at repo root):

```bash
bash builder/build_posix.sh
```

## Verification

There is no test framework. Verification is building and running the sample export flow (this is also what CONTRIBUTING.md requires before a PR):

```bash
./windows/font2c.exe build-all ./input -o ./output
```

Build a single config:

```bash
./windows/font2c.exe build input/arial_16_4bbp.json -o output
```

The tool operates relative to the current working directory and auto-creates `fonts/`, `input/`, `output/` there. Font files referenced by configs resolve in order: path relative to the JSON file → project `fonts/` → system font directories (matched by file name like `msyh.ttc`, not family name). `fonts/` is intentionally empty in git; `msyh.ttc`/`arial.ttf` configs work on Windows via the system font fallback.

`builder/testdata/probe_faces.c` is a tiny standalone FreeType utility for inspecting `.ttc` face indices (contains a hardcoded local path; compile manually when needed).

## Architecture

Five source files under `builder/src/`, one shared header `builder/include/font2c.h` (which also holds `FONT2C_VERSION`):

- `main.c` — entry point: CLI dispatch (`build` / `build-all` / `scan`), workspace auto-creation (build modes only; `scan` never creates directories).
- `common.c` — `StringBuilder`/`ByteBuffer` growable buffers, path/file utilities, directory listing, UTF-8 decoding, CLI parsing.
- `json_config.c` — hand-rolled JSON parser (full DOM) plus validation into `FontConfig`: `U+XXXX` range token parsing, symbol-name validation. Also the JSON serializer used by `rewrite_config_chars_file()`, which updates only `charset.chars` and re-emits the file in the canonical 2-space layout preserving all other fields and their order.
- `scan.c` — the `scan` subcommand: recursive source walk, a one-pass C lexer (comments and char literals consumed, chars collected only from string literals, `\u`/`\U` escapes decoded, invalid UTF-8 skipped with a warning), collects codepoints `>= U+0080` only, then diffs/merges into `charset.chars` (`--prune` to delete unseen; ASCII entries and `charset.ranges` are never touched).
- `font_build.c` — the whole export pipeline, driven by `build_one_config()`:
  1. parse config → `normalize_charset()` (sort + merge ranges, dedupe sparse chars, drop chars already covered by ranges)
  2. `resolve_font_path()` (lookup order above)
  3. FreeType renders each glyph (8-bit grayscale for 2/4/8 bpp; 1bpp always uses the mono path `FT_LOAD_TARGET_MONO` + `FT_RENDER_MODE_MONO`) → `quantize_gray_bitmap()` packs to 1/2/4/8 bpp, MSB-first, row-byte-aligned
  4. emit: `emit_internal_mode()` (.c/.h with bitmap arrays) or `build_external_blob()` + `emit_external_mode()` (.c/.h metadata + `.bin` with magic `0x02 0x00`, big-endian CRC-32/IEEE over `bin[6..]`)

Glyph order is always: all range glyphs (in codepoint order) then all sparse glyphs. All multi-byte integers in generated data are big-endian; glyph descriptors are 14 bytes.

### Error handling convention

Every fallible function returns `bool` and takes trailing `char *error, size_t error_cap` parameters; errors are formatted with `failf()`. Resource cleanup uses the `goto cleanup` pattern with all pointers initialized up front. No global state. Follow this pattern for any new code.

## The README is the format spec

The root `README.md` is the authoritative specification of the binary/generated format (blob layout, glyph descriptor fields, range/sparse tables, CRC rules, quantization formula, runtime lookup). Any change to the emit code in `font_build.c` must keep that spec in sync — and consumers depend on it, so format changes are breaking.

## Documentation conventions

- Every `README.md` has a `README_CN.md` Chinese sibling (root, `builder/`, `input/`, `fonts/`). Keep both in sync when editing docs.
- Docs are strictly scoped: root README = CLI usage + format spec; `input/README.md` = JSON config format; `builder/README.md` = build instructions. Don't duplicate content across them.
- JSON configs must be UTF-8 without BOM with LF line endings; `charset.chars` holds literal Unicode characters.

## Releases

- Version lives in `FONT2C_VERSION` (`builder/include/font2c.h`); update `CHANGELOG.md` alongside it.
- CI (`.github/workflows/ci.yml`) builds Windows (MSYS2 MINGW64) and macOS (Homebrew freetype) on pushes/PRs to `main`.
- Pushing a `v*` tag triggers `release.yml`, which publishes both binaries to a GitHub release.
- Never commit fonts (license risk), generated `output/` artifacts, or built binaries — all gitignored.
