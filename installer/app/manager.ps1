[CmdletBinding()]
param([switch]$Uninstall, [ValidateSet('en','zh-TW')][string]$Language = 'en')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
[Windows.Forms.Application]::SetUnhandledExceptionMode('CatchException')
[Windows.Forms.Application]::add_ThreadException({
    param($sender, $eventArgs)
    $log = Join-Path $env:LOCALAPPDATA 'XP Optimus Bypass Installer\installer-error.log'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $log)) | Out-Null
    $eventArgs.Exception.ToString() | Add-Content -LiteralPath $log -Encoding UTF8
    [void][Windows.Forms.MessageBox]::Show(($eventArgs.Exception.Message + "`r`nLog: " + $log), 'XP Optimus Bypass', 'OK', 'Error')
})
$script:Language = $Language
. (Join-Path $PSScriptRoot 'i18n.ps1')
. (Join-Path $PSScriptRoot 'core.ps1')
. (Join-Path $PSScriptRoot 'discovery.ps1')
. (Join-Path $PSScriptRoot 'log-viewer.ps1')
# Prevent concurrent installers/managers from changing the same registration.
$mutex = New-Object Threading.Mutex($false, 'Local\XPOptimusBypassInstaller')
$ownsMutex = $false
try { $ownsMutex = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
if (-not $ownsMutex) {
    [void][Windows.Forms.MessageBox]::Show((T '安裝或管理介面已開啟，請使用原本的視窗。'), 'XP Optimus Bypass')
    $mutex.Dispose()
    exit 1
}
$script:SearchJob = $null
$script:Busy = $false
$form = New-Object Windows.Forms.Form
$form.Text = (T 'XP Optimus Bypass — 安裝與管理')
$form.ClientSize = New-Object Drawing.Size(760, 670)
$form.MinimumSize = New-Object Drawing.Size(776, 709)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Microsoft JhengHei UI', 10)
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
$form.AutoScaleMode = 'Dpi'
$form.MaximizeBox = $false
function Add-Label([string]$Text, [int]$X, [int]$Y, [int]$W, [int]$H) {
    $c = New-Object Windows.Forms.Label
    $c.Text = $Text; $c.SetBounds($X, $Y, $W, $H)
    $form.Controls.Add($c)
    return $c
}
function Add-Button([string]$Text, [int]$X, [int]$Y, [int]$W) {
    $c = New-Object Windows.Forms.Button
    $c.Text = $Text; $c.SetBounds($X, $Y, $W, 34)
    $form.Controls.Add($c)
    return $c
}
$title = Add-Label 'XP Optimus Bypass' 24 18 470 35
$title.Font = New-Object Drawing.Font('Microsoft JhengHei UI', 18, [Drawing.FontStyle]::Bold)
# Language selection applies live; each fresh launch defaults to English.
$languageChoice = New-Object Windows.Forms.ComboBox
$languageChoice.SetBounds(565, 25, 165, 28)
$languageChoice.DropDownStyle = 'DropDownList'
[void]$languageChoice.Items.Add('English')
[void]$languageChoice.Items.Add('繁體中文')
$languageChoice.SelectedIndex = 0
if ($script:Language -eq 'zh-TW') { $languageChoice.SelectedIndex = 1 }
$form.Controls.Add($languageChoice)
$subtitle = Add-Label (T '安裝介面 1.5  ·  核心 DLL 0.9.1.1-beta  ·  目前 Windows 使用者') 26 58 710 28
$stateLabel = Add-Label '' 26 91 710 28
$null = Add-Label (T '1. X-Plane 安裝位置') 26 127 710 26
$pathCombo = New-Object Windows.Forms.ComboBox
$pathCombo.SetBounds(26, 158, 550, 30)
$pathCombo.DropDownStyle = 'DropDown'
$pathCombo.DropDownWidth = 850
$form.Controls.Add($pathCombo)
$browse = Add-Button (T '瀏覽…') 588 156 144
$search = Add-Button (T '重新自動尋找') 26 197 150
$searchLabel = Add-Label '' 188 202 544 45
$null = Add-Label (T '2. 選擇安裝模式') 26 250 710 26
$modeGroup = New-Object Windows.Forms.GroupBox
$modeGroup.SetBounds(26, 280, 706, 132)
$form.Controls.Add($modeGroup)
$xplaneRadio = New-Object Windows.Forms.RadioButton
$xplaneRadio.Text = (T '僅 X-Plane（建議）— 從專用捷徑啟動時載入')
$xplaneRadio.SetBounds(16, 20, 674, 28)
$xplaneRadio.Checked = $true
$modeGroup.Controls.Add($xplaneRadio)
$globalRadio = New-Object Windows.Forms.RadioButton
$globalRadio.Text = (T '全域 — 所有 Vulkan 程式載入，僅在 X-Plane 中作用')
$globalRadio.SetBounds(16, 53, 674, 28)
$modeGroup.Controls.Add($globalRadio)
$warning = New-Object Windows.Forms.Label
$warning.Text = (T '全域模式可能影響反作弊相容性；玩此類 Vulkan 遊戲前請改用僅 X-Plane 模式。')
$warning.ForeColor = [Drawing.Color]::FromArgb(150, 75, 0)
$warning.SetBounds(19, 88, 670, 35)
$modeGroup.Controls.Add($warning)
$null = Add-Label (T '3. 安裝位置（固定，無需系統管理員權限）') 26 424 710 26
$location = New-Object Windows.Forms.TextBox
$location.ReadOnly = $true; $location.Text = $script:InstallRoot
$location.SetBounds(26, 455, 706, 29)
$form.Controls.Add($location)
$desktop = New-Object Windows.Forms.CheckBox
$desktop.Text = (T '建立桌面啟動捷徑'); $desktop.Checked = $true
$desktop.SetBounds(26, 494, 310, 30); $form.Controls.Add($desktop)
$runAfter = New-Object Windows.Forms.CheckBox
$runAfter.Text = (T '安裝完成後啟動 X-Plane'); $runAfter.Checked = $false
$runAfter.SetBounds(350, 494, 380, 30); $form.Controls.Add($runAfter)
$status = Add-Label (T '請確認路徑與模式，再按「安裝」。') 26 535 706 43
$install = Add-Button (T '安裝') 26 588 130
$launch = Add-Button (T '啟動 X-Plane') 166 588 130
$logs = Add-Button (T '紀錄與說明') 306 588 130
$remove = Add-Button (T '解除安裝') 446 588 130
$close = Add-Button (T '關閉') 602 588 130
$null = Add-Label (T '安裝後可刪除下載資料夾；請保留固定安裝位置。模式變更需重新啟動 X-Plane。') 26 633 706 28

function Show-Error([string]$Message) {
    [void][Windows.Forms.MessageBox]::Show($form, $Message, (T '操作未完成'), 'OK', 'Error')
}
function Update-State {
    $state = Get-InstallState
    $installed = $null -ne $state
    $remove.Enabled = $installed; $launch.Enabled = $installed; $logs.Enabled = $installed
    if ($installed) {
        $modeName = (T '僅 X-Plane')
        if ($state.mode -eq 'global') { $modeName = (T '全域') }
        $stateLabel.Text = (T '目前狀態：已安裝  /  ') + $modeName
        $install.Text = (T '安裝 / 套用')
    } else { $stateLabel.Text = (T '目前狀態：尚未正式安裝'); $install.Text = (T '安裝') }
}
function Stop-Search {
    if ($script:SearchJob) {
        Stop-Job -Job $script:SearchJob -ErrorAction SilentlyContinue
        Remove-Job -Job $script:SearchJob -Force -ErrorAction SilentlyContinue
        $script:SearchJob = $null
    }
    $search.Enabled = $true
}
function Begin-Search {
    Stop-Search
    $search.Enabled = $false
    $searchLabel.Text = (T '搜尋中…若常見位置未找到，會繼續搜尋本機磁碟。')
    $script:SearchJob = Start-Job -ArgumentList (Join-Path $PSScriptRoot 'discovery.ps1'), $pathCombo.Text -ScriptBlock {
        param($DiscoveryScript, $KnownPath)
        $ErrorActionPreference = 'Stop'
        . $DiscoveryScript
        Add-Candidate $KnownPath
        Find-Quick
        if ($candidates.Count -eq 0) { Find-OnLocalDrives }
        foreach ($candidate in $candidates) { Write-Output $candidate }
    }
}
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    if (-not $script:SearchJob -or $script:SearchJob.State -in @('Running', 'NotStarted')) { return }
    try {
        $failed = $script:SearchJob.State -ne 'Completed'
        $found = @(Receive-Job -Job $script:SearchJob -ErrorAction SilentlyContinue)
        $existing = $pathCombo.Text
        foreach ($value in $found) {
            $valid = Resolve-XPlane ([string]$value)
            if ($valid -and -not $pathCombo.Items.Contains($valid)) { [void]$pathCombo.Items.Add($valid) }
        }
        if ([string]::IsNullOrWhiteSpace($existing) -and $pathCombo.Items.Count -gt 0) { $pathCombo.SelectedIndex = 0 }
        $searchLabel.Text = (T '已找到 ') + $pathCombo.Items.Count + (T ' 個位置。多套安裝可從下拉選單選擇。')
        if ($pathCombo.Items.Count -eq 0) { $searchLabel.Text = (T '未找到。可按「瀏覽」選取 X-Plane.exe，或貼上路徑。') }
        if ($failed) { $searchLabel.Text = (T '自動搜尋未完成；請按「瀏覽」選取 X-Plane.exe。') }
    } finally { Stop-Search }
})
$browse.Add_Click({
    $dialog = New-Object Windows.Forms.OpenFileDialog
    $dialog.Title = (T '選擇 X-Plane.exe')
    $dialog.Filter = (T 'X-Plane 執行檔 (X-Plane.exe)|X-Plane.exe')
    $dialog.CheckFileExists = $true
    if ($dialog.ShowDialog($form) -eq 'OK') {
        $pathCombo.Text = $dialog.FileName
    }
    $dialog.Dispose()
})
$search.Add_Click({ try { Begin-Search } catch { Stop-Search; Show-Error $_.Exception.Message } })
$install.Add_Click({
    if ($script:Busy) { return }
    $script:Busy = $true; $install.Enabled = $false
    try {
        $status.Text = (T '正在安裝與驗證檔案…'); $status.Refresh()
        Stop-Search
        $mode = 'xplane'
        if ($globalRadio.Checked) { $mode = 'global' }
        Install-XPBypass -SourceApp $PSScriptRoot -Mode $mode -XPlanePath $pathCombo.Text -DesktopShortcut $desktop.Checked
        Update-State
        $status.Text = (T '安裝完成。可從桌面或開始功能表啟動；模式與路徑已儲存。')
        if ($runAfter.Checked) {
            try { Start-InstalledXPlane; $status.Text = (T '安裝完成，已送出 X-Plane 啟動要求。') }
            catch { Show-Error ((T '安裝已完成，但無法啟動：') + $_.Exception.Message) }
        }
    } catch { $status.Text = (T '操作未完成，請查看提示。'); Show-Error $_.Exception.Message }
    finally { $script:Busy = $false; $install.Enabled = $true }
})
$launch.Add_Click({ try { Start-InstalledXPlane } catch { Show-Error $_.Exception.Message } })
$logs.Add_Click({
    try { Show-XPLogWindow -Root $script:InstallRoot -Owner $form }
    catch { Show-Error $_.Exception.Message }
})
$remove.Add_Click({
    if ($script:Busy) { return }
    $answer = [Windows.Forms.MessageBox]::Show($form, (T '將移除此工具、設定與捷徑，X-Plane 本身不受影響。確定解除安裝？'), (T '解除安裝'), 'YesNo', 'Question', 'Button2')
    if ($answer -ne 'Yes') { return }
    $script:Busy = $true
    try {
        Stop-Search
        $status.Text = (T '正在解除安裝…'); $status.Refresh()
        $result = Uninstall-XPBypass
        [void][Windows.Forms.MessageBox]::Show($form, [string]$result, 'XP Optimus Bypass')
        $script:Busy = $false
        Update-State
        $status.Text = T '已解除安裝，可直接在此視窗重新安裝。'
    } catch { Show-Error $_.Exception.Message; Update-State }
    finally { $script:Busy = $false }
})
$close.Add_Click({ $form.Close() })
$form.Add_FormClosing({ param($sender, $eventArgs)
    if ($script:Busy) { $eventArgs.Cancel = $true; return }
    $timer.Stop(); Stop-Search
})
$state = Get-InstallState
if ($state) {
    $pathCombo.Text = [string]$state.xplane
    $globalRadio.Checked = $state.mode -eq 'global'
    $xplaneRadio.Checked = $state.mode -ne 'global'
    $desktop.Checked = [bool]$state.desktopShortcut
} else {
    # Pick up the path saved by the previous portable launcher beside this package.
    foreach ($base in @($PSScriptRoot, (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) , (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))))) {
        $oldCfg = Join-Path $base 'xplane_path.txt'
        if (Test-Path -LiteralPath $oldCfg -PathType Leaf) {
            $valid = Resolve-XPlane (Get-Content -LiteralPath $oldCfg -Encoding UTF8 -TotalCount 1)
            if ($valid) { $pathCombo.Text = $valid; break }
        }
    }
}
Update-State
$languageChoice.Add_SelectedIndexChanged({
    $script:Language = 'en'
    if ($languageChoice.SelectedIndex -eq 1) { $script:Language = 'zh-TW' }
    Update-ControlLanguage $form
    Update-State
    if ($script:SearchJob) { $searchLabel.Text = T '搜尋中…若常見位置未找到，會繼續搜尋本機磁碟。' }
    else { $searchLabel.Text = (T '已找到 ') + $pathCombo.Items.Count + (T ' 個位置。多套安裝可從下拉選單選擇。') }
})
$form.Add_Shown({
    $timer.Start()
    if ($Uninstall) { $remove.PerformClick() }
    else { try { Begin-Search } catch { Stop-Search; Show-Error $_.Exception.Message } }
})
try { [void]$form.ShowDialog() }
finally {
    $timer.Stop(); $timer.Dispose(); Stop-Search
    $form.Dispose()
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
