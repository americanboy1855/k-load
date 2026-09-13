# Докачка инструментов для K LOAD (Windows): yt-dlp, ffmpeg/ffprobe, deno.
# Использование:  pwsh win/fetch-tools-win.ps1
# Скачивает в core/tools-win/ — ту же раскладку, что и мак (core/tools).

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot   # корень репо
$Out  = Join-Path $Root "core/tools-win"
New-Item -ItemType Directory -Force -Path "$Out/ytdlp" | Out-Null
$tmp = Join-Path $env:TEMP "kload-tools"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Fetch($url, $dest) {
    $name = Split-Path -Path $dest -Leaf
    Write-Host ">> $name"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

# yt-dlp (один exe)
Fetch "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" `
      "$Out/ytdlp/yt-dlp.exe"

# deno (zip с deno.exe) — JS-челленджи YouTube через yt-dlp
$denoZip = "$tmp/deno.zip"
Fetch "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip" $denoZip
Expand-Archive -Path $denoZip -DestinationPath $tmp/deno -Force
Copy-Item "$tmp/deno/deno.exe" "$Out/deno.exe" -Force

# ffmpeg/ffprobe (BtbN, win64 lgpl: bin/ffmpeg.exe + bin/ffprobe.exe)
$ffZip = "$tmp/ffmpeg.zip"
Fetch "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-win64-lgpl.zip" $ffZip
Expand-Archive -Path $ffZip -DestinationPath $tmp/ffmpeg -Force
$bin = Get-ChildItem "$tmp/ffmpeg" -Recurse -Filter "ffmpeg.exe" | Select-Object -First 1
Copy-Item $bin.FullName "$Out/ffmpeg.exe" -Force
$probe = Get-ChildItem "$tmp/ffmpeg" -Recurse -Filter "ffprobe.exe" | Select-Object -First 1
Copy-Item $probe.FullName "$Out/ffprobe.exe" -Force

# Лицензии инструментов — в пакет.
$lic = Join-Path $Root "mac/pkg/LICENSES"
if (Test-Path $lic) {
    New-Item -ItemType Directory -Force -Path "$Out/LICENSES" | Out-Null
    Copy-Item "$lic/*" "$Out/LICENSES" -Force
}

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "`nГотово: $Out"
Get-ChildItem $Out -Recurse -File | ForEach-Object { "  $($_.FullName.Substring($Out.Length + 1))  $([math]::Round($_.Length/1MB, 1)) МБ" }
