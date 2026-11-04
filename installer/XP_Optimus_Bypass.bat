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
rem Start the installer window without keeping this console open (Windows 10 1903 or newer, unrestricted PowerShell).
rem Otherwise run it here, so any error stays readable in this window.
set "XP_CONHOST=%SystemRoot%\System32\conhost.exe"
set "XP_BUILD=0"
for /f "tokens=4-6 delims=. " %%a in ('ver') do set "XP_BUILD=%%c"
set "XP_BUILD=%XP_BUILD:]=%"
if not exist "%XP_CONHOST%" goto visible
if %XP_BUILD% LSS 18362 goto visible
powershell.exe -NoLogo -NoProfile -Command "if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') { exit 3 }"
if errorlevel 1 goto visible
start "" "%XP_CONHOST%" --headless powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%XP_BOOT%"
exit /b 0

:visible
powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%XP_BOOT%"
if errorlevel 1 (
    echo Installer failed. See the message and %LOCALAPPDATA%\XP Optimus Bypass Installer\installer-error.log
    echo If it keeps failing, use the fallback folder ^(install.bat^).
    pause
    exit /b 1
)
exit /b 0
