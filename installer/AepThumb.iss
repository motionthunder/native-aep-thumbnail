; Setup for Native AEP Thumbnail - the one file end users download.
;
; Build it with make-installer.cmd in the repository root, which compiles the
; binaries first. Needs Inno Setup 6: winget install JRSoftware.InnoSetup
;
; Output: dist\NativeAEPThumbnail-<version>-Setup.exe

#define AppName      "Native AEP Thumbnail"
#define AppVersion   "0.1.0"
#define AppPublisher "motionthunder"
#define AppURL       "https://github.com/motionthunder/native-aep-thumbnail"
#define AppId        "{{9C1F2A63-7E4D-4B8A-9F31-2D5E6A0B7C48}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion} beta
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
AppCopyright=Copyright (c) 2026 {#AppPublisher}. MIT License.

; Fixed location: toggle.cmd, the self-check and the docs all expect it.
DefaultDirName={autopf}\AepThumb
DisableDirPage=yes
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes

OutputDir=..\dist
OutputBaseFilename=NativeAEPThumbnail-{#AppVersion}-Setup
SetupIconFile=assets\app.ico
WizardStyle=modern
; The welcome page is where the large artwork appears; Inno hides it by default.
DisableWelcomePage=no
WizardImageFile=assets\wizard.bmp,assets\wizard-200.bmp
WizardSmallImageFile=assets\wizard-small.bmp,assets\wizard-small-200.bmp
InfoBeforeFile=before.txt
Compression=lzma2/ultra64
SolidCompression=yes

VersionInfoVersion={#AppVersion}.0
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} setup
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoCopyright=Copyright (c) 2026 {#AppPublisher}

; The COM class and the .aep association are machine-wide.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; Our own [Code] stops exactly the processes holding our files. Restart
; Manager would instead offer to close Explorer, which only alarms people.
CloseApplications=no

; English only: the tool is distributed to an international audience.
ShowLanguageDialog=no
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\aepbake.exe

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedLabel=Setup has finished installing [name].%n%nOpen any folder with .aep files. Previews appear within a few seconds and are instant from then on.%n%nTo switch previews off or back on, use "Turn previews on or off" in the Start menu.

[CustomMessages]
GroupAE=After Effects:
GroupExplorer=Explorer:
TaskScripting=Allow After Effects to render previews (turns on "Allow Scripts to Write Files and Access Network" - required)
TaskRefresh=Refresh existing thumbnails now (Explorer restarts for a moment)
ShortcutToggle=Turn previews on or off
ShortcutCheck=Check setup
ShortcutSite=Project page
NoAE=After Effects was not found on this computer.%n%nNative AEP Thumbnail needs After Effects to render previews. Without it, .aep files will only show a placeholder card.%n%nInstall anyway?
CloseAE=After Effects is open.%n%nIt saves its settings when it closes, which would undo the change. Please close After Effects, then click Retry.%n%nCancel continues without changing the setting - you can turn it on later.
ScriptingNoPrefs=After Effects has not been started on this computer yet, so its setting could not be switched on.%n%nStart After Effects once, close it, and run "Check setup" from the Start menu.
ScriptingFailed=The After Effects setting could not be changed automatically.%n%nTurn it on by hand in After Effects:%nEdit > Preferences > Scripting & Expressions >%n"Allow Scripts to Write Files and Access Network"
ScriptingSkipped=The After Effects setting was left unchanged. Previews cannot render until "Allow Scripts to Write Files and Access Network" is on - run "Check setup" from the Start menu at any time.
RemoveCache=Also delete the previews already rendered?%n%nThey are only a cache and take a little disk space.

[Tasks]
Name: "scripting"; Description: "{cm:TaskScripting}"; GroupDescription: "{cm:GroupAE}"; Check: AfterEffectsInstalled
Name: "refresh";   Description: "{cm:TaskRefresh}";   GroupDescription: "{cm:GroupExplorer}"; Flags: unchecked

[Files]
; regserver calls DllRegisterServer on install and DllUnregisterServer on
; uninstall; that is what claims .aep and later hands it back to whatever
; handled it before.
Source: "..\build\aepthumb.dll";   DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "..\build\aepbake.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bake_batch.jsx"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\toggle.cmd";           DestDir: "{app}"; Flags: ignoreversion
Source: "selfcheck.cmd";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";              DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\{cm:ShortcutToggle}"; Filename: "{app}\toggle.cmd";    IconFilename: "{app}\aepbake.exe"
Name: "{group}\{cm:ShortcutCheck}";  Filename: "{app}\selfcheck.cmd"; IconFilename: "{app}\aepbake.exe"
Name: "{group}\{cm:ShortcutSite}";   Filename: "{#AppURL}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"

[UninstallDelete]
; The shell host can release the DLL a moment after uninstall removes the files,
; which otherwise leaves an empty program folder behind.
Type: dirifempty; Name: "{app}"

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im aepbake.exe"; Flags: runhidden; RunOnceId: "StopBaker"

[Code]

var
  SkipScripting: Boolean;

function AfterEffectsInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM64, 'SOFTWARE\Adobe\After Effects');
end;

function AfterEffectsRunning(): Boolean;
var
  ResultCode: Integer;
begin
  // find exits 0 only when tasklist printed a matching process.
  Result := Exec(ExpandConstant('{cmd}'),
                 '/c tasklist /fi "imagename eq AfterFX.exe" /nh | find /i "AfterFX.exe" >nul',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Stops only what holds our files: the baker, and the shell's thumbnail host
// processes that actually have aepthumb.dll loaded. Windows restarts those
// hosts on demand; unrelated dllhost.exe instances are left alone.
procedure ReleaseOpenFiles();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im aepbake.exe', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'),
       '/f /fi "imagename eq dllhost.exe" /fi "modules eq aepthumb.dll"', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  SkipScripting := False;
  if not AfterEffectsInstalled() then
    Result := SuppressibleMsgBox(CustomMessage('NoAE'), mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  // After Effects writes its preferences on exit, so changing them while it is
  // open would be silently undone. Ask for it to be closed first.
  if (CurPageID = wpReady) and WizardIsTaskSelected('scripting') then
  begin
    while AfterEffectsRunning() do
    begin
      if SuppressibleMsgBox(CustomMessage('CloseAE'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
      begin
        SkipScripting := True;
        Break;
      end;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ReleaseOpenFiles();
  Result := '';
end;

procedure EnableScripting();
var
  ResultCode: Integer;
begin
  // Runs as the signed-in user, not the elevated installer, so it edits that
  // person's After Effects preferences under their own AppData.
  if not ExecAsOriginalUser(ExpandConstant('{app}\aepbake.exe'), '--enable-scripting', '',
                            SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;

  case ResultCode of
    0: ;
    2: SuppressibleMsgBox(CustomMessage('ScriptingNoPrefs'), mbInformation, MB_OK, IDOK);
  else
    SuppressibleMsgBox(CustomMessage('ScriptingFailed'), mbError, MB_OK, IDOK);
  end;
end;

// Windows keeps drawn tiles in its own cache, so without this, folders that
// were already browsed would keep their old icons until it expires.
procedure RefreshThumbnails();
var
  ResultCode: Integer;
begin
  ExecAsOriginalUser(ExpandConstant('{sys}\taskkill.exe'), '/f /im explorer.exe', '',
                     SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(1500);
  ExecAsOriginalUser(ExpandConstant('{cmd}'),
                     '/c del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db"',
                     '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Started as the original user: launched from the elevated installer, the
  // desktop shell itself would run elevated.
  ExecAsOriginalUser(ExpandConstant('{win}\explorer.exe'), '', '',
                     SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('scripting') then
    begin
      if SkipScripting then
        SuppressibleMsgBox(CustomMessage('ScriptingSkipped'), mbInformation, MB_OK, IDOK)
      else
        EnableScripting();
    end;
    if WizardIsTaskSelected('refresh') then
      RefreshThumbnails();
  end;
end;

function InitializeUninstall(): Boolean;
begin
  ReleaseOpenFiles();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    if SuppressibleMsgBox(CustomMessage('RemoveCache'), mbConfirmation, MB_YESNO, IDNO) = IDYES then
      DelTree(ExpandConstant('{localappdata}\AepThumb'), True, True, True);
  end;
end;
