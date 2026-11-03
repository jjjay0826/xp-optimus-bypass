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
