; Inno Setup 6 script - builds VoxForge-Setup-x64.exe
;   1. Build the plug-in (scripts\build_windows.ps1)
;   2. iscc installer\VoxForge.iss
#define AppName "VoxForge"
#define AppVersion "1.0.0"
#define BuildDir "..\build\VoxForge_artefacts\Release"

[Setup]
AppId={{6C0F4D52-8B0E-4D8A-9E57-6A1B3C0F2E11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Bawmer
DefaultDirName={autopf}\VoxForge
DefaultGroupName=VoxForge
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=VoxForge-Setup-x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
WizardStyle=modern
DisableProgramGroupPage=yes

[Types]
Name: "full"; Description: "Plug-in + neural engine sources"
Name: "plugin"; Description: "Plug-in only"

[Components]
Name: "vst3"; Description: "VoxForge VST3 plug-in"; Types: full plugin; Flags: fixed
Name: "standalone"; Description: "Standalone app"; Types: full plugin
Name: "engine"; Description: "VoxForge Engine (neural training + lyrics extraction, needs Python 3.10-3.12)"; Types: full

[Files]
Source: "{#BuildDir}\VST3\VoxForge.vst3\*"; DestDir: "{commoncf64}\VST3\VoxForge.vst3"; Flags: recursesubdirs ignoreversion; Components: vst3
Source: "{#BuildDir}\Standalone\VoxForge.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: standalone
Source: "{#BuildDir}\Standalone\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist; Components: standalone
Source: "..\engine\*"; DestDir: "{app}\engine-src\engine"; Flags: recursesubdirs ignoreversion; Excludes: "__pycache__,*.pyc"; Components: engine
Source: "..\scripts\install_engine.ps1"; DestDir: "{app}\engine-src\scripts"; Flags: ignoreversion; Components: engine
Source: "..\docs\USER_MANUAL.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\VoxForge (Standalone)"; Filename: "{app}\VoxForge.exe"; Components: standalone
Name: "{group}\Install VoxForge Engine"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoExit -File ""{app}\engine-src\scripts\install_engine.ps1"""; Components: engine
Name: "{group}\User Manual"; Filename: "{app}\USER_MANUAL.md"

[Run]
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoExit -File ""{app}\engine-src\scripts\install_engine.ps1"""; \
    Description: "Install the VoxForge Engine now (downloads PyTorch, ~2-3 GB)"; Flags: postinstall unchecked shellexec; Components: engine
