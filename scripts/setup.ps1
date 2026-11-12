# Installs, updates or removes Lumenveil, a mod for FINAL FANTASY XV WINDOWS EDITION (Steam) on Windows.
# Normally started by install.bat, uninstall.bat, update.bat or undo-update.bat in the release folder.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -Action install|uninstall|update|undo
#        [-GameDir <folder>] [-SdkPath <zip or folder>] [-FsrPath <zip or folder>] [-XessPath <zip or folder>]
#        [-From <file, zip or folder>] [-AllowOlder] [-NoPrompt]
#        [-SteamRoot <folder>] [-DocumentsDir <folder>] [-DownloadsDir <folder>]   (each replaces its own lookup)
# Written for the Windows PowerShell 5.1 that comes with Windows 10 and 11.
param(
    [Parameter(Mandatory = $true)][ValidateSet('install', 'uninstall', 'update', 'undo')][string]$Action,
    [string]$GameDir,
    [string]$SteamRoot,
    [string]$DocumentsDir,
    [string]$DownloadsDir,
    [string]$SdkPath,
    [string]$FsrPath,
    [string]$XessPath,
    [string]$From,
    [switch]$AllowOlder,
    [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Mark = 'ffxv-dlss'                                # text inside this mod's dxgi.dll
$RecordRel = 'ffxv_dlss/installed_files.txt'
$Keep = '.before_ffxv_dlss'                        # suffix of the backups this script makes
$Part = '.ffxv_dlss_part'                          # suffix of a file while it is being copied
$SdkUrl = 'https://github.com/NVIDIA/DLSS'
$RelDir = 'lib/Windows_x86_64/rel'
$FsrUrl = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip'
$FsrRel = 'Kits/FidelityFX/signedbin'               # where AMD's signed DLLs are in their SDK
$FsrDir = 'ffxv_dlss/fidelityfx'
$FsrNames = @('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll')
$XessUrl = 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
$XessDir = 'ffxv_dlss/xess'
$XessNames = @('libxess.dll', 'libxess_fg.dll', 'libxell.dll')
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)  # every byte survives a decode and encode
$Release = Split-Path -Parent $PSScriptRoot
$Payload = (Join-Path $Release 'game')
$Game = $null

# ---- output and prompts ----------------------------------------------------------------------------
function Say([string]$Text) { Write-Host $Text }
function Write-Note([string]$Text) { Write-Host ('        ' + $Text) }
function Get-StepNumber([string[]]$Steps, [string]$Title) { return ([Array]::IndexOf($Steps, $Title) + 1) }
function Write-Step([string[]]$Steps, [string]$Title) {
    Write-Host ''
    Write-Host ('{0} of {1}  {2}' -f (Get-StepNumber $Steps $Title), $Steps.Count, $Title)
}
function Test-CanPrompt { return ((-not $NoPrompt) -and (-not [Console]::IsInputRedirected)) }
function Read-Answer([string]$Question) { return (Read-Host ('        ' + $Question)) }
function Get-CleanPath([string]$Text) {
    if ($null -eq $Text) { return '' }
    $s = $Text.Trim()
    if ($s.Length -ge 2 -and (($s[0] -eq '"' -and $s[$s.Length - 1] -eq '"') -or ($s[0] -eq "'" -and $s[$s.Length - 1] -eq "'"))) {
        $s = $s.Substring(1, $s.Length - 2)
    }
    return $s
}

# ---- pure helpers (no side effects) -----------------------------------------------------------------
function Join-Parts([string]$Base, [string[]]$Parts) {
    $p = $Base
    foreach ($part in $Parts) { $p = (Join-Path $p $part) }
    return $p
}
function Get-LibraryPaths([string]$Text) {
    $out = @()
    foreach ($m in [regex]::Matches($Text, '(?m)^\s*"path"\s*"(.*)"\s*$')) { $out += $m.Groups[1].Value.Replace('\\', '\') }
    return $out
}
function Get-InstallDir([string]$Text) {
    $m = [regex]::Match($Text, '(?m)^\s*"installdir"\s*"(.*)"\s*$')
    if ($m.Success) { return $m.Groups[1].Value.Replace('\\', '\') }
    return $null
}
function Get-IniNumber([string]$Text, [string]$Key) {
    $m = [regex]::Match($Text, '(?m)^' + [regex]::Escape($Key) + '=([0-9]+)\r?$')
    if ($m.Success) { return $m.Groups[1].Value }
    return $null
}
function Set-IniNumber([string]$Text, [string]$Key, [string]$Value) {
    $re = New-Object System.Text.RegularExpressions.Regex ('(?m)^(' + [regex]::Escape($Key) + '=)[0-9]+(\r?)$')
    return $re.Replace($Text, '${1}' + $Value + '${2}')
}
function Test-AtLeast([string]$Number, [int]$Limit) { return (($Number.Length -gt 4) -or ([int]$Number -ge $Limit)) }
function Find-SdkEntry([string[]]$Names, [string]$File) {
    $want = $RelDir + '/' + $File
    foreach ($n in $Names) {
        $m = $n.Replace('\', '/')
        if (($m -ceq $want) -or $m.EndsWith('/' + $want, [System.StringComparison]::Ordinal)) { return $n }
    }
    return $null
}
function Find-FsrEntry([string[]]$Names, [string]$File) {
    $want = $FsrRel + '/' + $File
    foreach ($n in $Names) {
        $m = $n.Replace('\', '/')
        if (($m -ceq $want) -or $m.EndsWith('/' + $want, [System.StringComparison]::Ordinal)) { return $n }
    }
    return $null
}
function Find-XessEntry([string[]]$Names, [string]$File) {
    $want = 'bin/' + $File
    foreach ($n in $Names) {
        $m = $n.Replace('\', '/')
        if (($m -ceq $want) -or $m.EndsWith('/' + $want, [System.StringComparison]::Ordinal)) { return $n }
    }
    return $null
}
function Get-MinDllSize([string]$Name) {
    # The smallest size in bytes a real copy of that vendor DLL can have.
    switch ($Name) {
        'amd_fidelityfx_loader_dx12.dll' { return 16384 }
        'libxell.dll' { return 131072 }
        default { return 1048576 }
    }
}
function Test-RecordEntry([string]$Entry) {
    # Only this mod's own names: dxgi.dll, or a plain relative path inside ffxv_dlss/.
    if ($Entry -ceq 'dxgi.dll') { return $true }
    if (-not $Entry.StartsWith('ffxv_dlss/', [System.StringComparison]::Ordinal)) { return $false }
    if ($Entry.Contains('\') -or $Entry.Contains(':')) { return $false }
    $parts = $Entry.Split('/')
    if ($parts.Count -lt 2) { return $false }
    foreach ($part in $parts) {
        if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ')) { return $false }
    }
    return $true
}

# ---- finding things ----------------------------------------------------------------------------------
function Get-SteamRoots {
    if ($SteamRoot) { return @($SteamRoot) }
    $roots = @()
    $keys = @(@('HKCU:\Software\Valve\Steam', 'SteamPath'), @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'))
    foreach ($k in $keys) {
        try {
            $v = (Get-ItemProperty -LiteralPath $k[0] -Name $k[1] -ErrorAction Stop).($k[1])
            if ($v) { $roots += $v }
        } catch { }
    }
    $pf = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if ($pf) { $roots += (Join-Path $pf 'Steam') }
    return $roots
}
function Get-DownloadsDir {
    if ($DownloadsDir) { return $DownloadsDir }
    $name = '{374DE290-123F-4565-9164-39C4925E467B}'
    try {
        $v = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' -Name $name -ErrorAction Stop).$name
        if ($v) { return [Environment]::ExpandEnvironmentVariables($v) }
    } catch { }
    $userProfile = $env:USERPROFILE
    if ($userProfile) { return (Join-Path $userProfile 'Downloads') }
    return $null
}
function Get-DocumentsDir {
    if ($DocumentsDir) { return $DocumentsDir }
    return [Environment]::GetFolderPath('MyDocuments')
}
function Get-Libraries {
    $all = @()
    foreach ($root in @(Get-SteamRoots)) {
        $apps = (Join-Path $root 'steamapps')
        if (-not (Test-Path -LiteralPath $apps -PathType Container)) { continue }
        $all += $root
        $vdf = (Join-Path $apps 'libraryfolders.vdf')
        if (Test-Path -LiteralPath $vdf -PathType Leaf) { $all += @(Get-LibraryPaths ([System.IO.File]::ReadAllText($vdf))) }
    }
    $seen = @{}
    $out = @()
    foreach ($lib in $all) {
        if (-not $lib) { continue }
        $apps = (Join-Path $lib 'steamapps')
        if (-not (Test-Path -LiteralPath $apps -PathType Container)) { continue }
        $full = [System.IO.Path]::GetFullPath($lib).TrimEnd('\', '/')
        if ($seen.ContainsKey($full)) { continue }
        $seen[$full] = $true
        $out += $full
    }
    return $out
}
function Find-Game {
    foreach ($lib in @(Get-Libraries)) {
        $apps = (Join-Path $lib 'steamapps')
        $acf = (Join-Path $apps 'appmanifest_637650.acf')
        if (-not (Test-Path -LiteralPath $acf -PathType Leaf)) { continue }
        $dir = Get-InstallDir ([System.IO.File]::ReadAllText($acf))
        if (-not $dir) { continue }
        $common = (Join-Path $apps 'common')
        $folder = (Join-Path $common $dir)
        $exe = (Join-Path $folder 'ffxv_s.exe')
        if (Test-Path -LiteralPath $exe -PathType Leaf) { return $folder }
    }
    return $null
}
function Select-GameFolder {
    $cand = $GameDir
    if (-not $cand) {
        $found = Find-Game
        if ($found) {
            $script:Game = $found
            Write-Note ('Found ' + $found)
            return 0
        }
        Write-Note 'Could not find FINAL FANTASY XV in your Steam libraries.'
    }
    while ($true) {
        if ($cand) {
            $exe = (Join-Path $cand 'ffxv_s.exe')
            if (Test-Path -LiteralPath $exe -PathType Leaf) {
                $script:Game = [System.IO.Path]::GetFullPath($cand).TrimEnd('\', '/')
                Write-Note ('Using ' + $script:Game)
                return 0
            }
            Write-Note ($cand + ' does not hold ffxv_s.exe, so it is not the game folder.')
        }
        if (-not (Test-CanPrompt)) {
            Write-Note 'Run this again and name the game folder (the folder that holds ffxv_s.exe), like this:'
            Write-Note ('powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -Action {0} -GameDir "D:\SteamLibrary\steamapps\common\FINAL FANTASY XV"' -f $Action)
            return 3
        }
        $cand = Get-CleanPath (Read-Answer 'Game folder (the one that holds ffxv_s.exe), or Enter to stop')
        if (-not $cand) { return 3 }
    }
}
function Get-GamePath([string]$Rel) { return (Join-Path $script:Game $Rel) }
function Get-SettingsFiles([string]$Suffix) {
    $docs = Get-DocumentsDir
    if (-not $docs) { return @() }
    $steam = Join-Parts $docs @('My Games', 'FINAL FANTASY XV', 'Steam')
    if (-not (Test-Path -LiteralPath $steam -PathType Container)) { return @() }
    $name = 'GraphicsConfig.ini' + $Suffix
    $out = @()
    foreach ($d in @(Get-ChildItem -LiteralPath $steam -Directory -Force | Sort-Object Name)) {
        $f = Join-Parts $d.FullName @('savestorage', $name)
        if (Test-Path -LiteralPath $f -PathType Leaf) { $out += $f }
    }
    return $out
}
function Get-AccountOf([string]$File) { return (Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $File))) }
function Test-GameRunning { return [bool](Get-Process -Name 'ffxv_s' -ErrorAction SilentlyContinue) }

# ---- files ---------------------------------------------------------------------------------------------
function Test-HasMark([string]$Path) { return $Latin1.GetString([System.IO.File]::ReadAllBytes($Path)).Contains($Mark) }
function Remove-File([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.IsReadOnly) { $item.IsReadOnly = $false }
    [System.IO.File]::Delete($Path)
}
# The bytes go to a temporary name beside the target, which is replaced only once they are all written, so a copy cut
# short (a full disk) never leaves a broken file under a real name.
function Copy-Bytes([string]$From, [string]$To) {
    $tmp = $To + $Part
    try {
        [System.IO.File]::WriteAllBytes($tmp, [System.IO.File]::ReadAllBytes($From))
    } catch {
        if (Test-Path -LiteralPath $tmp -PathType Leaf) { Remove-File $tmp }
        throw
    }
    if (Test-Path -LiteralPath $To -PathType Leaf) { Remove-File $To }
    [System.IO.File]::Move($tmp, $To)
}
function Add-RecordEntries([string[]]$Entries) {
    $rec = Get-GamePath $RecordRel
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $old = @()
    if (Test-Path -LiteralPath $rec -PathType Leaf) { $old = $Latin1.GetString([System.IO.File]::ReadAllBytes($rec)).Split("`n") }
    foreach ($e in ($old + $Entries)) {
        $e = $e.TrimEnd("`r")
        if ($e.Trim() -ne '' -and $seen.Add($e)) { $lines.Add($e) }
    }
    [System.IO.File]::WriteAllBytes($rec, $Latin1.GetBytes(($lines -join "`n") + "`n"))
}
function Test-ValidDll([string]$Path, [string]$Name) {
    if ((New-Object System.IO.FileInfo $Path).Length -le (Get-MinDllSize $Name)) { return $false }
    $buf = New-Object byte[] 2
    $fs = [System.IO.File]::OpenRead($Path)
    try { $n = $fs.Read($buf, 0, 2) } finally { $fs.Dispose() }
    return ($n -eq 2 -and $buf[0] -eq 0x4D -and $buf[1] -eq 0x5A)
}

# ---- zips and NVIDIA's files ------------------------------------------------------------------------------
function Get-ZipNames([string]$Zip) {
    $z = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try { return @($z.Entries | ForEach-Object { $_.FullName }) } finally { $z.Dispose() }
}
function Export-ZipEntry([string]$Zip, [string]$Name, [string]$Out) {
    $z = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $entry = $z.GetEntry($Name)
        if (-not $entry) { throw ('the zip has no ' + $Name) }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $Out, $true)
    } finally { $z.Dispose() }
}
function Get-SdkSource([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        try { $names = @(Get-ZipNames $Path) } catch { return $null }
        $dlss = Find-SdkEntry $names 'nvngx_dlss.dll'
        if (-not $dlss) { return $null }
        $g = $dlss.Substring(0, $dlss.Length - 'nvngx_dlss.dll'.Length) + 'nvngx_dlssg.dll'
        if (-not ($names -ccontains $g)) { $g = $null }
        return @{ Kind = 'zip'; Path = $Path; Dlss = $dlss; Dlssg = $g }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $cands = @($Path) + @(Get-ChildItem -LiteralPath $Path -Directory -Force | Sort-Object Name | ForEach-Object { $_.FullName })
        foreach ($c in $cands) {
            $rel = Join-Parts $c @('lib', 'Windows_x86_64', 'rel')
            $dlss = (Join-Path $rel 'nvngx_dlss.dll')
            if (-not (Test-Path -LiteralPath $dlss -PathType Leaf)) { continue }
            $g = (Join-Path $rel 'nvngx_dlssg.dll')
            if (-not (Test-Path -LiteralPath $g -PathType Leaf)) { $g = $null }
            return @{ Kind = 'folder'; Path = $c; Dlss = $dlss; Dlssg = $g }
        }
    }
    return $null
}
function Find-Sdk {
    if ($SdkPath) {
        $s = Get-SdkSource $SdkPath
        if (-not $s) { Write-Note ('No NVIDIA DLSS files found in ' + $SdkPath) }
        return $s
    }
    $dl = Get-DownloadsDir
    if (-not $dl -or -not (Test-Path -LiteralPath $dl -PathType Container)) { return $null }
    foreach ($z in @(Get-ChildItem -LiteralPath $dl -File -Filter 'DLSS*.zip' -Force | Sort-Object LastWriteTime -Descending)) {
        $s = Get-SdkSource $z.FullName
        if ($s) { return $s }
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $dl -Directory -Filter 'DLSS*' -Force | Sort-Object Name)) {
        $s = Get-SdkSource $d.FullName
        if ($s) { return $s }
    }
    return $null
}
function Get-PartPath([string]$Name) {
    $dir = Get-GamePath 'ffxv_dlss'
    $leaf = '.ffxv_dlss_part.' + $PID + '.' + $Name
    return (Join-Path $dir $leaf)
}
# One vendor file from a download to a temporary name in ffxv_dlss, checked. Returns $true when it is there and valid.
function Get-DllPart($Source, [string]$Entry, [string]$Name) {
    $tmp = Get-PartPath $Name
    try {
        if ($Source.Kind -eq 'zip') { Export-ZipEntry $Source.Path $Entry $tmp } else { Copy-Bytes $Entry $tmp }
    } catch {
        if (Test-Path -LiteralPath $tmp) { Remove-File $tmp }
        Write-Note ('Could not read {0} from {1}' -f $Name, $Source.Path)
        return $false
    }
    if (-not (Test-ValidDll $tmp $Name)) {
        Remove-File $tmp
        Write-Note ('Refused {0} from {1}: it is not a valid DLL.' -f $Name, $Source.Path)
        return $false
    }
    return $true
}
# A fetched file renamed into place and recorded.
function Move-DllPart([string]$Name, [string]$DestRel) {
    $dest = Get-GamePath $DestRel
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $dest))
    if (Test-Path -LiteralPath $dest -PathType Leaf) { Remove-File $dest }
    [System.IO.File]::Move((Get-PartPath $Name), $dest)
    Add-RecordEntries @($DestRel)
}
# True when the file in place and the fetched one both have a version and the one in place is higher; their versions
# are then in $script:InPlaceVer and $script:DownloadVer.
function Test-NewerInPlace([string]$Name, [string]$DestRel) {
    $dest = Get-GamePath $DestRel
    if (-not (Test-Path -LiteralPath $dest -PathType Leaf)) { return $false }
    $script:DownloadVer = Get-VersionOf (Get-PartPath $Name)
    $script:InPlaceVer = Get-VersionOf $dest
    return ($script:DownloadVer -and $script:InPlaceVer -and (Compare-Version $script:InPlaceVer $script:DownloadVer) -eq 1)
}
function Copy-NvidiaFile($Source, [string]$Entry, [string]$DestRel) {
    $name = Split-Path -Leaf (Get-GamePath $DestRel)
    if (-not (Get-DllPart $Source $Entry $name)) { return $false }
    if (Test-NewerInPlace $name $DestRel) {
        Remove-File (Get-PartPath $name)
        Write-Note ('Kept {0} {1}: it is newer than the one in this download ({2}).' -f $name, $script:InPlaceVer, $script:DownloadVer)
        $script:PutKept = $true
        return $true
    }
    Move-DllPart $name $DestRel
    return $true
}
# True (and the fetched files deleted) when any file of the set in place is newer than the download's.
function Test-KeepNewerSet([string]$Dir, [string[]]$Names) {
    foreach ($n in $Names) {
        if (-not (Test-NewerInPlace $n ($Dir + '/' + $n))) { continue }
        Write-Note ('Kept the files in place: {0} there ({1}) is newer than the one in this download ({2}).' -f $n, $script:InPlaceVer, $script:DownloadVer)
        foreach ($m in $Names) { Remove-IfThere (Get-PartPath $m) }
        return $true
    }
    return $false
}
function Write-SdkHelp {
    Write-Note "NVIDIA's DLSS files are not included with this mod. To add them:"
    Write-Note ('1. Open {0}, choose Code, then Download ZIP.' -f $SdkUrl)
    Write-Note '2. Leave the zip in your Downloads folder and run this again.'
    Write-Note 'Or copy them by hand from lib\Windows_x86_64\rel in that zip:'
    Write-Note '   nvngx_dlss.dll goes to ffxv_dlss\nvngx_dlss.dll in the game folder'
    Write-Note '   nvngx_dlssg.dll goes to ffxv_dlss\streamline\nvngx_dlssg.dll (Frame Generation)'
    Write-Note "By downloading them you accept NVIDIA's licence for those files."
}
function Install-NvidiaFiles {
    $dlssPath = Get-GamePath 'ffxv_dlss/nvngx_dlss.dll'
    $dlssgPath = Get-GamePath 'ffxv_dlss/streamline/nvngx_dlssg.dll'
    $inPlace = Test-Path -LiteralPath $dlssPath -PathType Leaf
    $src = Find-Sdk
    if (-not $src) {
        if ($inPlace) {
            Write-Note "NVIDIA's files are already in place."
            if (-not (Test-Path -LiteralPath $dlssgPath -PathType Leaf)) { Write-Note 'nvngx_dlssg.dll is missing: Frame Generation needs it.' }
            return 'already done'
        }
        Write-SdkHelp
        if (-not (Test-CanPrompt)) { return 'skipped' }
        $answer = Get-CleanPath (Read-Answer 'Path to the DLSS zip or folder, or Enter to skip')
        if (-not $answer) { return 'skipped' }
        $src = Get-SdkSource $answer
        if (-not $src) {
            Write-Note ('No NVIDIA DLSS files found in ' + $answer)
            return 'skipped'
        }
    }
    Write-Note ('Using your download ' + $src.Path)
    $script:PutKept = $false
    if (-not (Copy-NvidiaFile $src $src.Dlss 'ffxv_dlss/nvngx_dlss.dll')) { return 'failed' }
    if (-not $src.Dlssg) {
        Write-Note 'Copied nvngx_dlss.dll. This download has no nvngx_dlssg.dll: Frame Generation needs it.'
    } elseif (Copy-NvidiaFile $src $src.Dlssg 'ffxv_dlss/streamline/nvngx_dlssg.dll') {
        if (-not $script:PutKept) { Write-Note 'Copied nvngx_dlss.dll and nvngx_dlssg.dll.' }
    } else {
        Write-Note 'Copied nvngx_dlss.dll only: Frame Generation needs nvngx_dlssg.dll.'
    }
    return 'done'
}

# ---- AMD's files ------------------------------------------------------------------------------------------
# A source holds AMD's loader in Kits/FidelityFX/signedbin; Up and Fg are $null when that folder lacks them.
function Get-FsrSource([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        try { $names = @(Get-ZipNames $Path) } catch { return $null }
        $loader = Find-FsrEntry $names $FsrNames[0]
        if (-not $loader) { return $null }
        $pre = $loader.Substring(0, $loader.Length - $FsrNames[0].Length)
        $up = $pre + $FsrNames[1]
        $fg = $pre + $FsrNames[2]
        if (-not ($names -ccontains $up)) { $up = $null }
        if (-not ($names -ccontains $fg)) { $fg = $null }
        return @{ Kind = 'zip'; Path = $Path; Loader = $loader; Up = $up; Fg = $fg }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $cands = @($Path) + @(Get-ChildItem -LiteralPath $Path -Directory -Force | Sort-Object Name | ForEach-Object { $_.FullName })
        foreach ($c in $cands) {
            $bin = Join-Parts $c @('Kits', 'FidelityFX', 'signedbin')
            $loader = (Join-Path $bin 'amd_fidelityfx_loader_dx12.dll')
            if (-not (Test-Path -LiteralPath $loader -PathType Leaf)) { continue }
            $up = (Join-Path $bin 'amd_fidelityfx_upscaler_dx12.dll')
            $fg = (Join-Path $bin 'amd_fidelityfx_framegeneration_dx12.dll')
            if (-not (Test-Path -LiteralPath $up -PathType Leaf)) { $up = $null }
            if (-not (Test-Path -LiteralPath $fg -PathType Leaf)) { $fg = $null }
            return @{ Kind = 'folder'; Path = $c; Loader = $loader; Up = $up; Fg = $fg }
        }
    }
    return $null
}
function Find-Fsr {
    if ($FsrPath) {
        $s = Get-FsrSource $FsrPath
        if (-not $s) { Write-Note ('No AMD FSR files found in ' + $FsrPath) }
        return $s
    }
    $dl = Get-DownloadsDir
    if (-not $dl -or -not (Test-Path -LiteralPath $dl -PathType Container)) { return $null }
    foreach ($z in @(Get-ChildItem -LiteralPath $dl -File -Filter 'FidelityFX-SDK*.zip' -Force | Sort-Object LastWriteTime -Descending)) {
        $s = Get-FsrSource $z.FullName
        if ($s) { return $s }
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $dl -Directory -Filter 'FidelityFX-SDK*' -Force | Sort-Object Name)) {
        $s = Get-FsrSource $d.FullName
        if ($s) { return $s }
    }
    return $null
}
function Write-FsrHelp {
    Write-Note "AMD FSR is optional, and DLSS works without AMD's files. They are not included with this mod. To add them:"
    Write-Note ("1. Download AMD's FidelityFX SDK 2.4.0 from {0}" -f $FsrUrl)
    Write-Note '   Your browser saves it as FidelityFX-SDK-2.4.0.zip (about 200 MB).'
    Write-Note '2. Leave the zip in your Downloads folder and run this again.'
    Write-Note 'Or copy these three files by hand from Kits\FidelityFX\signedbin in that zip to ffxv_dlss\fidelityfx in the game folder:'
    Write-Note '   amd_fidelityfx_loader_dx12.dll'
    Write-Note '   amd_fidelityfx_upscaler_dx12.dll'
    Write-Note '   amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation)'
    Write-Note "By downloading them you accept AMD's licence for those files."
}
function Install-FsrFiles {
    $src = Find-Fsr
    if (-not $src) {
        $loaderPath = Get-GamePath ($FsrDir + '/' + $FsrNames[0])
        $upPath = Get-GamePath ($FsrDir + '/' + $FsrNames[1])
        $fgPath = Get-GamePath ($FsrDir + '/' + $FsrNames[2])
        if ((Test-Path -LiteralPath $loaderPath -PathType Leaf) -and (Test-Path -LiteralPath $upPath -PathType Leaf)) {
            Write-Note "AMD's FSR files are already in place."
            if (-not (Test-Path -LiteralPath $fgPath -PathType Leaf)) {
                Write-Note 'amd_fidelityfx_framegeneration_dx12.dll is missing, and FSR frame generation needs it.'
            }
            return 'already done'
        }
        Write-FsrHelp
        return 'skipped'
    }
    Write-Note ('Using your download ' + $src.Path)
    if (-not $src.Up) {
        Write-Note 'This download has amd_fidelityfx_loader_dx12.dll but no amd_fidelityfx_upscaler_dx12.dll, so nothing was copied.'
        return 'failed'
    }
    # All of them are checked before any is put in place, so a bad download changes nothing.
    $want = @(@($FsrNames[0], $src.Loader), @($FsrNames[1], $src.Up))
    if ($src.Fg) { $want += , @($FsrNames[2], $src.Fg) }
    $good = $true
    foreach ($w in $want) {
        if (-not (Get-DllPart $src $w[1] $w[0])) { $good = $false; break }
    }
    if (-not $good) {
        foreach ($w in $want) {
            $tmp = Get-PartPath $w[0]
            if (Test-Path -LiteralPath $tmp) { Remove-File $tmp }
        }
        Write-Note 'Nothing was copied from this download.'
        return 'failed'
    }
    if (Test-KeepNewerSet $FsrDir @($want | ForEach-Object { $_[0] })) { return 'kept newer' }
    foreach ($w in $want) { Move-DllPart $w[0] ($FsrDir + '/' + $w[0]) }
    if ($src.Fg) {
        Write-Note "Copied AMD's three FSR files."
    } else {
        Write-Note 'Copied amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll. This download has no'
        Write-Note 'amd_fidelityfx_framegeneration_dx12.dll, and FSR frame generation needs it.'
    }
    return 'done'
}

# ---- Intel's files ----------------------------------------------------------------------------------------
# A source holds Intel's libxess.dll in a bin folder; Fg and Xell are $null when that folder lacks them.
function Get-XessSource([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        try { $names = @(Get-ZipNames $Path) } catch { return $null }
        $sr = Find-XessEntry $names $XessNames[0]
        if (-not $sr) { return $null }
        $pre = $sr.Substring(0, $sr.Length - $XessNames[0].Length)
        $fg = $pre + $XessNames[1]
        $xell = $pre + $XessNames[2]
        if (-not ($names -ccontains $fg)) { $fg = $null }
        if (-not ($names -ccontains $xell)) { $xell = $null }
        return @{ Kind = 'zip'; Path = $Path; Sr = $sr; Fg = $fg; Xell = $xell }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $cands = @($Path) + @(Get-ChildItem -LiteralPath $Path -Directory -Force | Sort-Object Name | ForEach-Object { $_.FullName })
        foreach ($c in $cands) {
            $bin = (Join-Path $c 'bin')
            $sr = (Join-Path $bin 'libxess.dll')
            if (-not (Test-Path -LiteralPath $sr -PathType Leaf)) { continue }
            $fg = (Join-Path $bin 'libxess_fg.dll')
            $xell = (Join-Path $bin 'libxell.dll')
            if (-not (Test-Path -LiteralPath $fg -PathType Leaf)) { $fg = $null }
            if (-not (Test-Path -LiteralPath $xell -PathType Leaf)) { $xell = $null }
            return @{ Kind = 'folder'; Path = $c; Sr = $sr; Fg = $fg; Xell = $xell }
        }
    }
    return $null
}
function Find-Xess {
    if ($XessPath) {
        $s = Get-XessSource $XessPath
        if (-not $s) { Write-Note ('No Intel XeSS files found in ' + $XessPath) }
        return $s
    }
    $dl = Get-DownloadsDir
    if (-not $dl -or -not (Test-Path -LiteralPath $dl -PathType Container)) { return $null }
    foreach ($z in @(Get-ChildItem -LiteralPath $dl -File -Filter 'XeSS_SDK*.zip' -Force | Sort-Object LastWriteTime -Descending)) {
        $s = Get-XessSource $z.FullName
        if ($s) { return $s }
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $dl -Directory -Filter 'XeSS_SDK*' -Force | Sort-Object Name)) {
        $s = Get-XessSource $d.FullName
        if ($s) { return $s }
    }
    return $null
}
function Write-XessHelp {
    Write-Note "Intel XeSS is optional, and DLSS works without Intel's files. They are not included with this mod. To add them:"
    Write-Note ("1. Download Intel's XeSS SDK 3.0.2 from {0}" -f $XessUrl)
    Write-Note '   Your browser saves it as XeSS_SDK_3.0.2.zip (about 77 MB).'
    Write-Note '2. Leave the zip in your Downloads folder and run this again.'
    Write-Note 'Or copy these three files by hand from the bin folder in that zip to ffxv_dlss\xess in the game folder:'
    Write-Note '   libxess.dll'
    Write-Note '   libxess_fg.dll (XeSS frame generation)'
    Write-Note '   libxell.dll (XeSS frame generation)'
    Write-Note "By downloading them you accept Intel's licence for those files."
}
function Install-XessFiles {
    $src = Find-Xess
    if (-not $src) {
        $srPath = Get-GamePath ($XessDir + '/' + $XessNames[0])
        $fgPath = Get-GamePath ($XessDir + '/' + $XessNames[1])
        $xellPath = Get-GamePath ($XessDir + '/' + $XessNames[2])
        if (Test-Path -LiteralPath $srPath -PathType Leaf) {
            Write-Note "Intel's XeSS files are already in place."
            if (-not ((Test-Path -LiteralPath $fgPath -PathType Leaf) -and (Test-Path -LiteralPath $xellPath -PathType Leaf))) {
                Write-Note 'libxess_fg.dll or libxell.dll is missing, so XeSS frame generation is unavailable.'
            }
            return 'already done'
        }
        Write-XessHelp
        return 'skipped'
    }
    Write-Note ('Using your download ' + $src.Path)
    # Frame generation needs both of its files, so they are copied as a pair or not at all. All of them are checked
    # before any is put in place, so a bad download changes nothing.
    $want = @(, @($XessNames[0], $src.Sr))
    if ($src.Fg -and $src.Xell) { $want += @(@($XessNames[1], $src.Fg), @($XessNames[2], $src.Xell)) }
    $good = $true
    foreach ($w in $want) {
        if (-not (Get-DllPart $src $w[1] $w[0])) { $good = $false; break }
    }
    if (-not $good) {
        foreach ($w in $want) {
            $tmp = Get-PartPath $w[0]
            if (Test-Path -LiteralPath $tmp) { Remove-File $tmp }
        }
        Write-Note 'Nothing was copied from this download.'
        return 'failed'
    }
    if (Test-KeepNewerSet $XessDir @($want | ForEach-Object { $_[0] })) { return 'kept newer' }
    foreach ($w in $want) { Move-DllPart $w[0] ($XessDir + '/' + $w[0]) }
    if ($want.Count -eq 3) {
        Write-Note "Copied Intel's three XeSS files."
    } else {
        Write-Note 'Copied libxess.dll. This download lacks libxess_fg.dll or libxell.dll, so XeSS frame generation is unavailable.'
    }
    return 'done'
}

# ---- what a vendor DLL is ---------------------------------------------------------------------------------------
# The header, version and signature block are read by offset, a few bytes at a time. Every offset is checked against
# the file's size and every count is capped, so a damaged file is refused rather than read past its end.
function Read-PeBytes($Stream, [long]$Offset, [long]$Count) {
    if ($Offset -lt 0 -or $Count -lt 0 -or ($Offset + $Count) -gt $Stream.Length) { return $null }
    $buf = New-Object byte[] $Count
    [void]$Stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
    $n = 0
    while ($n -lt $Count) {
        $r = $Stream.Read($buf, $n, $Count - $n)
        if ($r -le 0) { return $null }
        $n += $r
    }
    return , $buf
}
function Read-PeNumber($Stream, [long]$Offset, [int]$Size) {
    $b = Read-PeBytes $Stream $Offset $Size
    if ($null -eq $b) { return $null }
    if ($Size -eq 2) { return [long]([int]$b[0] + 256 * [int]$b[1]) }
    return [long]([long]$b[0] + 256L * $b[1] + 65536L * $b[2] + 16777216L * $b[3])
}
# The file offset of an address in the image, from the section table; $null when no section holds it.
function ConvertTo-PeOffset([long[]]$Sections, [long]$Rva) {
    for ($i = 0; $i + 3 -lt $Sections.Count; $i += 4) {
        $va = $Sections[$i]; $size = $Sections[$i + 1]
        if ($Sections[$i + 2] -gt $size) { $size = $Sections[$i + 2] }
        if ($Rva -ge $va -and $Rva -lt ($va + $size)) { return ($Sections[$i + 3] + $Rva - $va) }
    }
    return $null
}
# From the resource directory at file offset $Dir, the offset field of the entry with id $Which, or of the first entry
# when $Which is -1; $null when there is none.
function Find-PeDirEntry($Stream, [long]$Dir, [long]$Which) {
    $named = Read-PeNumber $Stream ($Dir + 12) 2
    $ids = Read-PeNumber $Stream ($Dir + 14) 2
    if ($null -eq $named -or $null -eq $ids) { return $null }
    $n = $named + $ids
    if ($n -lt 1 -or $n -gt 64) { return $null }
    for ($i = 0; $i -lt $n; $i++) {
        $id = Read-PeNumber $Stream ($Dir + 16 + 8 * $i) 4
        $off = Read-PeNumber $Stream ($Dir + 20 + 8 * $i) 4
        if ($null -eq $id -or $null -eq $off) { return $null }
        if ($Which -eq -1 -or $id -eq $Which) { return $off }
    }
    return $null
}
function Get-PeString([string[]]$Lines, [string]$Key) {
    $found = $false
    foreach ($l in $Lines) {
        if ($found -and $l.Length -gt 0) { return $l }
        if ($l -ceq $Key) { $found = $true }
    }
    return ''
}
# Reads FILE's header into $Info: Bad (notdll, short, nover, or empty when all was read), Ver, Company, Product, Desc,
# Orig, CertOff, CertLen, Size.
function Read-PeInfo($fs, $Info) {
    $Info.Size = $fs.Length
    if ((Read-PeNumber $fs 0 2) -ne 23117) { return }                  # MZ
    $h = Read-PeNumber $fs 60 4
    if ($null -eq $h) { return }
    if ((Read-PeNumber $fs $h 4) -ne 17744) { return }                 # PE\0\0
    if ((Read-PeNumber $fs ($h + 4) 2) -ne 34404) { return }           # 64-bit x86 (0x8664)
    $nsec = Read-PeNumber $fs ($h + 6) 2
    $optsz = Read-PeNumber $fs ($h + 20) 2
    $chars = Read-PeNumber $fs ($h + 22) 2
    if ($null -eq $chars -or ($chars -band 0x2000) -eq 0) { return }   # a DLL, not a program
    $opt = $h + 24
    if ((Read-PeNumber $fs $opt 2) -ne 523) { return }                 # PE32+ (0x20b)
    if ($nsec -lt 1 -or $nsec -gt 96 -or $optsz -lt 152) { return }
    $Info.Bad = 'short'
    $ndd = Read-PeNumber $fs ($opt + 108) 4
    if ($null -eq $ndd) { return }
    if ($ndd -lt 5) { $Info.Bad = 'nover'; return }
    $sec = New-Object 'System.Collections.Generic.List[long]'
    for ($i = 0; $i -lt $nsec; $i++) {
        $s = $opt + $optsz + 40 * $i
        foreach ($k in @(12, 8, 16, 20)) {                              # address, virtual size, raw size, raw offset
            $v = Read-PeNumber $fs ($s + $k) 4
            if ($null -eq $v) { return }
            $sec.Add($v)
        }
    }
    $sections = $sec.ToArray()
    $certOff = Read-PeNumber $fs ($opt + 144) 4
    $certLen = Read-PeNumber $fs ($opt + 148) 4
    $rva = Read-PeNumber $fs ($opt + 128) 4
    $rsz = Read-PeNumber $fs ($opt + 132) 4
    if ($null -eq $certOff -or $null -eq $certLen -or $null -eq $rva -or $null -eq $rsz) { return }
    $Info.CertOff = $certOff
    $Info.CertLen = $certLen
    $Info.Bad = 'nover'
    if ($rva -eq 0 -or $rsz -eq 0) { return }
    $base = ConvertTo-PeOffset $sections $rva
    if ($null -eq $base) { return }
    $off = Find-PeDirEntry $fs $base 16
    if ($null -eq $off) {
        if ($base -ge $fs.Length) { $Info.Bad = 'short' }
        return
    }
    $Info.Bad = 'short'
    if (($off -band 2147483648) -eq 0) { return }
    $off = Find-PeDirEntry $fs ($base + ($off -band 0x7FFFFFFF)) -1
    if ($null -eq $off -or ($off -band 2147483648) -eq 0) { return }
    $off = Find-PeDirEntry $fs ($base + ($off -band 0x7FFFFFFF)) -1
    if ($null -eq $off -or ($off -band 2147483648) -ne 0) { return }
    $rva = Read-PeNumber $fs ($base + $off) 4
    $rsz = Read-PeNumber $fs ($base + $off + 4) 4
    if ($null -eq $rva -or $null -eq $rsz) { return }
    if ($rsz -lt 92 -or $rsz -gt 65536) { $Info.Bad = 'nover'; return }
    $off = ConvertTo-PeOffset $sections $rva
    if ($null -eq $off) { return }
    $res = Read-PeBytes $fs $off $rsz
    if ($null -eq $res) { return }
    # 0xFEEF04BD and 0x80000000 are written in decimal: PowerShell reads such hex literals as negative 32-bit numbers.
    if ([BitConverter]::ToUInt32($res, 40) -ne 4277077181) { $Info.Bad = 'nover'; return }
    $ms = [BitConverter]::ToUInt32($res, 48)
    $ls = [BitConverter]::ToUInt32($res, 52)
    $Info.Ver = '{0}.{1}.{2}.{3}' -f ($ms -shr 16), ($ms -band 0xFFFF), ($ls -shr 16), ($ls -band 0xFFFF)
    # The version strings are UTF-16: each key is followed, after zero padding, by its value.
    $sb = New-Object System.Text.StringBuilder
    for ($i = 0; $i + 1 -lt $res.Length; $i += 2) {
        $u = [int]$res[$i] + 256 * [int]$res[$i + 1]
        if ($u -ge 32 -and $u -lt 127) { [void]$sb.Append([char]$u) } else { [void]$sb.Append("`n") }
    }
    $lines = $sb.ToString().Split("`n")
    $Info.Company = Get-PeString $lines 'CompanyName'
    $Info.Product = Get-PeString $lines 'ProductName'
    $Info.Desc = Get-PeString $lines 'FileDescription'
    $Info.Orig = Get-PeString $lines 'OriginalFilename'
    $Info.Bad = ''
}
function Get-PeInfo([string]$Path) {
    $info = @{ Bad = 'notdll'; Ver = ''; Company = ''; Product = ''; Desc = ''; Orig = ''; CertOff = 0; CertLen = 0; Size = 0 }
    $fs = $null
    try { $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite') } catch { return $info }
    try { Read-PeInfo $fs $info } finally { $fs.Dispose() }
    return $info
}
function Get-VersionOf([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-PeInfo $Path).Ver
}
function Get-Vendor([string]$Name) {
    if ($Name.StartsWith('nvngx_', [System.StringComparison]::Ordinal)) { return 'NVIDIA' }
    if ($Name.StartsWith('amd_', [System.StringComparison]::Ordinal)) { return 'AMD' }
    return 'Intel'
}
function Get-VendorOrg([string]$Vendor) {
    switch ($Vendor) {
        'NVIDIA' { return 'NVIDIA Corporation' }
        'AMD' { return 'Advanced Micro Devices' }
        default { return 'Intel Corporation' }
    }
}
# What Windows says about a signature, turned into a reason to refuse (Refuse), or a note when it could not say (Note).
function Get-SignatureVerdict([string]$Status, [string]$Signer, [string]$Vendor) {
    $org = Get-VendorOrg $Vendor
    switch ($Status) {
        'Valid' {
            if ($Signer -and $Signer.Contains($org)) { return @{ Refuse = ''; Note = '' } }
            return @{ Refuse = ('its digital signature does not name {0}' -f $Vendor); Note = '' }
        }
        'NotSigned' { return @{ Refuse = 'it has no digital signature'; Note = '' } }
        'HashMismatch' { return @{ Refuse = 'its digital signature does not match the file'; Note = '' } }
        'NotTrusted' { return @{ Refuse = 'Windows does not trust its digital signature'; Note = '' } }
        default { return @{ Refuse = ''; Note = 'Windows could not check its digital signature, so only its labels were checked' } }
    }
}
function Test-OnWindows { return ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) }
# Checks that FILE is the vendor's own NAME. Returns the header info with Why set: empty when it passes.
function Test-VendorDll([string]$Path, [string]$Name) {
    $who = Get-Vendor $Name
    switch ($who) {
        'NVIDIA' { $company = 'NVIDIA' }
        'AMD' { $company = 'Advanced Micro Devices' }
        default { $company = 'Intel Corporation' }
    }
    $org = Get-VendorOrg $who
    $i = Get-PeInfo $Path
    $why = ''
    if ($i.Bad -eq 'notdll') { $why = 'it is not a 64-bit Windows DLL' }
    elseif ($i.Bad -eq 'short') { $why = 'it is cut short' }
    elseif (-not $i.Ver) { $why = 'it has no version information' }
    elseif (-not $i.Company.StartsWith($company, [System.StringComparison]::Ordinal)) { $why = "it is not $who's file" }
    if (-not $why) {
        switch ($Name) {
            'nvngx_dlssg.dll' { $ok = $i.Product.Contains('DLSS-G') }
            'nvngx_dlss.dll' { $ok = -not ($i.Product.Contains('DLSS-G') -or $i.Product.Contains('Ray Reconstruction')) }
            'amd_fidelityfx_loader_dx12.dll' { $ok = $i.Desc.Contains('Loader') }
            'amd_fidelityfx_upscaler_dx12.dll' { $ok = $i.Desc.Contains('Upscaler') }
            'amd_fidelityfx_framegeneration_dx12.dll' { $ok = $i.Desc.Contains('Frame Generation') }
            default { $ok = ($i.Orig -ceq $Name) }
        }
        if (-not $ok) { $why = "it is a different $who file under this name" }
    }
    if (-not $why -and $i.Size -le (Get-MinDllSize $Name)) { $why = 'it is cut short' }
    if (-not $why) {
        if ($i.CertLen -lt 8) {
            $why = 'it has no digital signature'
        } elseif ($i.CertLen -gt $MaxCert) {
            $why = 'its digital signature is too large to check'
        } elseif (($i.CertOff + $i.CertLen) -gt $i.Size) {
            $why = 'it is cut short'
        } else {
            $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
            try { $blob = Read-PeBytes $fs $i.CertOff $i.CertLen } finally { $fs.Dispose() }
            if ($null -eq $blob -or $blob[6] -ne 2 -or $blob[7] -ne 0) {
                $why = 'it has no digital signature'
            } elseif (-not $Latin1.GetString($blob).Contains($org)) {
                $why = "its digital signature does not name $who"
            }
        }
    }
    if (-not $why -and (Test-OnWindows)) {
        $status = 'UnknownError'
        $signer = ''
        if (Get-Command -Name 'Get-AuthenticodeSignature' -ErrorAction SilentlyContinue) {
            try {
                $sig = Get-AuthenticodeSignature -LiteralPath $Path
                $status = [string]$sig.Status
                if ($sig.SignerCertificate) { $signer = $sig.SignerCertificate.Subject }
            } catch { $status = 'UnknownError' }
        }
        $verdict = Get-SignatureVerdict $status $signer $who
        if ($verdict.Refuse) { $why = $verdict.Refuse }
        if ($verdict.Note) { Write-Note ('{0}: {1}.' -f $Name, $verdict.Note) }
    }
    $i.Why = $why
    return $i
}
# -1, 0 or 1 as version A is lower than, equal to or higher than B, number by number.
function Compare-Version([string]$A, [string]$B) {
    # PowerShell names ignore case: $A and $a are one [string] variable, so the parts get names of their own.
    $partsA = $A.Split('.')
    $partsB = $B.Split('.')
    for ($k = 0; $k -lt 4; $k++) {
        $x = 0; $y = 0
        if ($k -lt $partsA.Count -and $partsA[$k] -ne '') { $x = [long]$partsA[$k] }
        if ($k -lt $partsB.Count -and $partsB[$k] -ne '') { $y = [long]$partsB[$k] }
        if ($x -lt $y) { return -1 }
        if ($x -gt $y) { return 1 }
    }
    return 0
}

# ---- update -------------------------------------------------------------------------------------------------------
# The eight vendor files update knows, in the order they are listed. AMD's and Intel's three are a set each.
$Targets = @('nvngx_dlss.dll', 'nvngx_dlssg.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll',
    'amd_fidelityfx_loader_dx12.dll', 'libxess.dll', 'libxess_fg.dll', 'libxell.dll')
$AmdSet = @('amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_loader_dx12.dll')
$IntelSet = @('libxess.dll', 'libxess_fg.dll', 'libxell.dll')
$ListRel = 'ffxv_dlss/last_update.txt'
$Kept = '.before_update'                           # suffix of the copy update keeps of each file it replaces
$MaxCert = 1048576   # largest signature block read; the vendors' measure 10 to 12 KB
function Get-TargetDest([string]$Name) {
    switch ($Name) {
        'nvngx_dlss.dll' { return 'ffxv_dlss/nvngx_dlss.dll' }
        'nvngx_dlssg.dll' { return 'ffxv_dlss/streamline/nvngx_dlssg.dll' }
    }
    if ($Name.StartsWith('amd_', [System.StringComparison]::Ordinal)) { return ($FsrDir + '/' + $Name) }
    return ($XessDir + '/' + $Name)
}
function Test-TargetPath([string]$Rel) {
    foreach ($t in $Targets) { if ((Get-TargetDest $t) -ceq $Rel) { return $true } }
    return $false
}
# A target's name, or that name saved again as "name (N).dll": @{ Name; Copy } (Copy is the " (N)" part), else $null.
function Get-TargetMatch([string]$Leaf) {
    foreach ($t in $Targets) {
        if ($Leaf -ceq $t) { return @{ Name = $t; Copy = '' } }
        $m = [regex]::Match($Leaf, '^(.+) \(([0-9]{1,3})\)\.dll$')
        if ($m.Success -and ($m.Groups[1].Value + '.dll') -ceq $t) { return @{ Name = $t; Copy = ' (' + $m.Groups[2].Value + ')' } }
    }
    return $null
}
# True when a folder in the path holds a development, debug or ARM build.
function Test-DevPath([string]$Rel) {
    $parts = $Rel.Replace('\', '/').Split('/')
    for ($k = 0; $k -lt $parts.Count - 1; $k++) {
        $d = $parts[$k].ToLowerInvariant()
        if ($d -eq 'dev' -or $d -eq 'development' -or $d -eq 'debug' -or $d.Contains('arm64') -or $d.Contains('aarch64')) { return $true }
    }
    return $false
}
function Get-FolderOf([string]$Rel) {
    $r = $Rel.Replace('\', '/')
    $k = $r.LastIndexOf('/')
    if ($k -lt 0) { return '.' }
    return $r.Substring(0, $k)
}
$script:Cands = New-Object 'System.Collections.Generic.List[object]'
$script:DevSkipped = @{}
function Add-Candidate([string]$Kind, [string]$Zip, [string]$Path, [string]$Rel, [string]$From) {
    $leaf = $Rel.Replace('\', '/')
    $leaf = $leaf.Substring($leaf.LastIndexOf('/') + 1)
    $m = Get-TargetMatch $leaf
    if (-not $m) { return }
    if (Test-DevPath $Rel) { $script:DevSkipped[$m.Name] = $From; return }
    $script:Cands.Add(@{ Kind = $Kind; Zip = $Zip; Path = $Path; Name = $m.Name; From = $From
            Group = ($Zip + '|' + (Get-FolderOf $Rel) + '|' + $m.Copy) })
}
function Add-ZipCandidates([string]$Zip) {
    try { $names = @(Get-ZipNames $Zip) } catch {
        Write-Note ('Skipped {0}: it could not be opened as a zip.' -f $Zip)
        return
    }
    foreach ($e in $names) {
        if ($e -eq '' -or $e.EndsWith('/') -or $e.EndsWith('\')) { continue }
        Add-Candidate 'zip' $Zip $e $e $Zip
    }
}
function Add-FileCandidates([string]$Path, [string]$Rel) {
    $lower = $Path.ToLowerInvariant()
    if ($lower.EndsWith('.zip')) { Add-ZipCandidates $Path }
    elseif ($lower.EndsWith('.dll')) { Add-Candidate 'file' '' $Path $Rel $Path }
}
# A folder's own name and a slash, after its parent's name when that names an ARM build, so a --from folder that is
# itself a development or ARM folder is seen.
function Get-FromTail([string]$Dir) {
    $parts = [System.IO.Path]::GetFullPath($Dir).Replace('\', '/').TrimEnd('/').Split('/')
    $tail = $parts[-1] + '/'
    if ($parts.Count -ge 2) {
        $a = $parts[-2].ToLowerInvariant()
        if ($a.Contains('arm64') -or $a.Contains('aarch64')) { $tail = $parts[-2] + '/' + $tail }
    }
    return $tail
}
function Find-Candidates {
    if ($From) {
        if (Test-Path -LiteralPath $From -PathType Leaf) {
            Write-Note ('Looking in ' + $From)
            $pre = Get-FromTail ([System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($From)))
            Add-FileCandidates $From ($pre + (Split-Path -Leaf $From))
        } elseif (Test-Path -LiteralPath $From -PathType Container) {
            Write-Note ('Looking in ' + $From)
            $root = [System.IO.Path]::GetFullPath($From).TrimEnd('\', '/')
            $pre = Get-FromTail $root
            $files = @(Get-ChildItem -LiteralPath $root -Recurse -Depth 5 -File -Force |
                    ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') })
            [Array]::Sort($files, [System.StringComparer]::Ordinal)
            foreach ($f in $files) { Add-FileCandidates (Join-Path $root $f) ($pre + $f) }
        } else {
            Write-Note ($From + ' was not found.')
            return $false
        }
        return $true
    }
    $dl = Get-DownloadsDir
    if (-not $dl -or -not (Test-Path -LiteralPath $dl -PathType Container)) { return $true }
    Write-Note ('Looking in ' + $dl)
    $files = @(Get-ChildItem -LiteralPath $dl -File -Force | ForEach-Object { $_.Name })
    [Array]::Sort($files, [System.StringComparer]::Ordinal)
    foreach ($f in $files) { Add-FileCandidates (Join-Path $dl $f) $f }
    return $true
}
function Remove-IfThere([string]$Path) {
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) { Remove-File $Path }
}
function Get-Candidate($C, [string]$Out) {
    try {
        if ($C.Kind -eq 'zip') { Export-ZipEntry $C.Zip $C.Path $Out } else { [System.IO.File]::Copy($C.Path, $Out, $true) }
        return $true
    } catch {
        Remove-IfThere $Out
        Write-Note ('Could not read {0} from {1}' -f $C.Name, $C.From)
        return $false
    }
}
# The chosen download of each target waits in its part file; its version is in BestVer[name].
$script:BestVer = @{}
$script:StageFailed = $false   # true once a download could not be copied into the game folder: the run stops before any change
function Select-One([string]$Name) {
    if ($script:StageFailed) { return }
    $tmp = Get-PartPath ('cand.' + $Name)
    $found = $false
    foreach ($c in $script:Cands) {
        if ($c.Name -cne $Name) { continue }
        $found = $true
        if (-not (Get-Candidate $c $tmp)) { $script:StageFailed = $true; return }
        $i = Test-VendorDll $tmp $Name
        if ($i.Why) {
            Write-Note ('Refused {0} from {1}: {2}.' -f $Name, $c.From, $i.Why)
            Remove-IfThere $tmp
            continue
        }
        if (-not $script:BestVer.ContainsKey($Name) -or (Compare-Version $i.Ver $script:BestVer[$Name]) -eq 1) {
            $best = Get-PartPath $Name
            try {
                Remove-IfThere $best
                [System.IO.File]::Move($tmp, $best)
            } catch {
                Remove-IfThere $tmp
                $script:StageFailed = $true
                return
            }
            $script:BestVer[$Name] = $i.Ver
        } else {
            Remove-IfThere $tmp
        }
    }
    if (-not $found -and $script:DevSkipped.ContainsKey($Name)) {
        Write-Note ('Refused {0} from {1}: it comes from a development or ARM folder.' -f $Name, $script:DevSkipped[$Name])
    }
}
function Compare-VersionSet([string[]]$A, [string[]]$B) {
    for ($k = 0; $k -lt $A.Count; $k++) {
        $c = Compare-Version $A[$k] $B[$k]
        if ($c -ne 0) { return $c }
    }
    return 0
}
# The highest set of three files from one folder of one download.
function Select-Set([string]$Label, [string[]]$Names) {
    if ($script:StageFailed) { return }
    $at = @{}
    $groups = New-Object 'System.Collections.Generic.List[string]'
    for ($k = 0; $k -lt $script:Cands.Count; $k++) {
        $c = $script:Cands[$k]
        if (-not ($Names -ccontains $c.Name)) { continue }
        $key = $c.Group + '/' + $c.Name
        if ($at.ContainsKey($key)) { continue }
        $at[$key] = $k
        if (-not $groups.Contains($c.Group)) { $groups.Add($c.Group) }
    }
    $best = $null
    $partial = @()
    foreach ($g in $groups) {
        $idx = @()
        foreach ($n in $Names) { if ($at.ContainsKey($g + '/' + $n)) { $idx += $at[$g + '/' + $n] } }
        $first = $script:Cands[$idx[0]]
        if ($idx.Count -lt 3) {
            $partial += ('{0}: {1} has only {2} of the 3 files, so nothing was copied from it.' -f $Label, $first.From, $idx.Count)
            continue
        }
        $good = $true
        $vers = @()
        foreach ($k in $idx) {
            $c = $script:Cands[$k]
            $tmp = Get-PartPath ('cand.' + $c.Name)
            if (-not (Get-Candidate $c $tmp)) { $script:StageFailed = $true; return }
            $i = Test-VendorDll $tmp $c.Name
            if ($i.Why) {
                Write-Note ('Refused {0} from {1}: {2}.' -f $c.Name, $c.From, $i.Why)
                Write-Note ('{0}: nothing was copied from {1}.' -f $Label, $c.From)
                $good = $false
                break
            }
            $vers += $i.Ver
        }
        if ($good -and ($null -eq $best -or (Compare-VersionSet $vers $best) -eq 1)) {
            $best = $vers
            for ($k = 0; $k -lt $Names.Count; $k++) {
                $to = Get-PartPath $Names[$k]
                Remove-IfThere $to
                [System.IO.File]::Move((Get-PartPath ('cand.' + $Names[$k])), $to)
                $script:BestVer[$Names[$k]] = $vers[$k]
            }
        }
        foreach ($n in $Names) { Remove-IfThere (Get-PartPath ('cand.' + $n)) }
    }
    if ($null -eq $best) { foreach ($p in $partial) { Write-Note $p } }
    foreach ($n in $Names) {
        if (-not $script:BestVer.ContainsKey($n) -and $script:DevSkipped.ContainsKey($n)) {
            Write-Note ('Refused {0} from {1}: it comes from a development or ARM folder.' -f $n, $script:DevSkipped[$n])
        }
    }
}
# Change list, filled by Set-ItemPlan: Name, Mode (replaced, added, or failed), Old (the version it had).
$script:Changes = New-Object 'System.Collections.Generic.List[object]'
function Write-UpToDate([string[]]$Names) {
    foreach ($n in $Names) {
        $p = Get-GamePath (Get-TargetDest $n)
        $v = Get-VersionOf $p
        if ($v) { Write-Note ('{0}: {1} is in place and nothing newer was found.' -f $n, $v) }
        elseif (Test-Path -LiteralPath $p -PathType Leaf) { Write-Note ('{0}: in place, and nothing newer was found.' -f $n) }
        else { Write-Note ('{0}: not in place, and no download of it was found.' -f $n) }
    }
}
function Set-ItemPlan([string]$Label, [string[]]$Names) {
    if (-not $script:BestVer.ContainsKey($Names[0])) { Write-UpToDate $Names; return }
    $old = @()
    $newer = $false
    $older = $false
    foreach ($n in $Names) {
        $v = Get-VersionOf (Get-GamePath (Get-TargetDest $n))
        $old += $v
        if (-not $v) { $c = 1 } else { $c = Compare-Version $script:BestVer[$n] $v }
        if ($c -eq 1) { $newer = $true }
        if ($c -eq -1) { $older = $true }
    }
    if ($older -and -not $AllowOlder) {
        for ($k = 0; $k -lt $Names.Count; $k++) {
            $n = $Names[$k]
            if (-not $old[$k] -or (Compare-Version $script:BestVer[$n] $old[$k]) -ne -1) { continue }
            if ($Names.Count -eq 1) {
                Write-Note ('Left {0} alone: the copy in place ({1}) is newer than the download ({2}).' -f $n, $old[$k], $script:BestVer[$n])
                Write-Note 'To put the older one in anyway, run update with -AllowOlder.'
            } else {
                Write-Note ("Left {0} alone: {1} in place ({2}) is newer than the download's ({3})." -f $Label, $n, $old[$k], $script:BestVer[$n])
                Write-Note 'To put the older ones in anyway, run update with -AllowOlder.'
            }
            break
        }
        return
    }
    if (-not $newer -and -not $older) { Write-UpToDate $Names; return }
    for ($k = 0; $k -lt $Names.Count; $k++) {
        $mode = 'added'
        if (Test-Path -LiteralPath (Get-GamePath (Get-TargetDest $Names[$k])) -PathType Leaf) { $mode = 'replaced' }
        $script:Changes.Add(@{ Name = $Names[$k]; Mode = $mode; Old = $old[$k] })
    }
}
# Deletes every temporary name update may have made.
function Remove-UpdateParts {
    if (-not $script:Game) { return }
    foreach ($n in $Targets) {
        Remove-IfThere (Get-PartPath $n)
        Remove-IfThere (Get-PartPath ('cand.' + $n))
        Remove-IfThere ((Get-GamePath (Get-TargetDest $n)) + $Kept + $Part)
    }
    Remove-IfThere ((Get-GamePath $ListRel) + $Part)
    Remove-IfThere ((Get-GamePath $RecordRel) + $Part)
}
# Renames FROM over TO; TO may exist as a file.
function Move-Over([string]$FromPath, [string]$ToPath) {
    # [NullString]::Value: PowerShell would pass a plain $null to a .NET string parameter as an empty string.
    if (Test-Path -LiteralPath $ToPath -PathType Leaf) { [System.IO.File]::Replace($FromPath, $ToPath, [NullString]::Value) }
    else { [System.IO.File]::Move($FromPath, $ToPath) }
}
# Undoes the renames of changes First..Last.
function Restore-Changes([int]$First, [int]$Last) {
    for ($k = $First; $k -le $Last; $k++) {
        $d = Get-GamePath (Get-TargetDest $script:Changes[$k].Name)
        if ($script:Changes[$k].Mode -eq 'added') {
            Remove-IfThere $d
        } elseif (Test-Path -LiteralPath ($d + $Kept) -PathType Leaf) {
            [System.IO.File]::Copy($d + $Kept, $d + $Part, $true)
            Move-Over ($d + $Part) $d
        }
    }
}
# A failure before any file was replaced: the record and folders as they were, and nothing counts as done.
# Saved: the copy of the record was made in full; a cut copy is dropped and the record left alone.
function Undo-Preparation([string[]]$Made, [bool]$Saved) {
    $rec = Get-GamePath $RecordRel
    if ($Saved) { Move-Over ($rec + $Part) $rec } else { Remove-IfThere ($rec + $Part) }
    foreach ($m in $Made) {
        $dir = Get-GamePath $m
        if ((Test-Path -LiteralPath $dir -PathType Container) -and @(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) {
            [System.IO.Directory]::Delete($dir)
        }
    }
    foreach ($c in $script:Changes) { $c.Mode = 'failed' }
}
# A kept copy or the list could not be renamed: no file was replaced, and the earlier update's undo is gone.
function Write-PublishFailed([bool]$HadList) {
    foreach ($c in $script:Changes) { $c.Mode = 'failed' }
    if ($HadList) { Write-Note 'No file was changed, but the undo for the earlier update is no longer available.' }
}
function Invoke-Changes {
    $ch = $script:Changes
    $entries = @($ListRel)
    foreach ($c in $ch) {
        $d = Get-TargetDest $c.Name
        $entries += $d
        if ($c.Mode -eq 'replaced') { $entries += ($d + $Kept) }
    }
    # (a) the record names every file this run may leave behind, before any of them exists; a copy of it, and the
    # folders this run makes, let a failure before (c) leave the game folder as it was
    $rec = Get-GamePath $RecordRel
    $made = @()
    try { [System.IO.File]::Copy($rec, $rec + $Part, $true) } catch { Write-CopyFailed $RecordRel $_; Undo-Preparation $made $false; return 4 }
    foreach ($c in $ch) {
        $d = Get-FolderOf (Get-TargetDest $c.Name)
        while ($d -ne '.' -and -not (Test-Path -LiteralPath (Get-GamePath $d) -PathType Container)) { $made += $d; $d = Get-FolderOf $d }
    }
    try {
        foreach ($c in $ch) { [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent (Get-GamePath (Get-TargetDest $c.Name)))) }
        Add-RecordEntries $entries
    } catch { Write-CopyFailed $RecordRel $_; Undo-Preparation $made $true; return 4 }
    # (b) copies of the files about to be replaced, and the new list, under temporary names
    $lines = @()
    foreach ($c in $ch) {
        $lines += ($c.Mode + ' ' + (Get-TargetDest $c.Name))
        if ($c.Mode -ne 'replaced') { continue }
        $d = Get-GamePath (Get-TargetDest $c.Name)
        try { [System.IO.File]::Copy($d, $d + $Kept + $Part, $true) } catch { Write-CopyFailed $c.Name $_; Undo-Preparation $made $true; return 4 }
    }
    $list = Get-GamePath $ListRel
    try { [System.IO.File]::WriteAllBytes($list + $Part, $Latin1.GetBytes(($lines -join "`n") + "`n")) } catch { Write-CopyFailed $ListRel $_; Undo-Preparation $made $true; return 4 }
    # (c) From here on only renames and deletions remain, which need no free space. The earlier update's list goes
    # first: if a kept copy cannot be replaced, there is no undo rather than one that would mix two updates.
    Remove-IfThere ($rec + $Part)
    $hadList = Test-Path -LiteralPath $list -PathType Leaf
    Remove-IfThere $list
    foreach ($c in $ch) {
        if ($c.Mode -ne 'replaced') { continue }
        $d = Get-GamePath (Get-TargetDest $c.Name)
        try { Move-Over ($d + $Kept + $Part) ($d + $Kept) } catch { Write-CopyFailed $c.Name $_; Write-PublishFailed $hadList; return 4 }
    }
    try { Move-Over ($list + $Part) $list } catch { Write-CopyFailed $ListRel $_; Write-PublishFailed $hadList; return 4 }
    foreach ($n in $Targets) {   # one step only: kept copies from an earlier update go
        if (-not ($lines -ccontains ('replaced ' + (Get-TargetDest $n)))) { Remove-IfThere ((Get-GamePath (Get-TargetDest $n)) + $Kept) }
    }
    # (d) the new files into place, one item at a time (each NVIDIA file, AMD's set, Intel's set); an item that cannot
    # be completed is put back, so a set is never left mixed
    $rc = 0
    $first = 0
    while ($first -lt $ch.Count) {
        $item = Get-Vendor $ch[$first].Name
        $last = $first
        if ($item -ne 'NVIDIA') {
            while ($last + 1 -lt $ch.Count -and (Get-Vendor $ch[$last + 1].Name) -eq $item) { $last++ }
        }
        for ($k = $first; $k -le $last; $k++) {
            $n = $ch[$k].Name
            try {
                Move-Over (Get-PartPath $n) (Get-GamePath (Get-TargetDest $n))
            } catch {
                Write-CopyFailed $n $_
                Restore-Changes $first ($k - 1)
                for ($j = $first; $j -le $last; $j++) { $ch[$j].Mode = 'failed' }
                $rc = 4
                break
            }
        }
        $first = $last + 1
    }
    return $rc
}
function Write-Versions {
    Say ''
    Say 'Versions now in place'
    foreach ($n in $Targets) {
        $p = Get-GamePath (Get-TargetDest $n)
        $v = Get-VersionOf $p
        if ($v) { Write-Note ($n + ' ' + $v) }
        elseif (Test-Path -LiteralPath $p -PathType Leaf) { Write-Note ($n + ' (version not readable)') }
        else { Write-Note ($n + ' not in place') }
    }
}
function Test-Record {
    if (Test-Path -LiteralPath (Get-GamePath $RecordRel) -PathType Leaf) { return $true }
    Write-Note 'Run install first: no install record was found in this game folder.'
    return $false
}
function Invoke-Update {
    $steps = @('Game folder', 'Downloads', 'Files')
    Say 'Lumenveil: update'
    if (Test-GameRunning) {
        Say ''
        Write-Note 'Close FINAL FANTASY XV first, then run this again.'
        return 2
    }
    Write-Step $steps 'Game folder'
    $code = Select-GameFolder
    if ($code -ne 0) { return $code }
    if (-not (Test-Record)) { return 5 }
    $rc = 0
    try {
        Write-Step $steps 'Downloads'
        if (-not (Find-Candidates)) { return 1 }
        Select-One 'nvngx_dlss.dll'
        Select-One 'nvngx_dlssg.dll'
        Select-Set 'AMD FSR' $AmdSet
        Select-Set 'Intel XeSS' $IntelSet
        if ($script:StageFailed) {
            Write-Note "Nothing was changed. If the game's drive is full, free some space; if that download is damaged, download it again. Then run update again."
            return 4
        }
        Write-Step $steps 'Files'
        Set-ItemPlan 'nvngx_dlss.dll' @('nvngx_dlss.dll')
        Set-ItemPlan 'nvngx_dlssg.dll' @('nvngx_dlssg.dll')
        Set-ItemPlan 'AMD FSR' $AmdSet
        Set-ItemPlan 'Intel XeSS' $IntelSet
        $changed = $false
        $fg = $false
        if ($script:Changes.Count -gt 0) {
            $rc = Invoke-Changes
            foreach ($c in $script:Changes) {
                $old = $c.Old
                if (-not $old) { $old = 'an unknown version' }
                if ($c.Mode -eq 'replaced') { Write-Note ('{0}: updated from {1} to {2}.' -f $c.Name, $old, $script:BestVer[$c.Name]) }
                elseif ($c.Mode -eq 'added') { Write-Note ('{0}: added, version {1}.' -f $c.Name, $script:BestVer[$c.Name]) }
                else { continue }
                $changed = $true
                if ($c.Name -eq 'nvngx_dlssg.dll') { $fg = $true }
            }
        }
    } finally {
        Remove-UpdateParts
    }
    Write-Versions
    if ($changed) {
        Say ''
        Say 'The previous files are kept. To go back to them, run undo-update.sh on Linux or undo-update.bat on Windows.'
        if ($fg) { Say 'If frame generation stops working after this update, run the undo.' }
    }
    return $rc
}
function Invoke-Undo {
    $steps = @('Game folder', 'Undo')
    Say 'Lumenveil: undo the last update'
    if (Test-GameRunning) {
        Say ''
        Write-Note 'Close FINAL FANTASY XV first, then run this again.'
        return 2
    }
    Write-Step $steps 'Game folder'
    $code = Select-GameFolder
    if ($code -ne 0) { return $code }
    if (-not (Test-Record)) { return 5 }
    Write-Step $steps 'Undo'
    $list = Get-GamePath $ListRel
    if (-not (Test-Path -LiteralPath $list -PathType Leaf)) {
        Write-Note 'Nothing to undo.'
        return 0
    }
    $rc = 0
    $left = @()
    foreach ($line in $Latin1.GetString([System.IO.File]::ReadAllBytes($list)).Split("`n")) {
        $l = $line.TrimEnd("`r")
        if ($l -eq '') { continue }
        $k = $l.IndexOf(' ')
        $mode = ''
        $p = ''
        if ($k -gt 0) { $mode = $l.Substring(0, $k); $p = $l.Substring($k + 1) }
        if (($mode -cne 'replaced' -and $mode -cne 'added') -or -not (Test-TargetPath $p)) {
            Write-Note ('Skipped "{0}": not one of the files update changes.' -f $l)
            continue
        }
        $leaf = $p.Substring($p.LastIndexOf('/') + 1)
        $d = Get-GamePath $p
        if ($mode -eq 'added') {
            Remove-IfThere $d
            Write-Note ('Removed {0}, which the update added.' -f $leaf)
            $dir = Split-Path -Parent $d   # the folder too, when the update made it and it is empty
            if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { [System.IO.Directory]::Delete($dir) }
        } elseif (-not (Test-Path -LiteralPath ($d + $Kept) -PathType Leaf)) {
            Write-Note ('The kept copy of {0} is missing, so {0} was left as it is.' -f $leaf)
            $rc = 4
        } else {
            try {
                Move-Over ($d + $Kept) $d
                Write-Note ('Put back the previous {0} ({1}).' -f $leaf, (Get-VersionOf $d))
            } catch {
                Write-CopyFailed $leaf $_
                $left += $l
                $rc = 4
            }
        }
    }
    if ($left.Count -eq 0) {
        Remove-File $list
    } else {
        try {
            [System.IO.File]::WriteAllBytes($list + $Part, $Latin1.GetBytes(($left -join "`n") + "`n"))
            Move-Over ($list + $Part) $list
            Write-Note 'Close anything that may be using these files, then run the undo again to finish.'
        } catch { Remove-IfThere ($list + $Part) }
    }
    if ($rc -eq 0) { Write-Note 'Done: the files are as they were before the last update.' }
    return $rc
}

# ---- install --------------------------------------------------------------------------------------------------
function Write-CopyFailed([string]$File, $Problem) {
    Write-Note ('Could not copy {0}: {1}' -f $File, $Problem.Exception.Message)
    $bat = @{ install = 'install.bat'; uninstall = 'uninstall.bat'; update = 'update.bat'; undo = 'undo-update.bat' }[$Action]
    Write-Note ('If the game folder is protected, right-click {0} and choose Run as administrator.' -f $bat)
}
function Install-ModFiles {
    if (-not (Test-Path -LiteralPath $Payload -PathType Container)) {
        Write-Note "The release's game folder is missing. Extract the whole zip, then run this again."
        return 4
    }
    $root = [System.IO.Path]::GetFullPath($Payload).TrimEnd('\', '/')
    [string[]]$files = @(Get-ChildItem -LiteralPath $root -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') })
    [Array]::Sort($files, [System.StringComparer]::Ordinal)
    $dxgi = Get-GamePath 'dxgi.dll'
    $keptName = 'dxgi.dll' + $Keep
    $kept = Get-GamePath $keptName
    $foreign = (Test-Path -LiteralPath $dxgi -PathType Leaf) -and -not (Test-HasMark $dxgi)
    if ($foreign -and (Test-Path -LiteralPath $kept)) {
        Write-Note ("The game folder holds another tool's dxgi.dll, and {0} is there too." -f $keptName)
        Write-Note 'Move one of them out of the game folder, then run this again. Nothing was changed.'
        return 4
    }
    try {
        [void][System.IO.Directory]::CreateDirectory((Get-GamePath 'ffxv_dlss'))
        Add-RecordEntries ($files + @($RecordRel))
    } catch {
        Write-CopyFailed $RecordRel $_
        return 4
    }
    if ($foreign) {
        try { [System.IO.File]::Move($dxgi, $kept) } catch { Write-CopyFailed 'dxgi.dll' $_; return 4 }
        Write-Note ("Another tool's dxgi.dll was in the game folder. It is kept as {0}" -f $keptName)
        Write-Note 'and comes back when you uninstall.'
    }
    foreach ($f in $files) {
        try {
            $to = Get-GamePath $f
            [void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($to))
            $from = (Join-Path $root $f)
            Copy-Bytes $from $to
        } catch {
            Write-CopyFailed $f $_
            return 4
        }
    }
    Write-Note ('Copied {0} files.' -f $files.Count)
    return 0
}
function Set-FpsLimit {
    $files = @()
    try { $files = @(Get-SettingsFiles '') } catch { $files = @() }
    if ($files.Count -eq 0) {
        Write-Note "Skipped: the game's settings file was not found. Start the game once, change any graphics"
        Write-Note 'option, then run this again, or set MaxFramerate=240 by hand in'
        Write-Note 'Documents\My Games\FINAL FANTASY XV\Steam\<number>\savestorage\GraphicsConfig.ini'
        return 'skipped'
    }
    $status = 'already done'
    foreach ($f in $files) {
        $acct = Get-AccountOf $f
        try {
            $bytes = [System.IO.File]::ReadAllBytes($f)
        } catch {
            Write-Note ('Account {0}: failed, the settings file is read-only or in use. Set MaxFramerate=240 by hand in {1}' -f $acct, $f)
            $status = 'failed'
            continue
        }
        $text = $Latin1.GetString($bytes)
        $n = Get-IniNumber $text 'MaxFramerate'
        if ($null -eq $n) {
            Write-Note ('Account {0}: skipped, the file has no MaxFramerate line. Set MaxFramerate=240 by hand in {1}' -f $acct, $f)
            $status = 'skipped'
            continue
        }
        if (Test-AtLeast $n 240) {
            Write-Note ('Account {0}: already {1} fps, left as it is.' -f $acct, $n)
            continue
        }
        try {
            $probe = [System.IO.File]::Open($f, 'Open', 'ReadWrite', 'None')
            $probe.Dispose()
        } catch {
            Write-Note ('Account {0}: failed, the settings file is read-only or in use. Set MaxFramerate=240 by hand in {1}' -f $acct, $f)
            $status = 'failed'
            continue
        }
        try {
            $backup = $f + $Keep
            if (-not (Test-Path -LiteralPath $backup)) { [System.IO.File]::WriteAllBytes($backup, $bytes) }
            [System.IO.File]::WriteAllBytes($f, $Latin1.GetBytes((Set-IniNumber $text 'MaxFramerate' '240')))
            if ((Get-IniNumber $Latin1.GetString([System.IO.File]::ReadAllBytes($f)) 'MaxFramerate') -ne '240') { throw 'the new value did not stick' }
            Write-Note ("Account {0}: raised the game's limit from {1} to 240 fps. Your old settings file is kept as" -f $acct, $n)
            Write-Note ('GraphicsConfig.ini' + $Keep)
            if ($status -eq 'already done') { $status = 'done' }
        } catch {
            Write-Note ('Account {0}: failed to change the settings file. Set MaxFramerate=240 by hand in {1}' -f $acct, $f)
            $status = 'failed'
        }
    }
    return $status
}
function Invoke-Install {
    $steps = @('Game folder', 'Mod files', 'NVIDIA DLSS files', 'AMD FSR files', 'Intel XeSS files', 'Frame rate limit')
    Say 'Lumenveil: install'
    if (Test-GameRunning) {
        Say ''
        Write-Note 'Close FINAL FANTASY XV first, then run this again.'
        return 2
    }
    Write-Step $steps 'Game folder'
    $code = Select-GameFolder
    if ($code -ne 0) { return $code }
    Write-Step $steps 'Mod files'
    $code = Install-ModFiles
    if ($code -ne 0) { return $code }
    Write-Step $steps 'NVIDIA DLSS files'
    $nvidia = Install-NvidiaFiles
    Write-Step $steps 'AMD FSR files'
    $fsr = Install-FsrFiles
    Write-Step $steps 'Intel XeSS files'
    $xess = Install-XessFiles
    Write-Step $steps 'Frame rate limit'
    $fps = Set-FpsLimit
    Say ''
    Say 'Summary'
    Write-Note 'Game folder: found'
    Write-Note 'Mod files: done'
    Write-Note ('NVIDIA DLSS files: ' + $nvidia)
    Write-Note ('AMD FSR files: ' + $fsr + ' (optional)')
    Write-Note ('Intel XeSS files: ' + $xess + ' (optional)')
    Write-Note ('Frame rate limit: ' + $fps)
    Say ''
    if ($nvidia -eq 'skipped' -or $nvidia -eq 'failed') {
        Say ("Still to do: add NVIDIA's DLSS files (step {0}). Then start the game and press Insert for the mod's menu." -f (Get-StepNumber $steps 'NVIDIA DLSS files'))
    } else {
        Say "All done. Start the game and press Insert for the mod's menu."
    }
    if ($xess -eq 'skipped') {
        Say ('Still to do, if you want Intel XeSS (optional, step {0}): put XeSS_SDK_3.0.2.zip from {1}' -f (Get-StepNumber $steps 'Intel XeSS files'), $XessUrl)
        Say 'in your Downloads folder and run this again.'
    }
    Say 'To remove the mod later, run uninstall.bat.'
    return 0
}

# ---- uninstall --------------------------------------------------------------------------------------------------
function Restore-FpsLimit {
    $backups = @()
    try { $backups = @(Get-SettingsFiles $Keep) } catch { $backups = @() }
    foreach ($b in $backups) {
        $f = $b.Substring(0, $b.Length - $Keep.Length)
        $acct = Get-AccountOf $f
        try {
            $old = Get-IniNumber $Latin1.GetString([System.IO.File]::ReadAllBytes($b)) 'MaxFramerate'
        } catch {
            Write-Note ('Account {0}: failed to read the backup, so it was left as it is: {1}' -f $acct, $b)
            continue
        }
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
            Write-Note ('Account {0}: the settings file is gone, so its backup was left: {1}' -f $acct, $b)
            continue
        }
        try {
            $text = $Latin1.GetString([System.IO.File]::ReadAllBytes($f))
        } catch {
            $shown = $old
            if (-not $shown) { $shown = '<your old value>' }
            Write-Note ('Account {0}: failed, the settings file is in use. Set MaxFramerate={1} by hand in {2}' -f $acct, $shown, $f)
            Write-Note ('Your old settings file is kept as {0}' -f $b)
            continue
        }
        $now = Get-IniNumber $text 'MaxFramerate'
        if ($old -and $now -eq '240') {
            try {
                [System.IO.File]::WriteAllBytes($f, $Latin1.GetBytes((Set-IniNumber $text 'MaxFramerate' $old)))
                Write-Note ("Account {0}: set the game's limit back to {1} fps." -f $acct, $old)
            } catch {
                Write-Note ('Account {0}: failed to change the settings file. Set MaxFramerate={1} by hand in {2}' -f $acct, $old, $f)
                continue
            }
        } else {
            $shown = $now
            if (-not $shown) { $shown = 'not set' }
            Write-Note ('Account {0}: your limit is now {1}, left as it is.' -f $acct, $shown)
        }
        Remove-File $b
    }
    if ($backups.Count -eq 0) { Write-Note 'Nothing to put back.' }
    $files = @()
    try { $files = @(Get-SettingsFiles '') } catch { $files = @() }
    foreach ($f in $files) {
        $r = $null
        try { $r = Get-IniNumber $Latin1.GetString([System.IO.File]::ReadAllBytes($f)) 'RenderingResolutionRatio' } catch { continue }
        if ($r -and $r -ne '100') {
            Write-Note ("The game's render scale is {0}%. Set Resolution back to 100% in the game's graphics options" -f $r)
            Write-Note 'if you lowered it for DLSS.'
        }
    }
}
function Remove-ModFiles {
    $rec = Get-GamePath $RecordRel
    if (-not (Test-Path -LiteralPath $rec -PathType Leaf)) {
        Write-Note 'No install record found, nothing was removed.'
        Write-Note "To remove the mod by hand, delete these from the game folder: dxgi.dll (if it is this mod's),"
        Write-Note 'ffxv_dlss.ini, ffxv_dlss.log and the ffxv_dlss folder.'
        return 5
    }
    $removed = 0
    $dirs = New-Object 'System.Collections.Generic.List[string]'
    foreach ($line in $Latin1.GetString([System.IO.File]::ReadAllBytes($rec)).Split("`n")) {
        $e = $line.TrimEnd("`r")
        if ($e -eq '' -or $e -ceq $RecordRel) { continue }
        if (-not (Test-RecordEntry $e)) {
            Write-Note ("Skipped {0}: not one of the mod's own files." -f $e)
            continue
        }
        $p = Get-GamePath $e
        if ($e -ceq 'dxgi.dll') {
            if ((Test-Path -LiteralPath $p -PathType Leaf) -and (Test-HasMark $p)) { Remove-File $p; $removed++ }
            continue
        }
        if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-File $p; $removed++ }
        $dirs.Add($e.Substring(0, $e.LastIndexOf('/')))
    }
    Remove-File $rec
    $written = @((Get-GamePath 'ffxv_dlss.ini'), (Get-GamePath 'ffxv_dlss.log'))
    foreach ($d in @('ffxv_dlss', 'ffxv_dlss/streamline')) {
        $dp = Get-GamePath $d
        if (Test-Path -LiteralPath $dp -PathType Container) {
            $written += @(Get-ChildItem -LiteralPath $dp -File -Filter '*.log' -Force | ForEach-Object { $_.FullName })
        }
    }
    foreach ($p in $written) {
        if (Test-Path -LiteralPath $p -PathType Leaf) { Remove-File $p; $removed++ }
    }
    Write-Note ('Removed {0} files.' -f $removed)
    $dirs.Add('ffxv_dlss/streamline')
    $dirs.Add('ffxv_dlss/fonts')
    $dirs.Add($FsrDir)
    $dirs.Add($XessDir)
    $dirs.Add('ffxv_dlss')
    foreach ($d in @($dirs | Select-Object -Unique | Sort-Object -Descending -Property { $_.Split('/').Count })) {
        $dp = Get-GamePath $d
        if ((Test-Path -LiteralPath $dp -PathType Container) -and @(Get-ChildItem -LiteralPath $dp -Force).Count -eq 0) {
            [System.IO.Directory]::Delete($dp)
        }
    }
    $modDir = Get-GamePath 'ffxv_dlss'
    if (Test-Path -LiteralPath $modDir -PathType Container) {
        $root = [System.IO.Path]::GetFullPath($modDir).TrimEnd('\', '/')
        $left = @(Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object { -not $_.PSIsContainer } |
                ForEach-Object { 'ffxv_dlss/' + $_.FullName.Substring($root.Length + 1).Replace('\', '/') })
        if ($left.Count -gt 0) {
            Write-Note 'These files were not installed by this script and were left alone:'
            foreach ($l in $left) { Write-Note ('   ' + $l) }
        }
    }
    $dxgi = Get-GamePath 'dxgi.dll'
    $keptName = 'dxgi.dll' + $Keep
    $kept = Get-GamePath $keptName
    if (Test-Path -LiteralPath $kept -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $dxgi)) {
            [System.IO.File]::Move($kept, $dxgi)
            Write-Note "Put the other tool's dxgi.dll back."
        } else {
            Write-Note ("dxgi.dll in the game folder is not this mod's, so it and {0} were both left as they are." -f $keptName)
        }
    }
    return 0
}
function Invoke-Uninstall {
    Say 'Lumenveil: uninstall'
    if (Test-GameRunning) {
        Say ''
        Write-Note 'Close FINAL FANTASY XV first, then run this again.'
        return 2
    }
    $steps = @('Game folder', 'Frame rate limit', 'Mod files')
    Write-Step $steps 'Game folder'
    $code = Select-GameFolder
    if ($code -ne 0) { return $code }
    Write-Step $steps 'Frame rate limit'
    Restore-FpsLimit
    Write-Step $steps 'Mod files'
    $code = Remove-ModFiles
    if ($code -ne 0) { return $code }
    Say ''
    Say 'Done.'
    return 0
}
function Invoke-Setup {
    switch ($Action) {
        'install' { return (Invoke-Install) }
        'update' { return (Invoke-Update) }
        'undo' { return (Invoke-Undo) }
    }
    return (Invoke-Uninstall)
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $exitCode = Invoke-Setup
    } catch {
        Write-Host ('Something unexpected went wrong: ' + $_.Exception.Message)
        $exitCode = 1
    }
    exit $exitCode
}
