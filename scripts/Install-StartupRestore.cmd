@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Set-StartupRestore.ps1" -Action Install
if errorlevel 1 (
    echo Startup registration failed.
    pause
    exit /b 1
)
echo Res Switcher will run --restore-4k when the current user signs in.
pause
