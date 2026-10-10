[CmdletBinding()]
param(
    [ValidateSet('Install', 'Uninstall', 'Status')]
    [string]$Action = 'Install',
    [string]$ExecutablePath
)

$ErrorActionPreference = 'Stop'
$repositoryPath = Split-Path -Parent $PSScriptRoot
if (-not $ExecutablePath) {
    $ExecutablePath = Join-Path $repositoryPath 'x64\Release\Res Switcher.exe'
}
$startupDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::Startup)
if (-not $startupDirectory) { throw 'The current user startup folder is unavailable.' }
$shortcutPath = Join-Path $startupDirectory 'Res Switcher Restore 4K.lnk'
$description = 'Res Switcher: restore 3840x2160 @ 240 Hz at sign-in.'
$arguments = '--restore-4k'
$shell = $null
$shortcut = $null

try {
    if ($Action -eq 'Install') {
        $ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
        if (-not [IO.File]::Exists($ExecutablePath)) { throw 'The Release executable was not found.' }
    }
    $exists = [IO.File]::Exists($shortcutPath)
    $owned = $false
    $target = $null
    $actualArguments = $null
    if ($exists -or $Action -eq 'Install') {
        $shell = New-Object -ComObject WScript.Shell
        if ($Action -eq 'Install') { [void][IO.Directory]::CreateDirectory($startupDirectory) }
        $shortcut = $shell.CreateShortcut($shortcutPath)
        if ($exists) {
            $target = $shortcut.TargetPath
            $actualArguments = $shortcut.Arguments
            $owned = $actualArguments -eq $arguments -and (
                $shortcut.Description -eq $description -or
                [string]::Equals($target, $ExecutablePath, [StringComparison]::OrdinalIgnoreCase))
        }
    }

    if ($Action -eq 'Install') {
        if ($exists -and -not $owned) { throw 'An unrelated startup shortcut has the same name; it was left unchanged.' }
        $shortcut.TargetPath = $ExecutablePath
        $shortcut.Arguments = $arguments
        $shortcut.WorkingDirectory = [IO.Path]::GetDirectoryName($ExecutablePath)
        $shortcut.IconLocation = "$ExecutablePath,0"
        $shortcut.Description = $description
        $shortcut.WindowStyle = 7 # Minimized; do not take focus from the desktop.
        $shortcut.Save()
        $owned = $true
        $target = $ExecutablePath
        $actualArguments = $arguments
    } elseif ($Action -eq 'Uninstall' -and $exists) {
        if (-not $owned) { throw 'The startup shortcut is not owned by Res Switcher; it was left unchanged.' }
        [IO.File]::Delete($shortcutPath)
        $owned = $false
        $target = $null
        $actualArguments = $null
    }

    [pscustomobject]@{
        Success = $true
        Action = $Action
        Registered = $owned -and [IO.File]::Exists($shortcutPath)
        Shortcut = $shortcutPath
        Executable = $target
        Arguments = $actualArguments
        Trigger = 'Current user sign-in'
    } | ConvertTo-Json
} finally {
    if ($null -ne $shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
    if ($null -ne $shell) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
}
