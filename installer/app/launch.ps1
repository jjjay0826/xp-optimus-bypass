$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
try {
    . (Join-Path $PSScriptRoot 'i18n.ps1')
    . (Join-Path $PSScriptRoot 'discovery.ps1')
    $cfg = Join-Path $PSScriptRoot 'xplane_path.txt'
    $exe = $null
    if (Test-Path -LiteralPath $cfg) { $exe = Resolve-XPlane (Get-Content -LiteralPath $cfg -Encoding UTF8 -TotalCount 1) }
    if (-not $exe) {
        [void][Windows.Forms.MessageBox]::Show((T 'X-Plane 路徑失效，請在接下來的管理視窗重新選擇路徑並按「安裝 / 套用」。'), 'XP Optimus Bypass')
        Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList ('-NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\0.9.3\app\manager.ps1') + '"')
        exit 1
    }
    if (Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue) { throw (T 'X-Plane 已在執行，請使用目前的遊戲視窗。') }
    $root = Split-Path -Parent $PSScriptRoot
    $rootLog = Join-Path $root 'xp_optimus_bypass.log'
    $diagnostics = Join-Path $root 'launcher-diagnostics.log'
    function Write-Diagnostic([string]$Text) {
        ('[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Text) | Add-Content -LiteralPath $diagnostics -Encoding UTF8
    }
    Write-Diagnostic ('Launch request: ' + $exe)
    Write-Diagnostic ('Expected DLL: ' + (Join-Path $PSScriptRoot 'xp_optimus_bypass.dll'))
    Write-Diagnostic ('Actual DLL log: ' + $rootLog)
    if ($env:XP_OPTIMUS_BYPASS_DISABLE -eq '1') {
        throw 'XP_OPTIMUS_BYPASS_DISABLE=1 is set. Remove that environment override to load the layer. / 請先移除停用 layer 的環境變數。'
    }
    $layerKey = 'HKCU:\Software\Khronos\Vulkan\ImplicitLayers'
    if (-not (Test-Path -LiteralPath $layerKey)) { throw 'No Vulkan layer registration. Install / Apply first. / 請先安裝或套用。' }
    $key = Get-Item -LiteralPath $layerKey
    $registered = @($key.GetValueNames() | Where-Object {
        $_ -in @((Join-Path $PSScriptRoot 'xp_optimus_bypass.json'), (Join-Path $PSScriptRoot 'xp_optimus_bypass_global.json')) -and $key.GetValue($_) -eq 0
    })
    if ($registered.Count -eq 0) { throw 'This installed DLL is not registered. Click Install / Apply. / 此 DLL 未登記，請先套用。' }
    Write-Diagnostic ('Registered manifest: ' + ($registered -join ', '))
    if ((Get-SteamAppId $exe) -and -not (Get-Process -Name 'steam' -ErrorAction SilentlyContinue)) {
        throw (T '請先開啟 Steam，再啟動 X-Plane。')
    }
    try { Write-Diagnostic ('Steam direct start: ' + (Enable-SteamDirectLaunch $exe $root)) }
    catch { Write-Diagnostic ('Steam direct start: could not create steam_appid.txt: ' + $_.Exception.Message) }
    $env:XP_OPTIMUS_BYPASS_LOG = $rootLog
    $env:XP_OPTIMUS_BYPASS_ENABLE = '1'
    $startedAt = Get-Date
    $process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru -ErrorAction Stop
    Write-Diagnostic ('Started process ID: ' + $process.Id)
    # This launcher runs separately from the GUI. Allow startup time, then report missing DLL output.
    $written = $false
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $rootLog) {
            $info = Get-Item -LiteralPath $rootLog
            if ($info.Length -gt 0 -and $info.LastWriteTime -ge $startedAt.AddSeconds(-1)) { $written = $true; break }
        }
        $process.Refresh()
        if ($process.HasExited) { break }
    }
    if ($written) { Write-Diagnostic 'DLL log is receiving records.' }
    else {
        $loaded = 'unknown'
        try { $loaded = [string](@($process.Modules | Where-Object { $_.FileName -ieq (Join-Path $PSScriptRoot 'xp_optimus_bypass.dll') }).Count -gt 0) } catch { }
        Write-Diagnostic ('No fresh DLL log. Process exited: ' + $process.HasExited + '; expected DLL loaded: ' + $loaded)
        $relaunched = @(Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $process.Id })
        if ($process.HasExited -and $relaunched.Count -gt 0) {
            Write-Diagnostic ('Steam restarted X-Plane as process ' + $relaunched[0].Id + '; the layer is not loaded in it.')
            [void][Windows.Forms.MessageBox]::Show(((T 'Steam 重新啟動了 X-Plane，這次 layer 沒有載入。請關閉 X-Plane 後再啟動一次；若仍發生，請回報 launcher-diagnostics.log：') + "`r`n" + $diagnostics), 'XP Optimus Bypass', 'OK', 'Warning')
        } else {
        [void][Windows.Forms.MessageBox]::Show(('No fresh DLL log was detected. This does not prove the bypass is active.' + "`r`n" + '未收到 DLL 新紀錄，請查看載入診斷。' + "`r`n" + $diagnostics), 'XP Optimus Bypass', 'OK', 'Warning')
        }
    }
    # Anonymous compatibility report, once, after X-Plane has closed (if allowed in the manager).
    $state = $null
    $stateFile = Join-Path $PSScriptRoot 'install-state.json'
    if (Test-Path -LiteralPath $stateFile) { try { $state = Get-Content -LiteralPath $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { } }
    if ($state -and $state.reports -eq $true) {
        . (Join-Path $PSScriptRoot 'report.ps1')
        # Steam may restart X-Plane as a new process, so wait until no X-Plane process is left.
        Start-Sleep -Seconds 20
        while (Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 10 }
        $logInfo = $null
        if (Test-Path -LiteralPath $rootLog) { $logInfo = Get-Item -LiteralPath $rootLog }
        if ($logInfo -and $logInfo.LastWriteTime -ge $startedAt.AddSeconds(-1)) {
            $report = Get-XPReport (Split-Path -Parent $PSScriptRoot) ([string]$state.mode) ([string]$state.installerVersion)
            [void](Send-XPReport $report $diagnostics)
        } else {
            Write-Diagnostic 'Report not sent: no DLL log from this session.'
        }
    }
} catch {
    [void][Windows.Forms.MessageBox]::Show(((T '無法啟動 X-Plane：') + $_.Exception.Message), 'XP Optimus Bypass', 'OK', 'Error')
    exit 1
}
