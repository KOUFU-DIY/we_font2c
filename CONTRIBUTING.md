# Contributing

Thanks for your interest in `font2c`.

## Before you submit

- Keep generated files out of commits unless they are intentional examples.
- Do not add proprietary or system fonts to the repository.
- Run the build and sample export flow before opening a pull request.

## Build and test

Windows:

```txt
cmd /c builder\build_win.cmd
.\font2c.exe build-all .\input -o .\output
```

macOS / POSIX:

```txt
bash builder/build_posix.sh
./font2c build-all ./input -o ./output
```

## Reporting issues

Please include:

- your OS
- compiler or runtime version
- the JSON config you used
- the source font file name
- the exact error message or unexpected output
