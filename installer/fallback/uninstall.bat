@echo off
setlocal
rem Removes the fallback registration of THIS folder.
set "KEY=HKCU\Software\Khronos\Vulkan\ImplicitLayers"
for %%I in ("%~dp0..\app") do set "APP=%%~fI"
set "DONE=0"
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass.json" /f >nul 2>&1 && set "DONE=1"
reg delete "%KEY%" /v "%APP%\xp_optimus_bypass_global.json" /f >nul 2>&1 && set "DONE=1"
if exist "%~dp0steam_appid_created.txt" for /f "usebackq tokens=1,* delims=|" %%A in ("%~dp0steam_appid_created.txt") do call :rmappid "%%A" "%%B"
if exist "%~dp0steam_appid_created.txt" del "%~dp0steam_appid_created.txt"
if "%DONE%"=="1" (echo Removed. X-Plane is back to normal.) else (echo Nothing from this folder was registered.)
reg query "%KEY%" 2>nul | findstr /i "xp_optimus_bypass" >nul && (
    echo Note: another XP Optimus Bypass installation is still registered. Use its own uninstall.
)
pause
goto :eof

rem Delete a steam_appid.txt only if this folder created it and it still holds the same app id.
:rmappid
if /i not "%~nx2"=="steam_appid.txt" goto :eof
if not exist "%~2" goto :eof
set "V="
set /p V=<"%~2"
if "%V%"=="%~1" del "%~2"
goto :eof
