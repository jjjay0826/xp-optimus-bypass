@echo off
setlocal
rem Removes the fallback registration of THIS folder.
set "KEY=HKCU\Software\Khronos\Vulkan\ImplicitLayers"
for %%I in ("%~dp0..\app") do set "APP=%%~fI"
set "DONE=0"
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass.json" /f >nul 2>&1 && set "DONE=1"
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass_global.json" /f >nul 2>&1 && set "DONE=1"
if "%DONE%"=="1" (echo Removed. X-Plane is back to normal.) else (echo Nothing from this folder was registered.)
reg query "%KEY%" 2>nul | findstr /i "xp_optimus_bypass" >nul && (
    echo Note: another XP Optimus Bypass installation is still registered. Use its own uninstall.
)
pause
