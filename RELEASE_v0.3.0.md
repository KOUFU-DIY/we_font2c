# font2c v0.3.0

`font2c` v0.3.0 is the first public release of the project.

## Highlights

- Windows single-file executable
- `ttf` / `otf` / `ttc` font extraction
- Unicode range + sparse character mapping
- `internal` and `external` deployment modes
- `.c/.h` and `.bin` export support
- MIT licensed
- GitHub Actions CI for Windows and macOS

## Downloads

Release assets are generated automatically by GitHub Actions when a `v*` tag is pushed.

- Windows: `font2c-v0.3.0-windows.exe`
- macOS: `font2c-v0.3.0-macos`

## Notes

- `fonts/` is intentionally empty in the repository.
- Please confirm font redistribution rights before publishing generated assets.
- `external` mode stores glyph descriptors and bitmap data in the `.bin` payload.
- Generated fonts are based on the selected input font and JSON config.

## Usage

```txt
font2c.exe
font2c.exe build <config.json> [-o <output_dir>]
font2c.exe build-all <input_dir> [-o <output_dir>]
```

## Repository

- https://github.com/KOUFU-DIY/we_font2c
