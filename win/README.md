# K LOAD для Windows — пайплайн платформы Windows.

Состав (наполняется этапами):
- fetch-tools-win.ps1 — докачка yt-dlp.exe, ffmpeg/ffprobe, deno
- installer.iss — Inno Setup (per-user, русский)
- build.ps1 — локальная сборка на Windows
- win-build.yml — CI-воркфлоу (лежит в .github/workflows/)
