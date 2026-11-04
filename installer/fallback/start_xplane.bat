@echo off
setlocal
rem Starts X-Plane with the layer enabled (needed for the "X-Plane only" fallback install).
for %%I in ("%~dp0..\app") do set "APP=%%~fI"
set "CFG=%~dp0xplane_path.txt"
set "XPEXE="
if exist "%CFG%" set /p XPEXE=<"%CFG%"
if defined XPEXE if exist "%XPEXE%" goto run

set "XPEXE=%ProgramFiles(x86)%\Steam\steamapps\common\X-Plane 12\X-Plane.exe"
if exist "%XPEXE%" goto save

:ask
set "XPEXE="
echo Drag X-Plane.exe into this window (or paste its full path), then press Enter:
set /p XPEXE=
if not defined XPEXE goto ask
set XPEXE=%XPEXE:"=%
if exist "%XPEXE%" goto save
echo Not found: %XPEXE%
goto ask

:save
>"%CFG%" echo %XPEXE%
echo X-Plane: %XPEXE%
echo (saved in xplane_path.txt - delete that file to choose another X-Plane)

:run
rem Steam restarts X-Plane.exe when it is not started by Steam, and the new process would not get the variables
rem below. A steam_appid.txt next to X-Plane.exe lets it start directly (Steam must be running). Created only
rem when missing, recorded in steam_appid_created.txt and removed by uninstall.bat.
echo "%XPEXE%" | findstr /i /c:"steamapps" >nul || goto launch
for %%F in ("%XPEXE%") do set "XPDIR=%%~dpF"
for %%F in ("%XPDIR%.") do set "XPFOLDER=%%~nxF"
set "APPID="
if /i "%XPFOLDER%"=="X-Plane 12" set "APPID=2014780"
if /i "%XPFOLDER%"=="X-Plane 11" set "APPID=269950"
if not defined APPID goto launch
tasklist /fi "imagename eq steam.exe" | findstr /i "steam.exe" >nul || echo Please start Steam first, then run this again.
tasklist /fi "imagename eq steam.exe" | findstr /i "steam.exe" >nul || goto end
if exist "%XPDIR%steam_appid.txt" goto launch
>"%XPDIR%steam_appid.txt" <nul set /p "=%APPID%"
>>"%~dp0steam_appid_created.txt" echo %APPID%^|%XPDIR%steam_appid.txt

:launch
del "%APP%\xp_optimus_bypass.log" >nul 2>&1
set "XP_OPTIMUS_BYPASS_ENABLE=1"
set "XP_OPTIMUS_BYPASS_LOG=%APP%\xp_optimus_bypass.log"
for %%F in ("%XPEXE%") do start "" /D "%%~dpF." "%%~fF"
goto :eof

:end
pause
