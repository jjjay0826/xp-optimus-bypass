# Anonymous compatibility reports: built from the DLL log, sanitized, shown to the user, and (if allowed) sent once
# after X-Plane closes. Only the fields below are ever sent; no paths, user, computer or hardware serial numbers.

# Set by the maintainer when the report server is online (see server/README.md). Empty = never send.
$script:ReportEndpoint = 'https://xpob-reports.jjjay0826.workers.dev/v1/report'
$script:ReportSchema = 1

function Get-CleanText([string]$Text, [int]$Max = 120) {
    if ([string]::IsNullOrWhiteSpace($Text)) { return '' }
    $t = $Text -replace '[A-Za-z]:\\[^\s|]*', '<path>' -replace '\\\\[^\s|]+', '<path>' -replace '%USERPROFILE%[^\s|]*', '<path>'
    $t = ($t -replace '[^\x20-\x7E]', '').Trim()
    if ($t.Length -gt $Max) { $t = $t.Substring(0, $Max) }
    return $t
}

function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return [math]::Round($s[($n - 1) / 2], 2) }
    return [math]::Round(($s[$n / 2 - 1] + $s[$n / 2]) / 2, 2)
}

# Reads the log and returns an ordered dictionary with only whitelisted, sanitized fields.
function Get-XPReport([string]$Root, [string]$InstallMode = '', [string]$InstallerVersion = '') {
    $log = Join-Path $Root 'xp_optimus_bypass.log'
    if (-not (Test-Path -LiteralPath $log -PathType Leaf)) { return $null }
    $lines = @()
    $stream = New-Object IO.FileStream($log, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8, $true)
        while ($null -ne ($l = $reader.ReadLine())) { $lines += ($l -replace '^\[\d\d:\d\d:\d\d\] ', '') }
        $reader.Dispose()
    } finally { $stream.Dispose() }
    if ($lines.Count -eq 0) { return $null }

    $r = [ordered]@{
        schema = $script:ReportSchema; layer = ''; installer = (Get-CleanText $InstallerVersion 20); install_mode = (Get-CleanText $InstallMode 10)
        windows = ''; xplane = ''; render_gpu = ''; render_driver = ''; intel_gpu = ''; intel_driver = ''
        screens = 0; intel_screens = 0; window_on_intel = $null; mode = ''; path_pref = ''; test_switches = $false
        result = 'unknown'; reason = ''; mem = ''; conv = ''; probe = $null; fps = [ordered]@{}; overlays = @(); errors = @()
    }
    $fps = @{}
    $lastPath = ''
    foreach ($line in $lines) {
        if ($line -match '^XP Optimus Bypass (\S+) loaded in X-Plane\.exe \| mode=(\S+)(?: path=(\S+))?') {
            $r.layer = Get-CleanText $Matches[1] 20; $r.mode = Get-CleanText $Matches[2] 10
            if ($Matches[3]) { $r.path_pref = Get-CleanText $Matches[3] 10 }
        }
        elseif ($line -match '^Windows (\d+\.\d+) build (\d+)') { $r.windows = $Matches[1] + '.' + $Matches[2] }
        elseif ($line -match '^X-Plane\.exe file version ([\d.]+)') { $r.xplane = $Matches[1] }
        elseif ($line -match '^render GPU: (.+?), driver (\S+) \|') { $r.render_gpu = Get-CleanText $Matches[1] 80; $r.render_driver = Get-CleanText $Matches[2] 20 }
        elseif ($line -match '^Intel: using (.+?), driver (\S+?),') { $r.intel_gpu = Get-CleanText $Matches[1] 80; $r.intel_driver = Get-CleanText $Matches[2] 20 }
        elseif ($line -match '^\[diag\] screen .*driven by Intel \(EnumDisplayDevices\): (yes|no)(.*)$') {
            $isIntel = $Matches[1] -eq 'yes'
            $rest = $Matches[2]
            $r.screens++
            if ($isIntel) { $r.intel_screens++ }
            if ($rest -like '*X-Plane window is here*') { $r.window_on_intel = $isIntel }
        }
        elseif ($line -match '^\[diag\] TEST SWITCHES') { $r.test_switches = $true }
        elseif ($line -match '^\[diag\] bypass ready: memory (.+?) \| GeForce conversion (.+?) \|') {
            $r.mem = Get-CleanText $Matches[1] 60; $r.conv = Get-CleanText $Matches[2] 30
        }
        elseif ($line -match '^\[diag\] overlay-like module loaded: (.+)$') {
            $name = Get-CleanText ([IO.Path]::GetFileName($Matches[1])) 60
            if ($name -and $r.overlays -notcontains $name -and $r.overlays.Count -lt 10) { $r.overlays += $name }
        }
        elseif ($line -match '^\[diag\] implicit layer \(\w+\): (.+?)( \(disabled\))?$') {
            if (-not $Matches[2]) {
                $name = Get-CleanText ([IO.Path]::GetFileName($Matches[1])) 60
                if ($name -and $name -notlike 'xp_optimus_bypass*' -and $r.overlays -notcontains $name -and $r.overlays.Count -lt 10) { $r.overlays += $name }
            }
        }
        elseif ($line -match '^path -> (\w+) \(') { $lastPath = $Matches[1] }
        elseif ($line -match '^bypass unavailable: (.+)$') { $r.reason = Get-CleanText $Matches[1] }
        elseif ($line -match '^(native path failed|Intel side not working|virtual swapchain setup FAILED|no Intel GPU in this system|not an NVIDIA GPU|no free queue)') {
            $r.reason = Get-CleanText $line
        }
        elseif ($line -match '^selected (\w+): p50 native ([\d.]+) -> bypass ([\d.]+) ms, p95 native ([\d.]+) -> bypass ([\d.]+)') {
            $r.probe = [ordered]@{ selected = $Matches[1]; native_p50 = [double]$Matches[2]; bypass_p50 = [double]$Matches[3]
                                   native_p95 = [double]$Matches[4]; bypass_p95 = [double]$Matches[5] }
        }
        elseif ($line -match '^FPS ([\d.]+) \| path=(\S+).*?frame ms p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+) over50 (\d+)') {
            $p = $Matches[2] -replace '[^a-z-]', ''
            if (-not $fps.ContainsKey($p)) { $fps[$p] = New-Object 'System.Collections.Generic.List[object]' }
            $fps[$p].Add([pscustomobject]@{ fps = [double]$Matches[1]; p50 = [double]$Matches[3]; p95 = [double]$Matches[4]
                                            p99 = [double]$Matches[5]; over50 = [double]$Matches[7] })
        }
        elseif ($line -match 'failed \(-?\d+\)' -and $r.errors.Count -lt 5) {
            $e = Get-CleanText $line 100
            if ($r.errors -notcontains $e) { $r.errors += $e }
        }
    }
    foreach ($p in $fps.Keys) {
        $w = $fps[$p].ToArray()
        if ($w.Count -gt 1) { $w = $fps[$p].GetRange(1, $w.Count - 1).ToArray() }  # the first window includes loading
        $r.fps[$p] = [ordered]@{
            windows = $w.Count
            fps = Get-Median ([double[]]@($w | ForEach-Object { $_.fps }))
            p50_ms = Get-Median ([double[]]@($w | ForEach-Object { $_.p50 }))
            p95_ms = Get-Median ([double[]]@($w | ForEach-Object { $_.p95 }))
            p99_ms = Get-Median ([double[]]@($w | ForEach-Object { $_.p99 }))
            over50_per_300 = Get-Median ([double[]]@($w | ForEach-Object { $_.over50 }))
        }
    }
    if ($r.mode -eq 'off') { $r.result = 'disabled' }
    elseif ($lastPath) { $r.result = $lastPath }
    elseif ($fps.ContainsKey('real-swapchain')) { $r.result = 'real-swapchain' }
    if ($r.reason -match '^native path failed') { $r.result = 'real-swapchain' }
    return $r
}

