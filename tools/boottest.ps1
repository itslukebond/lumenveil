# Launch FFXV with given ini switches and report whether it survives startup.
# usage: boottest.ps1 [-game <FFXV folder, default $env:FFXV_DIR>] [-stage 3] [-cb 1] [-ui 1] [-ps 1] [-map 1] [-upd 1] [-seconds 75]
param([string]$game = $env:FFXV_DIR, [int]$cb = 1, [int]$ui = 1, [int]$ps = 1, [int]$map = 1, [int]$upd = 1, [int]$stage = 3, [int]$swap = 0, [int]$mv = 1, [int]$seconds = 75)
if (-not $game) { throw "pass -game <FINAL FANTASY XV folder> or set FFXV_DIR" }
$g = $game
Get-Process ffxv_s -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill(); $_.WaitForExit(15000) | Out-Null }
Copy-Item "$PSScriptRoot\..\build\dxgi.dll" "$g\dxgi.dll" -Force
Set-Content "$g\ffxv_dlss.ini" @"
[dlss]
; 0 = SDK default (transformer preset K for DLAA/Quality), 11 = K, 12 = L, 13 = M
preset=0
jitter_sign_x=1
jitter_sign_y=1
shot_x=0
shot_y=1440
stage=$stage
swap=$swap
mv=$mv

[hooks]
cb=$cb
ui=$ui
ps=$ps
map=$map
upd=$upd
"@
$p = Start-Process "$g\ffxv_s.exe" -WorkingDirectory $g -PassThru
$exited = $p.WaitForExit($seconds * 1000)
$log = Get-Content "$g\ffxv_dlss.log"
$fps = ($log | Select-String '^fps' | Select-Object -Last 1).Line
"stage=$stage swap=$swap mv=$mv cb=$cb ui=$ui ps=$ps map=$map upd=$upd -> " + $(if ($exited) { "CRASHED" } else { "alive" }) + " | last: $fps"
$log | Select-String '^(AV at|dlss:|hook ctx)' | Select-Object -First 20 | ForEach-Object { "  " + $_.Line }
if (-not $exited) { $p.Kill(); $p.WaitForExit(15000) | Out-Null }
