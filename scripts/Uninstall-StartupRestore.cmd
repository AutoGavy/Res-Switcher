@echo off
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0Set-StartupRestore.ps1" -Action Uninstall
if errorlevel 1 (
    echo Startup removal failed.
    pause
    exit /b 1
)
echo Res Switcher startup restoration has been removed.
pause
