; Inno Setup script for AepThumb.
;
; Build with Inno Setup 6 (free, https://jrsoftware.org/isdl.php):
;     "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\AepThumb.iss
; Output lands in installer\Output\AepThumb-Setup.exe - that single file is
; what colleagues run.
;
; Run build.cmd first: the script packages what is in build\.

#define AppName      "AepThumb"
#define AppVersion   "0.1.0"
#define AppPublisher "Motion Thunder"
#define AppId        "{{9C1F2A63-7E4D-4B8A-9F31-2D5E6A0B7C48}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
OutputDir=Output
OutputBaseFilename={#AppName}-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=
InfoBeforeFile=before.txt

; The COM class is registered in HKLM, and the .aep association is made for
; every user on the machine, so setup needs administrator rights.
PrivilegesRequired=admin

; x64 only: the DLL is loaded by the 64-bit shell.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

UninstallDisplayName={#AppName} - After Effects project thumbnails
UninstallDisplayIcon={app}\aepbake.exe

[Files]
; regserver makes setup call DllRegisterServer, and DllUnregisterServer on
; uninstall - that is what claims .aep and puts the previous handler back.
; restartreplace covers the case where the shell still holds the DLL open.
Source: "..\build\aepthumb.dll";   DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "..\build\aepbake.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bake_batch.jsx"; DestDir: "{app}"; Flags: ignoreversion
Source: "selfcheck.cmd";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";            DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName} self-check"; Filename: "{app}\selfcheck.cmd"; IconFilename: "{app}\aepbake.exe"
Name: "{group}\Read me";               Filename: "{app}\README.md"
Name: "{group}\Uninstall {#AppName}";  Filename: "{uninstallexe}"

[Run]
Filename: "{app}\selfcheck.cmd"; Description: "Check that After Effects is set up correctly"; Flags: postinstall nowait skipifsilent unchecked shellexec

[UninstallRun]
; Stop the baker so its exe is not held open while files are removed.
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im aepbake.exe"; Flags: runhidden skipifdoesntexist; RunOnceId: "StopBaker"

[Code]

// Both the baker and the shell can hold files open. Close the baker, and let
// go of the DLL by ending the thumbnail host, which Windows restarts on
// demand. Without this an upgrade over a running install fails to copy.
procedure ReleaseOpenFiles();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im aepbake.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im dllhost.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ReleaseOpenFiles();
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  ReleaseOpenFiles();
  Result := True;
end;

// Point out the two things that stop previews appearing, while setup is still
// on screen rather than leaving people to wonder later.
function InitializeSetup(): Boolean;
var
  AeKey: String;
begin
  Result := True;
  AeKey := 'SOFTWARE\Adobe\After Effects';
  if not RegKeyExists(HKEY_LOCAL_MACHINE, AeKey) then
  begin
    if MsgBox('After Effects was not found on this machine.' + #13#10#13#10 +
              'AepThumb can still be installed, but previews cannot be rendered ' +
              'without After Effects - tiles will show a placeholder card instead ' +
              'of the real frame.' + #13#10#13#10 +
              'Install anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;
