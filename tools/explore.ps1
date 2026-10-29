# Runs a scripted headless session and saves a contact sheet of the captured frames.
# Usage: tools\explore.ps1 -Rom <rom> -Inp "800-804:START;..." -ExitAt 1800 -Every 30 -From 800 -Out sheet.png [-Inp2 "..."]
param([string]$Rom, [string]$Inp, [string]$Inp2 = "", [string]$Inp3 = "", [string]$Inp4 = "", [int]$ExitAt, [int]$Every = 30, [int]$From = 0, [string]$Out)
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path $root "build\explore"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
$env:MK64_HEADLESS = "1"; $env:MK64_INPUT = $Inp; $env:MK64_EXIT_AT = "$ExitAt"; $env:MK64_SHOTS = "${Every}:${From}"
if ($Inp2) { $env:MK64_INPUT2 = $Inp2 } else { $env:MK64_INPUT2 = $null }
if ($Inp3) { $env:MK64_INPUT3 = $Inp3 } else { $env:MK64_INPUT3 = $null }
if ($Inp4) { $env:MK64_INPUT4 = $Inp4 } else { $env:MK64_INPUT4 = $null }
Push-Location $work
& (Join-Path $root "build\mk64.exe") $Rom | Out-Null
Pop-Location
Remove-Item Env:MK64_HEADLESS, Env:MK64_INPUT, Env:MK64_EXIT_AT, Env:MK64_SHOTS -ErrorAction SilentlyContinue
Remove-Item Env:MK64_INPUT2, Env:MK64_INPUT3, Env:MK64_INPUT4 -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem (Join-Path $work "frame_*.bmp") | Sort-Object Name
$cols = 8; $rows = [math]::Ceiling($files.Count / $cols)
$sheet = New-Object System.Drawing.Bitmap (160 * $cols), (132 * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font "Arial", 8
$i = 0
foreach ($f in $files) {
    $img = [System.Drawing.Image]::FromFile($f.FullName)
    $x = ($i % $cols) * 160; $y = [math]::Floor($i / $cols) * 132
    $g.DrawImage($img, $x, $y, 160, 120); $img.Dispose()
    $g.DrawString(($f.BaseName -replace "frame_0*", ""), $font, [System.Drawing.Brushes]::Yellow, $x, $y + 119)
    $i++
}
$sheet.Save($Out)
"$($files.Count) quadros -> $Out"
