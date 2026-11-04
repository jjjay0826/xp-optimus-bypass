@echo off
setlocal
rem Fallback install without PowerShell: registers the layer from THIS extracted folder. Keep the folder.
set "KEY=HKCU\Software\Khronos\Vulkan\ImplicitLayers"
for %%I in ("%~dp0..\app") do set "APP=%%~fI"
if not exist "%APP%\xp_optimus_bypass.dll" (
    echo The app folder is missing. Extract the whole ZIP again and keep the app folder next to this one.
    pause
    exit /b 1
)
reg query "%KEY%" 2>nul | findstr /i "xp_optimus_bypass" | findstr /i /v /c:"%APP%\xp_optimus_bypass" >nul && (
    echo Another XP Optimus Bypass installation is registered. Uninstall it first
    echo ^(XP_Optimus_Bypass.bat - Uninstall, or uninstall.bat in its fallback folder^).
    pause
    exit /b 1
)
echo XP Optimus Bypass - fallback install (no PowerShell needed)
echo.
echo   [1] X-Plane only (recommended): start X-Plane with start_xplane.bat in this folder.
echo   [2] All Vulkan programs: start X-Plane as usual. Every Vulkan program loads the DLL;
echo       uninstall it before playing games with anti-cheat in Vulkan mode.
echo.
choice /c 12 /n /m "Choose 1 or 2: "
if errorlevel 2 (set "JSON=%APP%\xp_optimus_bypass_global.json") else (set "JSON=%APP%\xp_optimus_bypass.json")
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass.json" /f >nul 2>&1
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass_global.json" /f >nul 2>&1
reg add "%KEY%" /v "%JSON%" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 (
    echo Install FAILED.
    pause
    exit /b 1
)
echo.
echo Installed from this folder. Do not move or delete it while installed.
echo The log is written to the app folder: xp_optimus_bypass.log
pause
