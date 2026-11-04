$candidates = New-Object 'System.Collections.Generic.List[string]'
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)

function Resolve-XPlane([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return }
    try {
        $p = [Environment]::ExpandEnvironmentVariables($Value.Trim().Trim('"'))
        if (Test-Path -LiteralPath $p -PathType Container) { $p = Join-Path $p 'X-Plane.exe' }
        if ((Split-Path -Leaf $p) -ine 'X-Plane.exe') { return }
        if (Test-Path -LiteralPath $p -PathType Leaf) { return (Get-Item -LiteralPath $p).FullName }
    } catch { return }
}
function Add-Candidate([string]$Value) {
    $p = Resolve-XPlane $Value
    if ($p -and $seen.Add($p)) { $candidates.Add($p) }
}
function Read-VdfPaths([string]$File) {
    if (Test-Path -LiteralPath $File -PathType Leaf) {
        # New libraryfolders.vdf uses "path"; old versions use numeric keys.
        foreach ($line in (Get-Content -LiteralPath $File -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*"(?:path|\d+)"\s+"([^"]+)"') {
                $v = $Matches[1].Replace('\\', '\')
                if ($v -match '^(?:[A-Za-z]:[\\/]|\\\\)') { $v }
            }
        }
    }
}
function Find-Quick {
    # The launcher can also be unpacked in or immediately below X-Plane.
    Add-Candidate $PSScriptRoot
    Add-Candidate (Split-Path -Parent $PSScriptRoot)
    foreach ($base in @($env:LOCALAPPDATA, $env:APPDATA)) {
        if (-not $base) { continue }
        foreach ($name in @('x-plane_install_12.txt', 'x-plane_install_11.txt', 'x-plane_install_10.txt')) {
            $file = Join-Path $base $name
            if (Test-Path -LiteralPath $file -PathType Leaf) {
                foreach ($line in (Get-Content -LiteralPath $file -ErrorAction SilentlyContinue)) { Add-Candidate $line }
            }
        }
    }
    $steamRoots = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        foreach ($prop in @('SteamPath', 'InstallPath')) {
            if ($item -and $item.$prop) { [void]$steamRoots.Add([string]$item.$prop) }
        }
    }
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($base) { [void]$steamRoots.Add((Join-Path $base 'Steam')) }
    }
    $libraries = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($root in $steamRoots) {
        [void]$libraries.Add($root)
        foreach ($relative in @('steamapps\libraryfolders.vdf', 'config\libraryfolders.vdf')) {
            foreach ($path in (Read-VdfPaths (Join-Path $root $relative))) { [void]$libraries.Add($path) }
        }
    }
    foreach ($library in $libraries) {
        $common = Join-Path $library 'steamapps\common'
        foreach ($dir in (Get-ChildItem -LiteralPath $common -Directory -Filter '*X-Plane*' -ErrorAction SilentlyContinue)) { Add-Candidate $dir.FullName }
    }
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -ne [IO.DriveType]::Fixed -or -not $drive.IsReady) { continue }
        foreach ($relative in @('X-Plane 12', 'X-Plane 11', 'Games\X-Plane 12', 'Games\X-Plane 11', 'SteamLibrary\steamapps\common\X-Plane 12', 'SteamLibrary\steamapps\common\X-Plane 11')) {
            Add-Candidate (Join-Path $drive.RootDirectory.FullName $relative)
        }
    }
    foreach ($base in @([Environment]::GetFolderPath('Desktop'), $env:USERPROFILE, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $base) { continue }
        foreach ($dir in (Get-ChildItem -LiteralPath $base -Directory -Filter '*X-Plane*' -ErrorAction SilentlyContinue)) { Add-Candidate $dir.FullName }
    }
}
function Find-OnLocalDrives {
    # Walk directories without following junctions/symlinks (avoids cycles).
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -eq [IO.DriveType]::Fixed -and $drive.IsReady) { $pending.Push($drive.RootDirectory.FullName) }
    }
    $count = 0
    while ($pending.Count -gt 0) {
        $dir = $pending.Pop()
        Add-Candidate $dir
        $count++
        if (($count % 100) -eq 0) { Write-Progress -Activity 'Searching for X-Plane.exe' -Status $dir }
        foreach ($child in (Get-ChildItem -LiteralPath $dir -Directory -Force -ErrorAction SilentlyContinue)) {
            if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) { $pending.Push($child.FullName) }
        }
    }
    Write-Progress -Activity 'Searching for X-Plane.exe' -Completed
}

# Steam relaunches X-Plane.exe when it is not started by Steam, and the relaunched process does not get the
# launcher's environment, so the "X-Plane only" layer would not load. A steam_appid.txt next to X-Plane.exe makes
# the Steam API accept the direct start (Steam must be running). We create it only when missing, remember that we
# did, and remove only files we created on uninstall.
function Get-SteamAppId([string]$Exe) {
    $dir = Split-Path -Parent $Exe
    $folder = Split-Path -Leaf $dir
    $steamapps = Split-Path -Parent (Split-Path -Parent $dir)
    if ((Split-Path -Leaf $steamapps) -ne 'steamapps') { return $null }
    foreach ($acf in @(Get-ChildItem -LiteralPath $steamapps -Filter 'appmanifest_*.acf' -File -ErrorAction SilentlyContinue)) {
        $text = Get-Content -LiteralPath $acf.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -and $text -match ('"installdir"\s+"' + [regex]::Escape($folder) + '"') -and $text -match '"appid"\s+"(\d+)"') { return $Matches[1] }
    }
    if ($folder -eq 'X-Plane 12') { return '2014780' }
    if ($folder -eq 'X-Plane 11') { return '269950' }
    return $null
}
function Enable-SteamDirectLaunch([string]$Exe, [string]$Root) {
    $id = Get-SteamAppId $Exe
    if (-not $id) { return 'not a Steam install' }
    $file = Join-Path (Split-Path -Parent $Exe) 'steam_appid.txt'
    if (Test-Path -LiteralPath $file) { return 'steam_appid.txt already present' }
    [IO.File]::WriteAllText($file, $id)
    Add-Content -LiteralPath (Join-Path $Root 'steam_appid_created.txt') -Value ($id + '|' + $file) -Encoding UTF8
    return ('created ' + $file + ' (' + $id + ')')
}
function Remove-SteamDirectLaunch([string]$Root) {
    $record = Join-Path $Root 'steam_appid_created.txt'
    if (-not (Test-Path -LiteralPath $record)) { return }
    foreach ($line in @(Get-Content -LiteralPath $record -Encoding UTF8)) {
        $parts = ([string]$line).Split('|', 2)
        if ($parts.Count -ne 2 -or [IO.Path]::GetFileName($parts[1]) -ne 'steam_appid.txt') { continue }
        $file = $parts[1]
        if ((Test-Path -LiteralPath $file -PathType Leaf) -and ((Get-Content -LiteralPath $file -Raw).Trim() -eq $parts[0])) {
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $record -Force -ErrorAction SilentlyContinue
}
