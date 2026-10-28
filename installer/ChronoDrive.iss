; ChronoDrive installer (Inno Setup 6). The version below is the ONLY place it lives: CMake reads it.
#define AppName "ChronoDrive"
#define AppVersion "0.1.0"
#define Root ".."
#ifndef BUILDDIR
#define BUILDDIR "build\Release"
#endif

[Setup]
AppId={{3C1D2E7B-9A4F-4C6E-8D21-5F0B7A9E1C42}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ChronoScan
DefaultDirName={autopf}\ChronoScan\ChronoDrive
DefaultGroupName=ChronoScan
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=ChronoDrive_Setup_{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
UninstallDisplayIcon={app}\ChronoDrive.exe

[Tasks]
Name: "startup"; Description: "Start ChronoDrive with Windows (tray icon)"; GroupDescription: "Startup:"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ChronoDrive"; ValueData: """{app}\ChronoDrive.exe"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Files]
Source: "{#Root}\{#BUILDDIR}\ChronoDrive.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\{#BUILDDIR}\cdrive.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ChronoDrive"; Filename: "{app}\ChronoDrive.exe"

[Run]
Filename: "{app}\ChronoDrive.exe"; Description: "Open ChronoDrive"; Flags: postinstall nowait skipifsilent
