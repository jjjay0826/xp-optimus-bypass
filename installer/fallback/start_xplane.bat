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
del "%APP%\xp_optimus_bypass.log" >nul 2>&1
set "XP_OPTIMUS_BYPASS_ENABLE=1"
set "XP_OPTIMUS_BYPASS_LOG=%APP%\xp_optimus_bypass.log"
for %%F in ("%XPEXE%") do start "" /D "%%~dpF." "%%~fF"
