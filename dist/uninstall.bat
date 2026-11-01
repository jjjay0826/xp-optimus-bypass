@echo off
reg delete "HKCU\Software\Khronos\Vulkan\ImplicitLayers" /v "%~dp0xp_optimus_bypass.json" /f
if errorlevel 1 (echo Nothing to remove - was it installed from this folder?) else (echo Removed. X-Plane is back to normal.)
pause
