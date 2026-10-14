# SFGE DFPlayer card builder -- turns a folder of NAMED voice MP3s into the
# numbered \mp3\NNNN.mp3 layout the DFPlayer needs, using the Guild config's
# speech_tracks map as the name->number database.
#
#   .\tools\make_dfplayer_card.ps1 -Mp3Folder C:\vex_lines -Drive G:
#
# File names must match speech_tracks keys (case-insensitive), e.g.
#   boot.mp3, greet.mp3, turnin.mp3, denied.mp3,
#   bounty_shilak.mp3 (= collected/turn-in), bounty_shilak_posted.mp3,
#   idle1.mp3 ... idle9.mp3
# Unmapped files are listed so the map can be extended.
param(
    [Parameter(Mandatory)][string]$Mp3Folder,
    [Parameter(Mandatory)][string]$Drive
)

$cfgPath = Join-Path $PSScriptRoot "..\scenarios\twin_suns_demo\props\GUILD_01\SFGE\config.json"
if (-not (Test-Path $cfgPath))   { Write-Host "Guild config not found: $cfgPath" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Mp3Folder)) { Write-Host "MP3 folder not found: $Mp3Folder" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Drive))     { Write-Host "Drive $Drive not found - card inserted?" -ForegroundColor Red; exit 1 }

$cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json
$tracks = $cfg.behavior.speech_tracks
if (-not $tracks) { Write-Host "No behavior.speech_tracks in the guild config" -ForegroundColor Red; exit 1 }

# name -> number lookup (case-insensitive)
$map = @{}
$tracks.PSObject.Properties | ForEach-Object { $map[$_.Name.ToLower()] = [int]$_.Value }

$dst = Join-Path $Drive "mp3"
New-Item -ItemType Directory -Force $dst | Out-Null

$copied = 0; $unmapped = @()
Get-ChildItem -Path $Mp3Folder -Filter *.mp3 -File | ForEach-Object {
    $name = [IO.Path]::GetFileNameWithoutExtension($_.Name).ToLower()
    # Convention: <bounty_id>_collected.mp3 is the turn-in clip for that
    # bounty, which the firmware plays by the bare id
    if ($name.EndsWith("_collected")) { $name = $name.Substring(0, $name.Length - 10) }
    if ($map.ContainsKey($name)) {
        $n = $map[$name]
        $target = Join-Path $dst ("{0:0000}.mp3" -f $n)
        Copy-Item $_.FullName $target -Force
        Write-Host ("  {0,-28} -> mp3\{1:0000}.mp3" -f $_.Name, $n) -ForegroundColor Green
        $copied++
    } else {
        $unmapped += $_.Name
    }
}

Write-Host ""
Write-Host "$copied file(s) written to $dst" -ForegroundColor Cyan
$missing = $map.Keys | Where-Object {
    $k = $_
    -not (Get-ChildItem -Path $Mp3Folder -Filter *.mp3 -File |
          Where-Object { [IO.Path]::GetFileNameWithoutExtension($_.Name).ToLower() -eq $k })
}
if ($missing)  { Write-Host ("Mapped but no file yet: " + ($missing -join ", ")) -ForegroundColor Yellow }
if ($unmapped) { Write-Host ("NOT in speech_tracks (add to the guild config first): " + ($unmapped -join ", ")) -ForegroundColor Red }
