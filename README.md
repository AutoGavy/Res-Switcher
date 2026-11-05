# Res Switcher

A small Windows command-line tool that switches the primary display between
`3840x2160` and a selected lower resolution.

## Usage

```console
"Res Switcher.exe" -1050   # 1680x1050 <-> 3840x2160
"Res Switcher.exe" -1080   # 1920x1080 <-> 3840x2160
"Res Switcher.exe" -1440   # 2560x1440 <-> 3840x2160
"Res Switcher.exe" -battery # Restore 4K @ 240 Hz + toggle power plan
```

Add `--dry-run` to preview the change. Without a resolution option, the program
switches between `1720x1080` and `3840x2160`.

If the primary display is not at 240 Hz, running any resolution profile first
targets `3840x2160 @ 240 Hz`, even when already at 3840x2160. At 240 Hz, the
selected lower resolution and 3840x2160 toggle normally. `--dry-run` never
changes the display, and `--help` only prints usage.

After a successful switch, `3840x2160` uses **Balanced**; all lower resolutions
use **Ultimate Performance**, including the default `1720x1080` mode. Both
plans must already exist in Windows (English and Simplified Chinese names are
recognized). A missing target plan cancels the switch. `--dry-run` never
changes the power plan.

Only one switch can run at a time. On a switching or power-activation failure,
the program attempts to restore the original display topology, mode, colors
and scaling, and reports any recovery it cannot verify. Duplicate matching
power-plan names must be resolved in Windows before switching.

The display scale is set to 150% at `3840x2160`, 100% at `1720x1080` and
`1920x1080`, and 125% at `1680x1050` and `2560x1440`.

The switch is cancelled if the target 240 Hz mode is unavailable at the current
desktop color depth, or the NVIDIA color settings cannot be preserved.

Use `"Res Switcher.exe" --restore-4k` to ensure `3840x2160 @ 240 Hz` without
prompts. If already at this mode, it leaves the display unchanged and ensures
Balanced is active. Otherwise it restores this mode with 150% scaling and
activates Balanced. An already active plan is not reapplied. Exit code 0 means
success; nonzero means failure. Add `--dry-run` to preview only.

Use `"Res Switcher.exe" -battery` to restore the same 4K mode and toggle
**Balanced <-> Ultimate Performance** instead of always selecting Balanced.
Other active power plans switch to Balanced. If already at 4K @ 240 Hz, only
the power plan changes. If restoring 4K fails, the power plan is not toggled.
After success, it shows `Current power plan: NAME` with the plan name in green
and keeps the window open for 1 second. This mode runs without prompts,
supports `--dry-run` (no delay), and cannot be combined with `--restore-4k`
or a resolution option.

## Restore at sign-in

Double-click `scripts\Install-StartupRestore.cmd` to run `--restore-4k` once
when the current user signs in to Windows. No administrator rights, service,
or resident background process are required. This does not run at shutdown.

Use `scripts\Set-StartupRestore.ps1 -Action Status` to check registration,
or `scripts\Uninstall-StartupRestore.cmd` to remove the startup shortcut.
Reinstall after moving the built EXE. The primary display must support
`3840x2160 @ 240 Hz`; unsupported modes are not applied.

If the same monitor is connected through NVIDIA DisplayPort and another GPU's
HDMI output, Res Switcher forces the NVIDIA DisplayPort path and removes the
other GPU's active clone before switching resolution. The HDMI cable remains
connected and can still be re-enabled manually as a backup display path.
Other independent displays are retained.
