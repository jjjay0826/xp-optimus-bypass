@echo off
setlocal
set "KEY=HKCU\Software\Khronos\Vulkan\ImplicitLayers"
echo XP Optimus Bypass - install
echo.
echo   [1] X-Plane only (recommended)
echo       Start X-Plane with start_xplane.bat. Other Vulkan programs never load the layer.
echo   [2] All Vulkan programs (old 0.9.1 behaviour)
echo       Start X-Plane as usual. Every Vulkan program loads the DLL, but it only acts in X-Plane.exe.
echo       Uninstall it before playing games with anti-cheat in Vulkan mode.
echo.
choice /c 12 /n /m "Choose 1 or 2: "
if errorlevel 2 (set "JSON=%~dp0xp_optimus_bypass_global.json") else (set "JSON=%~dp0xp_optimus_bypass.json")

rem Remove every earlier XP Optimus Bypass entry (any folder, either mode)
powershell -NoProfile -Command "$k='HKCU:\Software\Khronos\Vulkan\ImplicitLayers'; if (Test-Path $k) { (Get-Item $k).Property | Where-Object { $_ -like '*xp_optimus_bypass*' } | ForEach-Object { Remove-ItemProperty -LiteralPath $k -Name $_; Write-Host ('Removed old entry: ' + $_) } }"

reg add "%KEY%" /v "%JSON%" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 (
    echo Install FAILED.
    pause
    exit /b 1
)
echo.
if /i "%JSON:~-11%"=="global.json" (
    echo Installed for all Vulkan programs. Start X-Plane as usual,
    echo then check xp_optimus_bypass.log in this folder.
) else (
    echo Installed for X-Plane only. Start X-Plane with start_xplane.bat in this folder,
    echo then check xp_optimus_bypass.log here. Starting X-Plane any other way runs it without the layer.
)
pause
