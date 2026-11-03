@echo off
setlocal
rem Removes every XP Optimus Bypass entry, from any folder and either install mode
powershell -NoProfile -Command "$k='HKCU:\Software\Khronos\Vulkan\ImplicitLayers'; $n=0; if (Test-Path $k) { (Get-Item $k).Property | Where-Object { $_ -like '*xp_optimus_bypass*' } | ForEach-Object { Remove-ItemProperty -LiteralPath $k -Name $_; Write-Host ('Removed: ' + $_); $n++ } }; if ($n -eq 0) { Write-Host 'Nothing to remove - it was not installed.' } else { Write-Host 'X-Plane is back to normal. You can delete this folder now.' }"
pause
