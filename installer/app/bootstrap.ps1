[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$cache = Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\0.9.2'
$diagnostics = Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\installer-error.log'
try {
    Add-Type -AssemblyName System.Windows.Forms
    $sourceRoot = Split-Path -Parent $PSScriptRoot
    $files = @('XP_Optimus_Bypass.bat','README.txt','LICENSE','CHANGELOG.md')
    $appFiles = @('bootstrap.ps1','core.ps1','manager.ps1','discovery.ps1','launch.ps1','i18n.ps1','i18n.json','log-viewer.ps1',
        'xp_optimus_bypass.dll','xp_optimus_bypass.ini','xp_optimus_bypass.json','xp_optimus_bypass_global.json')
    # A separate installer cache survives uninstall. Never run maintenance from files being removed.
    if ([IO.Path]::GetFullPath($sourceRoot).TrimEnd('\') -ine [IO.Path]::GetFullPath($cache).TrimEnd('\')) {
        [IO.Directory]::CreateDirectory((Join-Path $cache 'app')) | Out-Null
        foreach ($name in $files) { Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination $cache -Force }
        foreach ($name in $appFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $cache 'app') -Force }
        $sourceDocs = Join-Path $sourceRoot 'docs'
        [IO.Directory]::CreateDirectory((Join-Path $cache 'docs')) | Out-Null
        foreach ($file in (Get-ChildItem -LiteralPath $sourceDocs -File)) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $cache 'docs') -Force
        }
    }
    & (Join-Path $cache 'app\manager.ps1')
} catch {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $diagnostics)) | Out-Null
    ($_ | Out-String) | Add-Content -LiteralPath $diagnostics -Encoding UTF8
    $message = 'Unable to open the installer. / 無法開啟安裝介面。' + "`r`n`r`n" + $_.Exception.Message + "`r`n`r`nLog / 錯誤紀錄：" + $diagnostics
    try { [void][Windows.Forms.MessageBox]::Show($message, 'XP Optimus Bypass', 'OK', 'Error') }
    catch { Write-Host $message }
    exit 1
}
