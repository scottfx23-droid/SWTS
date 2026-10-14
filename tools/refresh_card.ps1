# SFGE card refresher -- copies a device's scenario master onto its SD card.
#
#   .\tools\refresh_card.ps1 -Device PANEL_02 -Drive E:
#   .\tools\refresh_card.ps1 -Device DATAPAD  -Drive E: -Wipe
#   .\tools\refresh_card.ps1 -List
#
# Default is an overwrite-copy: files on the card that the master doesn't
# have (guild speech WAVs, droid_state.json, a live player.json) survive.
# -Wipe deletes the card's /SFGE first for a factory-fresh card.
param(
    [string]$Device,
    [string]$Drive,
    [switch]$Wipe,
    [switch]$List
)

$scenario = "twin_suns_demo"
$map = @{
    "DATAPAD"        = "datapad"
    "GM"             = "gm"
    "ACTOR"          = "actor"
    "PANEL_01"       = "props/PANEL_01"
    "PANEL_02"       = "props/PANEL_02"
    "TERM_01"        = "props/TERM_01"
    "TERM_02"        = "props/TERM_02"
    "DROID_R5D8"     = "props/DROID_R5D8"
    "DROID_C11U17K"  = "props/DROID_C11U17K"
    "GUILD_01"       = "props/GUILD_01"
}

if ($List -or -not $Device -or -not $Drive) {
    Write-Host "Devices:" -ForegroundColor Cyan
    $map.Keys | Sort-Object | ForEach-Object { Write-Host ("  {0,-15} -> scenarios/$scenario/{1}/SFGE" -f $_, $map[$_]) }
    Write-Host "`nUsage: .\tools\refresh_card.ps1 -Device PANEL_02 -Drive E: [-Wipe]"
    exit 0
}

$key = $Device.ToUpper()
if (-not $map.ContainsKey($key)) {
    Write-Host "Unknown device '$Device' -- run with -List to see choices" -ForegroundColor Red
    exit 1
}

$src = Join-Path $PSScriptRoot "..\scenarios\$scenario\$($map[$key])\SFGE"
$src = (Resolve-Path $src).Path
$dst = Join-Path $Drive "SFGE"

if (-not (Test-Path $src)) { Write-Host "Master missing: $src" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Drive)) { Write-Host "Drive $Drive not found -- card inserted?" -ForegroundColor Red; exit 1 }

if ($Wipe -and (Test-Path $dst)) {
    Write-Host "Wiping $dst ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $dst
}

Write-Host "Copying $key master -> $dst"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item -Path (Join-Path $src "*") -Destination $dst -Recurse -Force

$n = (Get-ChildItem -Recurse -File $dst | Measure-Object).Count
Write-Host "Done -- $n files on the card." -ForegroundColor Green
if ($key -eq "GUILD_01") {
    Write-Host "Reminder: drop voice lines into $dst\speech\ (boot/greet/turnin/denied/idle + per-bounty)." -ForegroundColor Cyan
}
if ($key -eq "DATAPAD" -and -not $Wipe) {
    Write-Host "Note: an existing player.json on the card was OVERWRITTEN with the fresh template (player reset)." -ForegroundColor Cyan
}
