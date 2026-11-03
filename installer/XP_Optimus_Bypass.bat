@echo off
setlocal DisableDelayedExpansion
set "XP_BOOT=%~dp0app\bootstrap.ps1"
if not exist "%XP_BOOT%" set "XP_BOOT=%LOCALAPPDATA%\XP Optimus Bypass Installer\1.5\app\bootstrap.ps1"
if not exist "%XP_BOOT%" (
    echo Installer files are missing. Extract the full ZIP again.
    echo Keep the app and docs folders next to this BAT.
    pause
    exit /b 1
)
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%XP_BOOT%"
if errorlevel 1 (
    echo Installer failed. See the message and installer-error.log.
    pause
    exit /b 1
)
exit /b 0
