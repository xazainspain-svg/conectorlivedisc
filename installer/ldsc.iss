; Inno Setup script: installs the background app + the VST3 plugin in one go.
; Expects (relative to this file): ..\dist\ldsc-tray.exe, ..\dist\ldsc-device.exe, ..\dist\vst3\*
[Setup]
AppName=Live Discord Send
AppVersion=0.2.0
AppPublisher=conectorlivedisc
DefaultDirName={autopf}\LiveDiscordSend
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=LiveDiscordSend-Setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=Live Discord Send
CloseApplications=yes

[Files]
Source: "..\dist\ldsc-tray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\ldsc-device.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\vst3\*"; DestDir: "{commoncf64}\VST3"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
; Registers autostart for the real (non-elevated) user and starts the tray app.
Filename: "{app}\ldsc-tray.exe"; Parameters: "--install-autostart"; Flags: nowait runasoriginaluser

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/f /im ldsc-tray.exe"; Flags: runhidden; RunOnceId: "KillTray"
Filename: "{app}\ldsc-tray.exe"; Parameters: "--uninstall"; Flags: runhidden; RunOnceId: "RemoveAutostart"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not DirExists(ExpandConstant('{commonpf64}\VB\CABLE')) and not DirExists(ExpandConstant('{commonpf}\VB\CABLE')) then
    MsgBox('No detecto VB-CABLE. Se instalará igualmente, pero necesitas instalar el cable virtual (vb-audio.com/Cable) para que Discord vea el micrófono.', mbInformation, MB_OK);
end;
