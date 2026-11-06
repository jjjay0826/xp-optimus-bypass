[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$cache = Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\0.9.3'
$diagnostics = Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\installer-error.log'
try {
    # Locked-down PowerShell (Constrained Language Mode, e.g. school or company PCs) cannot show the installer.
    if ($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage') {
        throw ('PowerShell is restricted on this PC (' + $ExecutionContext.SessionState.LanguageMode + ' mode). ' +
               'Use the files in the "fallback" folder instead. / 這台電腦的 PowerShell 受到限制，請改用 fallback 資料夾裡的安裝方式。')
    }
    Add-Type -AssemblyName System.Windows.Forms
    $sourceRoot = Split-Path -Parent $PSScriptRoot
    $files = @('XP_Optimus_Bypass.bat','README.txt','LICENSE','CHANGELOG.md')
    $appFiles = @('bootstrap.ps1','core.ps1','manager.ps1','discovery.ps1','launch.ps1','i18n.ps1','i18n.json','log-viewer.ps1','report.ps1',
        'xp_optimus_bypass.dll','xp_optimus_bypass.ini','xp_optimus_bypass.json','xp_optimus_bypass_global.json')
    # Files from a downloaded ZIP carry an Internet mark; remove it from this package's own files only
    # (the files listed above, plus the files directly inside its docs and fallback folders).
    $own = @($files | ForEach-Object { Join-Path $sourceRoot $_ }) + @($appFiles | ForEach-Object { Join-Path $PSScriptRoot $_ })
    foreach ($sub in @('docs', 'fallback')) {
        $dir = Join-Path $sourceRoot $sub
        if (Test-Path -LiteralPath $dir -PathType Container) {
            $own += @(Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
        }
    }
    foreach ($f in $own) {
        if (Test-Path -LiteralPath $f -PathType Leaf) { Unblock-File -LiteralPath $f -ErrorAction SilentlyContinue }
    }
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
    $env = 'PowerShell ' + $PSVersionTable.PSVersion + ' | ' + [Environment]::OSVersion.VersionString + ' | language mode ' +
           $ExecutionContext.SessionState.LanguageMode + ' | 64-bit process ' + [Environment]::Is64BitProcess
    ('[' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '] installer 0.9.3 | ' + $env) | Add-Content -LiteralPath $diagnostics -Encoding UTF8
    ($_ | Out-String) | Add-Content -LiteralPath $diagnostics -Encoding UTF8
    if ($_.ScriptStackTrace) { $_.ScriptStackTrace | Add-Content -LiteralPath $diagnostics -Encoding UTF8 }
    $message = 'Unable to open the installer. / 無法開啟安裝介面。' + "`r`n`r`n" + $_.Exception.Message + "`r`n`r`nLog / 錯誤紀錄：" + $diagnostics +
               "`r`n`r`nIf this keeps happening, use the fallback folder (install.bat). / 若持續發生，請改用 fallback 資料夾的 install.bat。" 
    try { [void][Windows.Forms.MessageBox]::Show($message, 'XP Optimus Bypass', 'OK', 'Error') }
    catch { Write-Host $message }
    exit 1
}
