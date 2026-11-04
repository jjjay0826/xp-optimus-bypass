@echo off
setlocal DisableDelayedExpansion
set "XP_HERE=%~dp0"
rem Running straight from inside the ZIP (Windows extracts to a temporary folder) cannot work.
echo %XP_HERE% | findstr /i /c:"\AppData\Local\Temp\" >nul && (
    echo This was started from inside the ZIP file. Extract the whole ZIP first ^(right-click, Extract All^), then run
    echo XP_Optimus_Bypass.bat from the extracted folder.
    pause
    exit /b 1
)
set "XP_BOOT=%~dp0app\bootstrap.ps1"
if not exist "%XP_BOOT%" set "XP_BOOT=%LOCALAPPDATA%\XP Optimus Bypass Installer\0.9.3\app\bootstrap.ps1"
if not exist "%XP_BOOT%" (
    echo Installer files are missing. Extract the full ZIP again.
    echo Keep the app and docs folders next to this BAT.
    pause
    exit /b 1
)
where powershell.exe >nul 2>&1 || (
    echo Windows PowerShell was not found. Use the fallback folder ^(install.bat^) instead.
    pause
    exit /b 1
)
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%XP_BOOT%"
if errorlevel 1 (
    echo Installer failed. See the message and %LOCALAPPDATA%\XP Optimus Bypass Installer\installer-error.log
    echo If it keeps failing, use the fallback folder ^(install.bat^).
    pause
    exit /b 1
)
exit /b 0
