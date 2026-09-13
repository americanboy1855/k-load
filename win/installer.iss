; K LOAD V1.0 — установщик Windows (Inno Setup).
; Сборка: iscc win/installer.iss   (из корня репо; нужен готовый
; портативный состав в dist/K LOAD — его собирает win/build.ps1 или CI).
; Per-user установка без прав администратора, русский интерфейс.

#define AppName "K LOAD"
#define AppVersion "1.0.0"
#define AppPublisher "kvartal records"
#define AppExe "K LOAD.exe"

[Setup]
AppId={{5ECD3C05-B2FE-405B-AA0B-B153195A4822}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\K LOAD
PrivilegesRequired=lowest
DisableDirPage=no
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=K-LOAD-V1.0-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#AppExe}
CloseApplications=yes
RestartApplications=no
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Портативный состав: K LOAD.exe, dll, data\, tools\ (всё уже уложено).
Source: "..\dist\K LOAD\*"; DestDir: "{app}"; \
    Flags: recursesubdirs ignoreversion createallsubdirs

[Icons]
Name: "{autoprograms}\K LOAD"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\K LOAD"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent
