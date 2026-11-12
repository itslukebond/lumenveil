# Calls scripts/setup.ps1's helpers directly (dot-sourced), with Windows-shaped text, and drives its prompt.
# Usage: pwsh -NoProfile -File tests/unit.ps1 -Case parser|prompt|pe|signer -Script <setup.ps1> [fixture paths]
param(
    [Parameter(Mandatory = $true)][string]$Case,
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$SteamRoot, [string]$DocumentsDir, [string]$DownloadsDir, [string]$Answer1, [string]$Answer2, [string]$Fixtures
)
$failed = 0
function Check([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Output "ok   $Name" } else { Write-Output "FAIL $Name"; $script:failed++ }
}

if ($Case -eq 'parser') {
    . $Script -Action install
    $vdf = "`"libraryfolders`"`r`n{`r`n`t`"0`"`r`n`t{`r`n`t`t`"path`"`t`t`"C:\\Program Files (x86)\\Steam`"`r`n`t`t`"label`"`t`t`"`"`r`n`t}`r`n" +
           "`t`"1`"`r`n`t{`r`n`t`t`"path`"`t`t`"D:\\Steam Library`"`r`n`t`t`"apps`"`r`n`t`t{`r`n`t`t`t`"637650`"`t`t`"1`"`r`n`t`t}`r`n`t}`r`n}`r`n"
    $libs = @(Get-LibraryPaths $vdf)
    Check 'two libraries' ($libs.Count -eq 2)
    Check 'first library, doubled backslashes undone' ($libs[0] -ceq 'C:\Program Files (x86)\Steam')
    Check 'second library with a space' ($libs[1] -ceq 'D:\Steam Library')
    $acf = "`"AppState`"`n{`n`t`"appid`"`t`t`"637650`"`n`t`"installdir`"`t`t`"FINAL FANTASY XV`"`n}`n"
    Check 'installdir' ((Get-InstallDir $acf) -ceq 'FINAL FANTASY XV')
    $ini = "[BasicSettings]`r`nRenderingResolutionRatio=100`r`nMaxFramerate=120`r`nVSync=0`r`n"
    Check 'ini read' ((Get-IniNumber $ini 'MaxFramerate') -eq 120)
    Check 'ini read, missing key' ($null -eq (Get-IniNumber $ini 'Nope'))
    Check 'ini set keeps CRLF and every other byte' ((Set-IniNumber $ini 'MaxFramerate' 240) -ceq "[BasicSettings]`r`nRenderingResolutionRatio=100`r`nMaxFramerate=240`r`nVSync=0`r`n")
    $names = @('DLSS-main/', 'DLSS-main/lib/Windows_x86_64/dev/nvngx_dlss.dll', 'DLSS-main/lib/Windows_x86_64/rel/nvngx_dlss.dll',
               'DLSS-main/lib/Windows_x86_64/dev/nvngx_dlssg.dll', 'DLSS-main/lib/Windows_x86_64/rel/nvngx_dlssg.dll')
    Check 'SDK entry: rel, not dev' ((Find-SdkEntry $names 'nvngx_dlss.dll') -ceq 'DLSS-main/lib/Windows_x86_64/rel/nvngx_dlss.dll')
    Check 'SDK entry: dlssg' ((Find-SdkEntry $names 'nvngx_dlssg.dll') -ceq 'DLSS-main/lib/Windows_x86_64/rel/nvngx_dlssg.dll')
    Check 'SDK entry: none' ($null -eq (Find-SdkEntry @('DLSS-Sample/bin/nvngx_dlss.dll') 'nvngx_dlss.dll'))
    foreach ($e in @('dxgi.dll', 'ffxv_dlss/nvngx_dlss.dll', 'ffxv_dlss/streamline/sl.common.dll', 'ffxv_dlss/installed_files.txt')) {
        Check "record entry accepted: $e" (Test-RecordEntry $e)
    }
    foreach ($e in @('../x', 'ffxv_dlss/../x', 'ffxv_dlss\..\..\x', 'C:\x', 'C:/x', 'c:x', '/etc/x', '\\server\x', 'ffxv_s.exe',
                     'ffxv_dlss', 'ffxv_dlss/', 'dxgi.dll.before_ffxv_dlss', 'ffxv_dlss/a:stream', '', 'ffxv_dlss//x',
                     'ffxv_dlss/.. /x', 'ffxv_dlss/x/.', 'ffxv_dlss/x.', 'DXGI.DLL')) {
        Check "record entry refused: '$e'" (-not (Test-RecordEntry $e))
    }
    if ($failed) { exit 1 } else { exit 0 }
}

if ($Case -eq 'prompt') {
    . $Script -Action install -SteamRoot $SteamRoot -DocumentsDir $DocumentsDir -DownloadsDir $DownloadsDir
    $script:answers = @($Answer1, $Answer2)
    $script:asked = 0
    function Test-CanPrompt { return $true }
    function Read-Answer([string]$Question) {
        Write-Host "$Question"
        if ($script:asked -lt $script:answers.Count) { $a = $script:answers[$script:asked] } else { $a = '' }
        $script:asked++
        return $a
    }
    $code = Invoke-Setup
    exit $code
}
# pe: the header reader on the fixtures in $Fixtures (made by tests/cases.sh), and on damaged copies made here.
if ($Case -eq 'pe') {
    . $Script -Action update
    $want = @(
        @('dlss.dll', '310.9.1.0', 'NVIDIA', 'NVIDIA Deep Learning SuperSampling', 'NVIDIA DLSS - DVS PRODUCTION', 'CL 38868064'),
        @('dlssg.dll', '310.9.1.0', 'NVIDIA', 'NVIDIA DLSS-G MFGLW', 'NVIDIA DLSS-G MFGLW - DVS PRODUCTION', 'CL 38868064'),
        @('up.dll', '4.1.2.4555', 'Advanced Micro Devices, Inc.', 'AMD FidelityFX Upscaler', 'AMD FidelityFX (DirectX) Upscaler Library', $null),
        @('fg.dll', '4.0.2.4555', 'Advanced Micro Devices, Inc.', 'AMD FidelityFX Frame Generation', 'AMD FidelityFX (DirectX) Frame Generation Library', $null),
        @('ld.dll', '2.3.0.4555', 'Advanced Micro Devices, Inc.', 'AMD FidelityFX', 'AMD FidelityFX (DirectX) Loader Library', $null),
        @('xs.dll', '2.0.2.68', 'Intel Corporation', 'XeSS SDK', 'XeSS SDK', 'libxess.dll'),
        @('xfg.dll', '1.3.1.78', 'Intel Corporation', 'XeSS Frame Generation SDK', 'XeSS Frame Generation SDK', 'libxess_fg.dll'),
        @('xl.dll', '1.3.2.10', 'Intel Corporation', 'XeLL SDK', 'XeLL SDK', 'libxell.dll'))
    foreach ($w in $want) {
        $i = Get-PeInfo (Join-Path $Fixtures $w[0])
        Check "pe $($w[0]): read in full" ($i.Bad -ceq '')
        Check "pe $($w[0]): version $($w[1])" ($i.Ver -ceq $w[1])
        Check "pe $($w[0]): company" ($i.Company -ceq $w[2])
        Check "pe $($w[0]): product" ($i.Product -ceq $w[3])
        Check "pe $($w[0]): description" ($i.Desc -ceq $w[4])
        if ($null -ne $w[5]) { Check "pe $($w[0]): original file name" ($i.Orig -ceq $w[5]) }
        Check "pe $($w[0]): signature block inside the file" ($i.CertLen -ge 8 -and ($i.CertOff + $i.CertLen) -le $i.Size)
    }
    foreach ($c in @(@('arm64.dll', 'notdll'), @('32bit.dll', 'notdll'), @('exe.dll', 'notdll'), @('text.dll', 'notdll'),
                     @('nover.dll', 'nover'))) {
        Check "pe $($c[0]): $($c[1])" ((Get-PeInfo (Join-Path $Fixtures $c[0])).Bad -ceq $c[1])
    }
    $cut = Get-PeInfo (Join-Path $Fixtures 'cut.dll')
    Check 'pe cut.dll: header read, signature block past the end' ($cut.Bad -ceq '' -and ($cut.CertOff + $cut.CertLen) -gt $cut.Size)
    Check 'pe missing file: notdll' ((Get-PeInfo (Join-Path $Fixtures 'none.dll')).Bad -ceq 'notdll')
    Check 'pe missing file: no version' ((Get-VersionOf (Join-Path $Fixtures 'none.dll')) -ceq '')
    # Damaged copies of dlss.dll: the header cut at each of these lengths, and fields set past their caps.
    # Layout of the fixture (tests/cases.sh mkpe): header 0x200 bytes, then the resource section at file offset 0x200.
    $good = [System.IO.File]::ReadAllBytes((Join-Path $Fixtures 'dlss.dll'))
    $tmp = Join-Path $Fixtures 'damaged.dll'
    foreach ($len in @(0, 1, 2, 0x3C, 0x40, 0x84, 0x9A, 0x100, 0x188, 0x200, 0x210, 0x248, 0x258, 0x280)) {
        $part = New-Object byte[] $len
        [Array]::Copy($good, $part, $len)
        [System.IO.File]::WriteAllBytes($tmp, $part)
        $r = $null
        try { $r = Get-PeInfo $tmp } catch { $r = $null }
        Check "pe cut to $len bytes: refused, no error" ($null -ne $r -and $r.Bad -cne '' -and $r.Ver -ceq '')
    }
    $patches = @(
        @('section count 0xFFFF', 0x86, @(0xFF, 0xFF), 'notdll'),
        @('directory count 0xFFFFFFFF', (0x98 + 108), @(0xFF, 0xFF, 0xFF, 0xFF), ''),
        @('directory count 4', (0x98 + 108), @(4, 0, 0, 0), 'nover'),
        @('resource entries 0xFFFF', (0x200 + 14), @(0xFF, 0xFF), 'nover'),
        @('version size 0x7FFFFFFF', (0x200 + 0x48 + 4), @(0xFF, 0xFF, 0xFF, 0x7F), 'nover'),
        @('version address past every section', (0x200 + 0x48), @(0, 0, 0, 0x70), 'short'),
        @('resource address past every section', (0x98 + 128), @(0, 0, 0, 0x70), 'nover'),
        @('header offset past the end', 0x3C, @(0xFF, 0xFF, 0xFF, 0x7F), 'notdll'))
    foreach ($p in $patches) {
        $b = [byte[]]$good.Clone()
        for ($k = 0; $k -lt $p[2].Count; $k++) { $b[$p[1] + $k] = [byte]$p[2][$k] }
        [System.IO.File]::WriteAllBytes($tmp, $b)
        $r = $null
        try { $r = Get-PeInfo $tmp } catch { $r = $null }
        Check "pe $($p[0]): $($p[3]), no error" ($null -ne $r -and $r.Bad -ceq $p[3])
    }
    Remove-Item -LiteralPath $tmp
    # Versions compare number by number.
    foreach ($v in @(@('310.10.0.0', '310.9.1.0', 1), @('310.9.1.0', '310.10.0.0', -1), @('310.9.1.0', '310.9.1.0', 0),
                     @('2.0.2.68', '2.0.10.1', -1), @('4.1.2.4555', '4.1.2.4554', 1), @('1.3.1.78', '1.3.2.10', -1),
                     @('2.3.0.4555', '2.0.0.1', 1), @('1.0', '1.0.0.0', 0))) {
        Check "version $($v[0]) against $($v[1]): $($v[2])" ((Compare-Version $v[0] $v[1]) -eq $v[2])
    }
    if ($failed) { exit 1 } else { exit 0 }
}

# signer: what Windows says about a signature, and that the check asks Windows only on Windows.
if ($Case -eq 'signer') {
    . $Script -Action update
    $nv = 'CN=NVIDIA Corporation, O=NVIDIA Corporation, L=Santa Clara, S=California, C=US'
    $amd = 'CN="Advanced Micro Devices, Inc.", O="Advanced Micro Devices, Inc.", L=Santa Clara, S=California, C=US'
    $intel = 'CN=Intel Corporation, O=Intel Corporation, L=Santa Clara, S=California, C=US'
    $table = @(
        @('Valid', $nv, 'NVIDIA', '', $false),
        @('Valid', $amd, 'AMD', '', $false),
        @('Valid', $intel, 'Intel', '', $false),
        @('Valid', $intel, 'NVIDIA', 'its digital signature does not name NVIDIA', $false),
        @('Valid', 'CN=Example Signing Ltd', 'AMD', 'its digital signature does not name AMD', $false),
        @('Valid', '', 'Intel', 'its digital signature does not name Intel', $false),
        @('NotSigned', '', 'NVIDIA', 'it has no digital signature', $false),
        @('HashMismatch', $nv, 'NVIDIA', 'its digital signature does not match the file', $false),
        @('NotTrusted', $nv, 'NVIDIA', 'Windows does not trust its digital signature', $false),
        @('UnknownError', '', 'NVIDIA', '', $true),
        @('NotSupportedFileFormat', '', 'AMD', '', $true),
        @('Incompatible', '', 'Intel', '', $true))
    foreach ($r in $table) {
        $v = Get-SignatureVerdict $r[0] $r[1] $r[2]
        Check "signer $($r[0]) $($r[2]) '$($r[1])': refuse '$($r[3])'" ($v.Refuse -ceq $r[3])
        Check "signer $($r[0]) $($r[2]): note $($r[4])" ([bool]$v.Note -eq $r[4])
    }
    # Through the file check, with Windows' answer stood in for.
    $script:notes = @()
    function Write-Note([string]$Text) { $script:notes += $Text }
    $script:asked = 0
    $script:status = 'Valid'
    $script:subject = $nv
    function Get-AuthenticodeSignature([string]$LiteralPath) {
        $script:asked++
        if ($script:status -ceq 'throw') { throw 'cannot check' }
        $cert = $null
        if ($script:subject) { $cert = [pscustomobject]@{ Subject = $script:subject } }
        return [pscustomobject]@{ Status = $script:status; SignerCertificate = $cert }
    }
    $dll = Join-Path $Fixtures 'dlss.dll'
    function Test-OnWindows { return $false }
    $script:status = 'HashMismatch'
    Check 'signer not on Windows: Windows not asked' ((Test-VendorDll $dll 'nvngx_dlss.dll').Why -ceq '' -and $script:asked -eq 0)
    function Test-OnWindows { return $true }
    $script:status = 'Valid'
    Check 'signer on Windows, valid NVIDIA: passes' ((Test-VendorDll $dll 'nvngx_dlss.dll').Why -ceq '' -and $script:asked -eq 1)
    $script:status = 'HashMismatch'
    Check 'signer on Windows, hash mismatch: refused' ((Test-VendorDll $dll 'nvngx_dlss.dll').Why -ceq 'its digital signature does not match the file')
    $script:status = 'Valid'
    $script:subject = $intel
    Check 'signer on Windows, valid but Intel: refused' ((Test-VendorDll $dll 'nvngx_dlss.dll').Why -ceq 'its digital signature does not name NVIDIA')
    $script:status = 'throw'
    $script:notes = @()
    Check 'signer on Windows, the check fails: passes on labels' ((Test-VendorDll $dll 'nvngx_dlss.dll').Why -ceq '')
    Check 'signer on Windows, the check fails: says so' (($script:notes -join '|') -ceq 'nvngx_dlss.dll: Windows could not check its digital signature, so only its labels were checked.')
    $script:asked = 0
    $script:status = 'Valid'
    $script:subject = $nv
    Check 'signer on Windows, labels wrong: Windows not asked' ((Test-VendorDll (Join-Path $Fixtures 'nover.dll') 'nvngx_dlss.dll').Why -ceq 'it has no version information' -and $script:asked -eq 0)
    if ($failed) { exit 1 } else { exit 0 }
}
Write-Output "unknown case $Case"
exit 1
