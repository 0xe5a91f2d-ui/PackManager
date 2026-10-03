#define AppName "PackManager"
#define AppVersion "0.1.0"
#define AppPublisher "PackManager"

[Setup]
AppId={{A46B41A4-24C6-4196-9A24-69E6C9B75257}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\PackManager
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=PackManager-Setup-{#AppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayName={#AppName}

[Files]
Source: "..\dist\*"; DestDir: "{app}"; Excludes: "PackManager-Setup-*.exe"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\PackManager"; Filename: "{app}\PackManager.exe"
Name: "{autodesktop}\PackManager"; Filename: "{app}\PackManager.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"
