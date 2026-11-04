function Show-XPLogWindow([string]$Root, $Owner) {
    $window = New-Object Windows.Forms.Form
    $window.Text = 'Live logs / 即時紀錄 — XP Optimus Bypass'
    $window.Size = New-Object Drawing.Size(940, 620)
    $window.StartPosition = 'CenterParent'
    $tabs = New-Object Windows.Forms.TabControl
    $tabs.Dock = 'Fill'
    $window.Controls.Add($tabs)
    $views = @{}
    foreach ($name in @('xp_optimus_bypass.log','launcher-diagnostics.log')) {
        $page = New-Object Windows.Forms.TabPage
        $page.Text = $name
        $box = New-Object Windows.Forms.TextBox
        $box.Multiline = $true; $box.ReadOnly = $true; $box.ScrollBars = 'Both'
        $box.WordWrap = $false; $box.Dock = 'Fill'
        $box.Font = New-Object Drawing.Font('Consolas', 10)
        $page.Controls.Add($box); $tabs.TabPages.Add($page)
        $views[$name] = $box
    }
    $pathLabel = New-Object Windows.Forms.Label
    $pathLabel.Dock = 'Bottom'; $pathLabel.Height = 45
    $pathLabel.Text = $Root + "`r`nRefresh: 1 second / 每秒更新；FPS samples: every 300 frames / FPS 每 300 幀統計。"
    $window.Controls.Add($pathLabel)
    $reportButton = New-Object Windows.Forms.Button
    $reportButton.Text = 'Report / 產生回報'
    $reportButton.Dock = 'Bottom'; $reportButton.Height = 34
    $reportButton.Add_Click({
        $state = Get-InstallState
        $mode = ''; $ver = ''
        if ($state) { $mode = [string]$state.mode; $ver = [string]$state.installerVersion }
        Show-XPReportWindow $Root $window $mode $ver
    }.GetNewClosure())
    $window.Controls.Add($reportButton)
    $refresh = {
        foreach ($name in $views.Keys) {
            $path = Join-Path $Root $name
            $stream = $null; $reader = $null
            try {
                if (-not (Test-Path -LiteralPath $path)) {
                    $content = 'No records yet. Start X-Plane using the dedicated launcher. / 尚無紀錄，請從專用入口啟動 X-Plane。'
                } else {
                    $stream = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                    if ($stream.Length -gt 2097152) { [void]$stream.Seek(-2097152, [IO.SeekOrigin]::End) }
                    $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8, $true)
                    $content = $reader.ReadToEnd()
                    if (-not $content) { $content = 'The DLL has not written records. Check launcher-diagnostics.log. / DLL 尚未寫入，請查看載入診斷。' }
                }
                if ($views[$name].Text -cne $content) {
                    $views[$name].Text = $content
                    $views[$name].SelectionStart = $views[$name].TextLength
                    $views[$name].ScrollToCaret()
                }
            } catch { $views[$name].Text = $_.Exception.Message }
            finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
        }
    }.GetNewClosure()
    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 1000
    $timer.Add_Tick($refresh)
    try { & $refresh; $timer.Start(); [void]$window.ShowDialog($Owner) }
    finally { $timer.Stop(); $timer.Dispose(); $window.Dispose() }
}
