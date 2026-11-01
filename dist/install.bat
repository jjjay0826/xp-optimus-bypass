@echo off
reg add "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%~dp0xp_optimus_bypass.json" /t REG_DWORD /d 0 /f
if errorlevel 1 (echo Install FAILED.) else (echo Installed. Start X-Plane, then check xp_optimus_bypass.log in this folder.)
pause