function ConvertTo-XPReportJson($Report) { return ($Report | ConvertTo-Json -Depth 5 -Compress) }

# Human-readable version for the preview window and for pasting into Reddit / forums / GitHub.
function Get-XPReportText($Report, [string]$Root) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.AppendLine('XP Optimus Bypass report')
    foreach ($k in $Report.Keys) {
        $v = $Report[$k]
        if ($v -is [Collections.IDictionary]) { [void]$sb.AppendLine(('{0}: {1}' -f $k, ($v | ConvertTo-Json -Depth 4 -Compress))) }
        elseif ($v -is [Array]) { [void]$sb.AppendLine(('{0}: {1}' -f $k, ($v -join '; '))) }
        else { [void]$sb.AppendLine(('{0}: {1}' -f $k, $v)) }
    }
    return $sb.ToString()
}

function Send-XPReport($Report, [string]$Diagnostics) {
    $note = { param($t) if ($Diagnostics) { ('[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $t) | Add-Content -LiteralPath $Diagnostics -Encoding UTF8 } }
    if ([string]::IsNullOrWhiteSpace($script:ReportEndpoint)) { & $note 'Report not sent: no report server configured in this build.'; return $false }
    if (-not $Report) { & $note 'Report not sent: no DLL log from this session.'; return $false }
    $body = ConvertTo-XPReportJson $Report
    if ($body.Length -gt 4000) { & $note 'Report not sent: too large.'; return $false }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $bytes = [Text.Encoding]::UTF8.GetBytes($body)
        Invoke-RestMethod -Uri $script:ReportEndpoint -Method Post -ContentType 'application/json' -Body $bytes -TimeoutSec 10 -UseBasicParsing | Out-Null
        & $note 'Anonymous compatibility report sent.'
        return $true
    } catch {
        & $note ('Report not sent: ' + $_.Exception.Message)
        return $false
    }
}

# Preview window: what would be sent, plus copy and GitHub buttons.
function Show-XPReportWindow([string]$Root, $Owner, [string]$InstallMode = '', [string]$InstallerVersion = '') {
    $report = Get-XPReport $Root $InstallMode $InstallerVersion
    $window = New-Object Windows.Forms.Form
    $window.Text = (T '相容性回報內容')
    $window.Size = New-Object Drawing.Size(860, 600)
    $window.StartPosition = 'CenterParent'
    $box = New-Object Windows.Forms.TextBox
    $box.Multiline = $true; $box.ReadOnly = $true; $box.ScrollBars = 'Both'; $box.WordWrap = $false; $box.Dock = 'Fill'
    $box.Font = New-Object Drawing.Font('Consolas', 10)
    if ($report) { $text = Get-XPReportText $report $Root }
    else { $text = (T '還沒有這次執行的紀錄。用專用入口啟動 X-Plane 飛一下，關閉後再開啟這個視窗。') }
    $box.Text = (T '自動回報只會送出下面這些欄位（不含路徑、使用者名稱、電腦名稱）。') + "`r`n`r`n" + $text
    $panel = New-Object Windows.Forms.FlowLayoutPanel
    $panel.Dock = 'Bottom'; $panel.Height = 46; $panel.FlowDirection = 'RightToLeft'
    $closeBtn = New-Object Windows.Forms.Button; $closeBtn.Text = (T '關閉'); $closeBtn.Width = 120; $closeBtn.Height = 34
    $copyBtn = New-Object Windows.Forms.Button; $copyBtn.Text = (T '複製'); $copyBtn.Width = 120; $copyBtn.Height = 34
    $ghBtn = New-Object Windows.Forms.Button; $ghBtn.Text = (T '在 GitHub 回報'); $ghBtn.Width = 170; $ghBtn.Height = 34
    $copyBtn.Enabled = $null -ne $report; $ghBtn.Enabled = $null -ne $report
    $panel.Controls.AddRange(@($closeBtn, $copyBtn, $ghBtn))
    $window.Controls.Add($box); $window.Controls.Add($panel)
    $closeBtn.Add_Click({ $window.Close() }.GetNewClosure())
    $copyBtn.Add_Click({ [Windows.Forms.Clipboard]::SetText($text) }.GetNewClosure())
    $ghBtn.Add_Click({
        [Windows.Forms.Clipboard]::SetText($text)
        $body = [Uri]::EscapeDataString("Report (also copied to the clipboard):`n`n" + $text)
        if ($body.Length -gt 6000) { $body = [Uri]::EscapeDataString('Please paste the report from the clipboard here.') }
        Start-Process ('https://github.com/jjjay0826/xp-optimus-bypass/issues/new?labels=report&title=' + [Uri]::EscapeDataString('Compatibility report') + '&body=' + $body)
    }.GetNewClosure())
    try { [void]$window.ShowDialog($Owner) } finally { $window.Dispose() }
}
