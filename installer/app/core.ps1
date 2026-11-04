$script:ProductName = 'XP Optimus Bypass'
$script:InstallerVersion = '0.9.3'
$script:DllVersion = '0.9.3.1'
$script:LayerKey = 'HKCU:\Software\Khronos\Vulkan\ImplicitLayers'
$script:UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\XPOptimusBypass'
$script:RootFiles = @('XP_Optimus_Bypass.bat', 'README.txt', 'LICENSE', 'CHANGELOG.md')
$script:LegacyRootFiles = @('Setup.bat', 'Start_XPlane.bat', 'Manage.bat', 'Uninstall.bat', 'README.md')
$script:ManagedRootFiles = $script:RootFiles + $script:LegacyRootFiles
function Set-InstallPaths([string]$Root) {
    $script:InstallRoot = $Root
    $script:AppDir = Join-Path $Root 'app'
    $script:DocsDir = Join-Path $Root 'docs'
    $script:RootLog = Join-Path $Root 'xp_optimus_bypass.log'
    $script:Marker = Join-Path $Root 'xpob-install.json'
}
function Resolve-InstallDestination([string]$Value, [string]$SourceApp) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw (T '請選擇安裝資料夾。') }
    $expanded = [Environment]::ExpandEnvironmentVariables($Value.Trim().Trim('"'))
    if ($expanded -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\]+\\[^\\]+\\)') { throw (T '請輸入完整的安裝資料夾路徑。') }
    $target = [IO.Path]::GetFullPath($expanded).TrimEnd('\')
    $forbidden = @([IO.Path]::GetPathRoot($target).TrimEnd('\'), $env:WINDIR, $env:USERPROFILE, $env:LOCALAPPDATA, $env:APPDATA, $env:ProgramFiles, ${env:ProgramFiles(x86)})
    foreach ($path in $forbidden) {
        if ($path -and $target -ieq $path.TrimEnd('\')) { throw (T '請為本工具選擇獨立的子資料夾。') }
    }
    $source = [IO.Path]::GetFullPath((Split-Path -Parent $SourceApp)).TrimEnd('\')
    if ($target -ieq $source -or $target.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase) -or $source.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw (T '安裝位置不能包含或位於保留的安裝來源內。')
    }
    return $target
}
$initialRoot = Join-Path $env:LOCALAPPDATA 'Programs\XP Optimus Bypass'
$registeredInstall = Get-ItemProperty -LiteralPath $script:UninstallKey -ErrorAction SilentlyContinue
if ($registeredInstall -and -not [string]::IsNullOrWhiteSpace($registeredInstall.InstallLocation)) {
    $initialRoot = [string]$registeredInstall.InstallLocation
}
Set-InstallPaths $initialRoot

function Get-InstallState {
    $file = Join-Path $script:AppDir 'install-state.json'
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        try { return (Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json) } catch { return $null }
    }
}
# Turn automatic anonymous reports on or off without reinstalling.
function Set-ReportConsent([bool]$Enabled) {
    $file = Join-Path $script:AppDir 'install-state.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return }
    $state = Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json
    $table = @{}
    foreach ($prop in $state.PSObject.Properties) { $table[$prop.Name] = $prop.Value }
    $table['reports'] = $Enabled
    $table | ConvertTo-Json | Set-Content -LiteralPath $file -Encoding UTF8
}
function Assert-OwnedRoot {
    if (Test-Path -LiteralPath $script:InstallRoot) {
        $item = Get-Item -LiteralPath $script:InstallRoot
        if (-not $item.PSIsContainer) { throw (T '安裝位置必須是資料夾。') }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw (T '安裝位置不能是目錄連結。') }
        if (-not (Test-Path -LiteralPath $script:Marker -PathType Leaf)) {
            if (@(Get-ChildItem -LiteralPath $script:InstallRoot -Force).Count -eq 0) { return }
            throw ((T '安裝位置已有其他內容，為避免覆蓋，請先將該資料夾移走：') + $script:InstallRoot)
        }
        $markerData = Get-Content -LiteralPath $script:Marker -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($markerData.product -ne 'XPOptimusBypass') { throw (T '安裝位置的識別資料不符，已停止操作。') }
    }
}
function Assert-XPlaneClosed {
    if (Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue) { throw (T '請先關閉 X-Plane，再安裝、切換模式或解除安裝。') }
}
function Get-LayerEntries {
    if (Test-Path -LiteralPath $script:LayerKey) {
        $key = Get-Item -LiteralPath $script:LayerKey
        foreach ($name in $key.GetValueNames()) {
            if ([IO.Path]::GetFileName($name) -in @('xp_optimus_bypass.json', 'xp_optimus_bypass_global.json')) {
                [pscustomobject]@{ Name = $name; Value = $key.GetValue($name); Kind = $key.GetValueKind($name).ToString() }
            }
        }
    }
}
function Clear-LayerEntries {
    foreach ($entry in @(Get-LayerEntries)) { Remove-ItemProperty -LiteralPath $script:LayerKey -Name $entry.Name -ErrorAction Stop }
}
function Restore-LayerEntries($Entries) {
    Clear-LayerEntries
    if (@($Entries).Count -gt 0) {
        New-Item -Path $script:LayerKey -Force | Out-Null
        foreach ($entry in $Entries) { New-ItemProperty -LiteralPath $script:LayerKey -Name $entry.Name -Value $entry.Value -PropertyType $entry.Kind -Force | Out-Null }
    }
}
function Get-ShortcutPaths {
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'XP Optimus Bypass'
    @((Join-Path $menu '啟動 X-Plane.lnk'), (Join-Path $menu '管理與解除安裝.lnk'),
      (Join-Path ([Environment]::GetFolderPath('Desktop')) 'X-Plane (Optimus Bypass).lnk'))
}
# Windows Terminal (the default console on Windows 11) ignores -WindowStyle Hidden, so an empty PowerShell window
# would stay open while the launcher waits for X-Plane to close. A headless conhost runs the script with no window.
function Get-HiddenLaunch([string]$ScriptPath) {
    $ps = Join-Path $PSHOME 'powershell.exe'
    $psArgs = '-NoLogo -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $ScriptPath + '"'
    $conhost = Join-Path $env:SystemRoot 'System32\conhost.exe'
    if ([Environment]::OSVersion.Version.Build -ge 18362 -and (Test-Path -LiteralPath $conhost)) {
        return @{ File = $conhost; Args = ('--headless "' + $ps + '" ' + $psArgs) }
    }
    return @{ File = $ps; Args = $psArgs }
}
function Write-Shortcut([string]$Path, [string]$ScriptPath) {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($Path)
    $run = Get-HiddenLaunch $ScriptPath
    $link.TargetPath = $run.File
    $link.Arguments = $run.Args
    $link.IconLocation = (Join-Path $PSHOME 'powershell.exe') + ',0'
    $link.WorkingDirectory = $script:AppDir
    $link.Description = 'XP Optimus Bypass'
    $link.Save()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
}
function Start-InstalledXPlane {
    $run = Get-HiddenLaunch (Join-Path $script:AppDir 'launch.ps1')
    Start-Process -FilePath $run.File -ArgumentList $run.Args -WindowStyle Hidden -ErrorAction Stop
}
function Install-XPBypass([string]$SourceApp, [string]$Mode, [string]$XPlanePath, [bool]$DesktopShortcut, [string]$DestinationRoot, [bool]$Reports = $true) {
    $target = Resolve-InstallDestination $DestinationRoot $SourceApp
    $registered = Get-ItemProperty -LiteralPath $script:UninstallKey -ErrorAction SilentlyContinue
    if ($registered -and $registered.InstallLocation -and $target -ine ([string]$registered.InstallLocation).TrimEnd('\')) {
        throw (T '若要更換安裝位置，請先解除安裝，再選擇新位置重新安裝。')
    }
    Set-InstallPaths $target
    if ($Mode -notin @('xplane', 'global')) { throw (T '安裝模式不正確。') }
    $exe = Resolve-XPlane $XPlanePath
    if (-not $exe) { throw (T '請選擇含有 X-Plane.exe 的有效安裝位置。') }
    Assert-OwnedRoot
    Assert-XPlaneClosed
    $required = @('bootstrap.ps1', 'i18n.ps1', 'i18n.json', 'log-viewer.ps1', 'report.ps1', 'core.ps1', 'manager.ps1', 'discovery.ps1', 'launch.ps1', 'xp_optimus_bypass.dll',
                  'xp_optimus_bypass.ini', 'xp_optimus_bypass.json', 'xp_optimus_bypass_global.json')
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceApp $name) -PathType Leaf)) { throw (T '安裝檔不完整，缺少：') + $name }
    }
    $oldEntries = @(Get-LayerEntries)
    $oldArp = $null
    if (Test-Path -LiteralPath $script:UninstallKey) {
        $key = Get-Item -LiteralPath $script:UninstallKey
        if ($key.GetValue('InstallLocation') -ne $script:InstallRoot) { throw (T '既有解除安裝登記指向不同位置，已停止安裝。') }
        $oldArp = @{}
        foreach ($name in $key.GetValueNames()) { $oldArp[$name] = @{Value=$key.GetValue($name); Kind=$key.GetValueKind($name).ToString()} }
    }
    $shortcuts = @(Get-ShortcutPaths)
    $oldLinks = @{}
    foreach ($path in $shortcuts) {
        if (Test-Path -LiteralPath $path) { $oldLinks[$path] = [IO.File]::ReadAllBytes($path) }
    }
    $sourceRoot = Split-Path -Parent $SourceApp
    $oldRootFiles = @{}
    foreach ($name in $script:RootFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $name) -PathType Leaf)) { throw ((T '安裝檔不完整，缺少：') + $name) }
    }
    foreach ($name in $script:ManagedRootFiles) {
        $path = Join-Path $script:InstallRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { $oldRootFiles[$name] = [IO.File]::ReadAllBytes($path) }
    }
    $hadRootLog = Test-Path -LiteralPath $script:RootLog -PathType Leaf
    $hadRoot = Test-Path -LiteralPath $script:InstallRoot
    [IO.Directory]::CreateDirectory($script:InstallRoot) | Out-Null
    $stage = Join-Path $script:InstallRoot ('stage-' + [guid]::NewGuid().ToString('N'))
    $backup = Join-Path $script:InstallRoot ('backup-' + [guid]::NewGuid().ToString('N'))
    $docsBackup = Join-Path $script:InstallRoot ('backup-docs-' + [guid]::NewGuid().ToString('N'))
    $docsActivated = $false
    $docsBackedUp = $false
    $activated = $false
    $backedUp = $false
    $registryTouched = $false
    try {
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        foreach ($file in (Get-ChildItem -LiteralPath $SourceApp -File)) {
            if ($file.Name -notin @('install-state.json', 'xplane_path.txt', 'xp_optimus_bypass.log')) {
                Copy-Item -LiteralPath $file.FullName -Destination $stage -Force
            }
        }
        $docs = Join-Path $sourceRoot 'docs'
        if (-not (Test-Path -LiteralPath $docs -PathType Container)) { throw (T '安裝檔不完整，缺少 docs 資料夾。') }
        Copy-Item -LiteralPath $docs -Destination $stage -Recurse -Force
        foreach ($name in $required) {
            $originalHash = (Get-FileHash -LiteralPath (Join-Path $SourceApp $name) -Algorithm SHA256).Hash
            if ((Get-FileHash -LiteralPath (Join-Path $stage $name) -Algorithm SHA256).Hash -ne $originalHash) { throw (T '檔案複製驗證失敗：') + $name }
        }
        # Preserve the user's graphics settings during reinstall / mode changes.
        $oldIni = Join-Path $script:AppDir 'xp_optimus_bypass.ini'
        if (Test-Path -LiteralPath $oldIni) { Copy-Item -LiteralPath $oldIni -Destination $stage -Force }
        $oldCache = Join-Path $script:AppDir 'xpob_path_cache.txt'
        if (Test-Path -LiteralPath $oldCache) { Copy-Item -LiteralPath $oldCache -Destination $stage -Force }
        Set-Content -LiteralPath (Join-Path $stage 'xplane_path.txt') -Value $exe -Encoding UTF8
        @{mode=$Mode; xplane=$exe; desktopShortcut=$DesktopShortcut; reports=$Reports; installerVersion=$script:InstallerVersion; dllVersion=$script:DllVersion} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'install-state.json') -Encoding UTF8
        if (Test-Path -LiteralPath $script:AppDir) {
            Move-Item -LiteralPath $script:AppDir -Destination $backup
            $backedUp = $true
        }
        Move-Item -LiteralPath $stage -Destination $script:AppDir
        $activated = $true
        if (Test-Path -LiteralPath $script:DocsDir) {
            Move-Item -LiteralPath $script:DocsDir -Destination $docsBackup
            $docsBackedUp = $true
        }
        Move-Item -LiteralPath (Join-Path $script:AppDir 'docs') -Destination $script:DocsDir
        $docsActivated = $true
        $registryTouched = $true
        Clear-LayerEntries
        $manifest = 'xp_optimus_bypass.json'
        if ($Mode -eq 'global') { $manifest = 'xp_optimus_bypass_global.json' }
        New-Item -Path $script:LayerKey -Force | Out-Null
        New-ItemProperty -LiteralPath $script:LayerKey -Name (Join-Path $script:AppDir $manifest) -PropertyType DWord -Value 0 -Force | Out-Null
        if ([IO.Path]::GetFullPath($sourceRoot).TrimEnd('\') -ine [IO.Path]::GetFullPath($script:InstallRoot).TrimEnd('\')) {
            foreach ($name in $script:RootFiles) { Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $script:InstallRoot $name) -Force }
        }
        foreach ($name in $script:LegacyRootFiles) {
            $path = Join-Path $script:InstallRoot $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
        New-Item -Path $script:UninstallKey -Force | Out-Null
        $uninstall = '"' + (Join-Path $PSHOME 'powershell.exe') + '" -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $SourceApp 'manager.ps1') + '" -Uninstall'
        $values = @{DisplayName='XP Optimus Bypass'; DisplayVersion=($script:DllVersion + ' (Installer ' + $script:InstallerVersion + ')'); InstallLocation=$script:InstallRoot;
            UninstallString=$uninstall; DisplayIcon=($exe + ',0'); Publisher='XP Optimus Bypass'; InstallDate=(Get-Date -Format 'yyyyMMdd')}
        foreach ($name in $values.Keys) { New-ItemProperty -LiteralPath $script:UninstallKey -Name $name -Value $values[$name] -PropertyType String -Force | Out-Null }
        foreach ($name in @('NoModify', 'NoRepair')) { New-ItemProperty -LiteralPath $script:UninstallKey -Name $name -Value 1 -PropertyType DWord -Force | Out-Null }
        Write-Shortcut $shortcuts[0] (Join-Path $script:AppDir 'launch.ps1')
        Write-Shortcut $shortcuts[1] (Join-Path $SourceApp 'manager.ps1')
        if ($DesktopShortcut) { Write-Shortcut $shortcuts[2] (Join-Path $script:AppDir 'launch.ps1') }
        elseif (Test-Path -LiteralPath $shortcuts[2]) { Remove-Item -LiteralPath $shortcuts[2] -Force }
        @{product='XPOptimusBypass'; format=1} | ConvertTo-Json | Set-Content -LiteralPath $script:Marker -Encoding UTF8
    } catch {
        $failure = $_.Exception.Message
        $rollbackErrors = New-Object 'System.Collections.Generic.List[string]'
        try {
            if ($docsActivated -and (Test-Path -LiteralPath $script:DocsDir)) { Remove-Item -LiteralPath $script:DocsDir -Recurse -Force }
            if ($docsBackedUp) { Move-Item -LiteralPath $docsBackup -Destination $script:DocsDir }
            if ($activated -and (Test-Path -LiteralPath $script:AppDir)) { Remove-Item -LiteralPath $script:AppDir -Recurse -Force }
            if ($backedUp) { Move-Item -LiteralPath $backup -Destination $script:AppDir }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
        if ($registryTouched) {
            try {
                Restore-LayerEntries $oldEntries
                if (Test-Path -LiteralPath $script:UninstallKey) { Remove-Item -LiteralPath $script:UninstallKey -Recurse -Force }
                if ($oldArp) {
                    New-Item -Path $script:UninstallKey -Force | Out-Null
                    foreach ($name in $oldArp.Keys) { New-ItemProperty -LiteralPath $script:UninstallKey -Name $name -Value $oldArp[$name].Value -PropertyType $oldArp[$name].Kind -Force | Out-Null }
                }
                foreach ($path in $shortcuts) {
                    if ($oldLinks.ContainsKey($path)) { [IO.File]::WriteAllBytes($path, $oldLinks[$path]) }
                    elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
                }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        try {
            foreach ($name in $script:ManagedRootFiles) {
                $path = Join-Path $script:InstallRoot $name
                if ($oldRootFiles.ContainsKey($name)) { [IO.File]::WriteAllBytes($path, $oldRootFiles[$name]) }
                elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            }
            if (-not $hadRootLog -and (Test-Path -LiteralPath $script:RootLog)) { Remove-Item -LiteralPath $script:RootLog -Force }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
        if (-not $hadRoot -and $rollbackErrors.Count -eq 0) {
            Remove-Item -LiteralPath $script:InstallRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
        if ($rollbackErrors.Count -gt 0) { throw ($failure + (T "`r`n復原未完全成功，請保留安裝資料夾並重新執行安裝：") + ($rollbackErrors -join '; ')) }
        throw ($failure + (T "`r`n安裝未完成，已復原先前狀態。"))
    } finally {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $docsBackup) { Remove-Item -LiteralPath $docsBackup -Recurse -Force -ErrorAction SilentlyContinue }
}
function Uninstall-XPBypass {
    Assert-OwnedRoot
    Assert-XPlaneClosed
    if (-not (Test-Path -LiteralPath $script:Marker)) { throw (T '找不到此工具的安裝紀錄。') }
    $dll = Join-Path $script:AppDir 'xp_optimus_bypass.dll'
    if (Test-Path -LiteralPath $dll) {
        try { $handle = [IO.File]::Open($dll, 'Open', 'ReadWrite', 'None'); $handle.Dispose() }
        catch { throw (T 'DLL 仍被程式使用，請關閉 Vulkan 程式，必要時重新開機後再解除安裝。') }
    }
    $oldEntries = @(Get-LayerEntries)
    $shortcuts = @(Get-ShortcutPaths)
    $oldLinks = @{}
    foreach ($path in $shortcuts) {
        if (Test-Path -LiteralPath $path) { $oldLinks[$path] = [IO.File]::ReadAllBytes($path) }
    }
    $oldArp = @{}
    if (Test-Path -LiteralPath $script:UninstallKey) {
        $key = Get-Item -LiteralPath $script:UninstallKey
        if ($key.GetValue('InstallLocation') -ne $script:InstallRoot) { throw (T '解除安裝登記指向其他位置，已停止操作。') }
        foreach ($name in $key.GetValueNames()) { $oldArp[$name] = @{Value=$key.GetValue($name); Kind=$key.GetValueKind($name).ToString()} }
    }
    $oldRootFiles = @{}
    foreach ($name in $script:ManagedRootFiles) {
        $path = Join-Path $script:InstallRoot $name
        if (Test-Path -LiteralPath $path -PathType Leaf) { $oldRootFiles[$name] = [IO.File]::ReadAllBytes($path) }
    }
    $quarantine = Join-Path $script:InstallRoot ('remove-' + [guid]::NewGuid().ToString('N'))
    $moved = $false
    $docsMoved = $false
    try {
        if (Test-Path -LiteralPath $script:AppDir) {
            Move-Item -LiteralPath $script:AppDir -Destination $quarantine
            $moved = $true
        }
        if (Test-Path -LiteralPath $script:DocsDir) {
            [IO.Directory]::CreateDirectory($quarantine) | Out-Null
            Move-Item -LiteralPath $script:DocsDir -Destination (Join-Path $quarantine 'root-docs')
            $docsMoved = $true
        }
        foreach ($name in @('xp_optimus_bypass.json', 'xp_optimus_bypass_global.json')) {
            $valueName = Join-Path $script:AppDir $name
            if (@($oldEntries | Where-Object { $_.Name -eq $valueName }).Count -gt 0) {
                Remove-ItemProperty -LiteralPath $script:LayerKey -Name $valueName -ErrorAction Stop
            }
        }
        foreach ($path in $shortcuts) { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force } }
        if (Test-Path -LiteralPath $script:UninstallKey) { Remove-Item -LiteralPath $script:UninstallKey -Recurse -Force }
        foreach ($name in $oldRootFiles.Keys) { Remove-Item -LiteralPath (Join-Path $script:InstallRoot $name) -Force }
    } catch {
        $failure = $_.Exception.Message
        try {
            if ($docsMoved) { Move-Item -LiteralPath (Join-Path $quarantine 'root-docs') -Destination $script:DocsDir }
            if ($moved) { Move-Item -LiteralPath $quarantine -Destination $script:AppDir }
            Restore-LayerEntries $oldEntries
            foreach ($name in $oldRootFiles.Keys) { [IO.File]::WriteAllBytes((Join-Path $script:InstallRoot $name), $oldRootFiles[$name]) }
            foreach ($path in $oldLinks.Keys) { [IO.File]::WriteAllBytes($path, $oldLinks[$path]) }
            if ($oldArp.Count -gt 0) {
                New-Item -Path $script:UninstallKey -Force | Out-Null
                foreach ($name in $oldArp.Keys) { New-ItemProperty -LiteralPath $script:UninstallKey -Name $name -Value $oldArp[$name].Value -PropertyType $oldArp[$name].Kind -Force | Out-Null }
            }
        } catch { throw ($failure + (T "`r`n復原未完全成功，請保留資料夾並重新執行安裝：") + $_.Exception.Message) }
        throw ($failure + (T "`r`n解除安裝未完成，已復原先前狀態。"))
    }
    # Deletion is last: the active registration is already removed. The only X-Plane file touched is a
    # steam_appid.txt that this tool created itself (see Enable-SteamDirectLaunch).
    Remove-SteamDirectLaunch $script:InstallRoot
    if ($moved -or $docsMoved) { Remove-Item -LiteralPath $quarantine -Recurse -Force -ErrorAction SilentlyContinue }
    $menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'XP Optimus Bypass'
    if ((Test-Path -LiteralPath $menu) -and @(Get-ChildItem -LiteralPath $menu -Force).Count -eq 0) { Remove-Item -LiteralPath $menu -Force -ErrorAction SilentlyContinue }
    foreach ($name in @('xp_optimus_bypass.log', 'launcher-diagnostics.log')) {
        $path = Join-Path $script:InstallRoot $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }
    }
    $remaining = @(Get-ChildItem -LiteralPath $script:InstallRoot -Force | Where-Object { $_.FullName -ne $script:Marker })
    if ($remaining.Count -gt 0) {
        return ((T '解除安裝完成。部分檔案仍留在下列位置，關閉相關程式後可手動刪除：') + "`r`n" + $script:InstallRoot)
    }
    Remove-Item -LiteralPath $script:Marker -Force
    Remove-Item -LiteralPath $script:InstallRoot -Force
    return (T '解除安裝完成。')
}
