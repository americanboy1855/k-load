# Локальная сборка K LOAD на Windows.
# Использование:  pwsh win/build.ps1 [-Installer]
#
# Требует: Flutter SDK (stable), Git, vcpkg (VCPKG_ROOT) — для curl.
# Результат: dist/K LOAD — портативная папка; с -Installer ещё и
# dist/K-LOAD-V1.0-Setup.exe (нужен Inno Setup 6 в PATH).

param([switch]$Installer)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

# 1. Инструменты (yt-dlp/ffmpeg/ffprobe/deno).
if (-not (Test-Path "core/tools-win/ytdlp/yt-dlp.exe")) {
    & "$PSScriptRoot/fetch-tools-win.ps1"
}

# 2. Ядро kdcore.dll (vcpkg: curl, TLS = Schannel).
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$env:LOCALAPPDATA/vcpkg" }
& vcpkg install "curl:x64-windows-static-md"
cmake -S kdcore -B kdcore/build-win -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build kdcore/build-win --config Release

# 3. Приложение.
Push-Location app
flutter test
flutter build windows --release
Pop-Location

# 4. Портативная папка.
$out = "dist/K LOAD"
New-Item -ItemType Directory -Force -Path "$out/tools/ytdlp" | Out-Null
Copy-Item "app/build/windows/x64/runner/Release/*" $out -Recurse -Force
Copy-Item "kdcore/build-win/Release/kdcore.dll" $out -Force
foreach ($t in @("ffmpeg.exe", "ffprobe.exe", "deno.exe")) {
    Copy-Item "core/tools-win/$t" "$out/tools/$t" -Force
}
Copy-Item "core/tools-win/ytdlp/yt-dlp.exe" "$out/tools/ytdlp/yt-dlp.exe" -Force
Copy-Item "core/tools-win/LICENSES" "$out/tools/LICENSES" -Recurse -Force

Write-Host "`nГотово: $out"

# 5. Установщик (опционально).
if ($Installer) {
    & iscc "win/installer.iss"
    Write-Host "Готово: dist/K-LOAD-V1.0-Setup.exe"
}
