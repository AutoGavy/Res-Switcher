# Res Switcher

A command-line tool for toggling the primary display resolution.

## Resolution options

```text
"Res Switcher.exe" -1050    Toggle between 1680x1050 and 3840x2160
"Res Switcher.exe" -1080    Toggle between 1920x1080 and 3840x2160
"Res Switcher.exe" -1440    Toggle between 2560x1440 and 3840x2160
```

Without a resolution option, the program preserves the existing behavior and
toggles between `1720x1080` and `3840x2160`.

Use `--dry-run` to preview the selected mode without changing the display:

```text
"Res Switcher.exe" -1440 --dry-run
```
