# Res Switcher

A small Windows command-line tool that switches the primary display between
`3840x2160` and a selected lower resolution.

## Usage

```console
"Res Switcher.exe" -1050   # 1680x1050 <-> 3840x2160
"Res Switcher.exe" -1080   # 1920x1080 <-> 3840x2160
"Res Switcher.exe" -1440   # 2560x1440 <-> 3840x2160
```

Add `--dry-run` to preview the change. Without a resolution option, the program
switches between `1720x1080` and `3840x2160`.
