# builder

This directory stores the source code and build scripts for `font2c`.
This document only describes build-related usage.

Executable usage is in `../README.md`.
JSON usage is in `../input/README.md`.

## Files

- `src/`: core source files
- `include/`: shared headers
- `build_win.cmd`: Windows build script
- `build_posix.sh`: POSIX build script
- `font2c.rc`: Windows resource script
- `icon_64x64.ico`: Windows executable icon

## Build Requirements

Common requirements:

- C compiler
- `pkg-config`
- `freetype2`

Windows resource build also uses:

- `windres`

## Windows Build

From the project root:

```txt
builder\build_win.cmd
```

Output:

```txt
font2c.exe
```

Notes:

- prefers `D:\msys64\mingw64\bin\gcc.exe` when present
- falls back to `gcc` from `PATH`
- prefers `D:\msys64\mingw64\bin\pkg-config.exe` when present
- prefers `D:\msys64\mingw64\bin\windres.exe` when present
- embeds `builder/icon_64x64.ico` into the exe if the file exists

Optional environment overrides:

- `CC`
- `PKG_CONFIG`
- `WINDRES`

## POSIX Build

From the project root:

```sh
sh builder/build_posix.sh
```

Output:

```txt
font2c
```

Optional environment overrides:

- `CC`
- `PKG_CONFIG`

## Current Scope

- Windows-first release target
- source layout keeps room for macOS and Linux builds later
