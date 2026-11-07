; Build with tools/build-installer.py. Payload includes come from a verified package receipt.
#include "release.iss"
#ifndef SimulatorProcessName
  #define SimulatorProcessName "FlightSimulator2024.exe"
#endif
#ifndef XPlaneProcessName
  #define XPlaneProcessName "X-Plane.exe"
#endif

[Setup]
AppId={{8C662A8B-F01B-43BE-9104-2D63CA78C921}
AppName=FS Camera Studio (Tester)
AppVersion={#ReleaseVersion}
AppVerName=FS Camera Studio {#ReleaseVersion}
AppPublisher=Devinci
VersionInfoVersion={#NumericVersion}
VersionInfoDescription=FS Camera Studio tester setup for MSFS 2024 and X-Plane 12
DefaultDirName={localappdata}\FS Camera Studio\Unselected
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
MinVersion=10.0.22000
UninstallFilesDir={localappdata}\FS Camera Studio\Installer
UninstallDisplayName=FS Camera Studio (Tester)
OutputDir={#ReleaseOutput}
OutputBaseFilename=FS-Camera-Studio-{#ReleaseVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
SetupLogging=yes
CloseApplications=no
RestartApplications=no
AllowNoIcons=yes
InfoBeforeFile={#StageRoot}\TESTER-GUIDE.txt
InfoAfterFile={#StageRoot}\AFTER-INSTALL.txt
Uninstallable=yes

; One installer for both simulators: choose MSFS 2024, X-Plane 12 or both. Each gets its own folder page.
[Types]
Name: "custom"; Description: "Choose simulators"; Flags: iscustom

[Components]
Name: "msfs"; Description: "Microsoft Flight Simulator 2024 (package in the Community folder)"; Types: custom
Name: "xplane"; Description: "X-Plane 12 (XP Camera Studio plugin, X-Plane 12.4.4 or newer)"

[InstallDelete]
; The previous plugin folder was backed up and verified in PrepareToInstall; a clean folder leaves no stale files.
; Profiles live in Output\preferences\XPCameraStudio, outside it.
Type: filesandordirs; Name: "{code:XPlaneDir}"; Components: xplane

[Files]
#include "payload.iss"
Source: "{#StageRoot}\TESTER-GUIDE.txt"; DestDir: "{localappdata}\FS Camera Studio"; Flags: ignoreversion
Source: "{#StageRoot}\THIRD-PARTY-NOTICES.txt"; DestDir: "{localappdata}\FS Camera Studio"; Flags: ignoreversion
Source: "{#StageRoot}\BUILD-INFO.json"; DestDir: "{localappdata}\FS Camera Studio"; Flags: ignoreversion
Source: "{#StageRoot}\FS-Camera-Studio-Manual.pdf"; DestDir: "{localappdata}\FS Camera Studio"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\FS Camera Studio\Tester guide"; Filename: "{localappdata}\FS Camera Studio\TESTER-GUIDE.txt"
Name: "{userprograms}\FS Camera Studio\User manual"; Filename: "{localappdata}\FS Camera Studio\FS-Camera-Studio-Manual.pdf"
Name: "{userprograms}\FS Camera Studio\Uninstall"; Filename: "{uninstallexe}"

[Code]
var
  CommunityPage, XPlanePage: TInputDirWizardPage;
  PreviousCommunity, PreviousXPlane, BackupDir, BackedUpTarget, BackedUpXPlane: String;

function GetFileAttributesW(Name: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function IsLink(const Path: String): Boolean;
var Attributes: LongWord;
begin
  Attributes := GetFileAttributesW(Path);
  Result := (Attributes <> $FFFFFFFF) and ((Attributes and $400) <> 0);
end;

function PlainTree(const Path: String): Boolean;
var Item: TFindRec;
begin
  Result := not IsLink(Path);
  if not Result then exit;
  if FindFirst(AddBackslash(Path) + '*', Item) then begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') then begin
          if (Item.Attributes and $400) <> 0 then Result := False
          else if (Item.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := PlainTree(AddBackslash(Path) + Item.Name);
          if not Result then exit;
        end;
      until not FindNext(Item);
    finally FindClose(Item); end;
  end;
end;

function ProcessStopped(const Name: String): Boolean;
var Locator, Services, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + Name + '''');
    Result := Processes.Count = 0;
  except Log('Could not verify whether ' + Name + ' is running: ' + GetExceptionMessage); end;
end;

function SimulatorStopped: Boolean;
begin
  Result := ProcessStopped('{#SimulatorProcessName}');
end;

function XPlaneStopped: Boolean;
begin
  Result := ProcessStopped('{#XPlaneProcessName}');
end;

function MsfsSelected: Boolean;
begin
  Result := WizardIsComponentSelected('msfs');
end;

function XPlaneSelected: Boolean;
begin
  Result := WizardIsComponentSelected('xplane');
end;

{ X-Plane 12's folder: the one with X-Plane.exe and Resources\plugins. }
function IsXPlaneRoot(const Path: String): Boolean;
begin
  Result := (Path <> '') and FileExists(AddBackslash(Path)+'X-Plane.exe') and DirExists(AddBackslash(Path)+'Resources\plugins');
end;

{ X-Plane lists every X-Plane 12 folder it has run from in x-plane_install_12.txt, one per line. }
function XPlaneFromInstallList: String;
var Lines: TArrayOfString; I: Integer; Line: String;
begin
  Result := '';
  if not LoadStringsFromFile(ExpandConstant('{localappdata}\x-plane_install_12.txt'), Lines) then exit;
  for I := 0 to GetArrayLength(Lines)-1 do begin
    Line := Trim(Lines[I]);
    StringChangeEx(Line, '/', '\', True);
    Line := RemoveBackslashUnlessRoot(Line);
    if IsXPlaneRoot(Line) then begin Result := Line; exit; end;
  end;
end;

{ Steam installs: steamapps\common\X-Plane 12 in the default library or in one listed in libraryfolders.vdf. }
function XPlaneFromSteam: String;
var Lines: TArrayOfString; I, Q: Integer; Steam, Line, Library: String;
begin
  Steam := ExpandConstant('{commonpf32}\Steam');
  Result := AddBackslash(Steam)+'steamapps\common\X-Plane 12';
  if IsXPlaneRoot(Result) then exit;
  Result := '';
  if not LoadStringsFromFile(AddBackslash(Steam)+'steamapps\libraryfolders.vdf', Lines) then exit;
  for I := 0 to GetArrayLength(Lines)-1 do begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) <> 1 then continue;
    Line := Trim(Copy(Line, 7, Length(Line)));
    if (Length(Line) < 2) or (Line[1] <> '"') then continue;
    Line := Copy(Line, 2, Length(Line));
    Q := Pos('"', Line);
    if Q = 0 then continue;
    Library := Copy(Line, 1, Q-1);
    StringChangeEx(Library, '\\', '\', True);
    if IsXPlaneRoot(AddBackslash(Library)+'steamapps\common\X-Plane 12') then begin
      Result := AddBackslash(Library)+'steamapps\common\X-Plane 12'; exit;
    end;
  end;
end;

function DetectXPlane: String;
begin
  Result := ExpandConstant('{param:XPLANE|}');
  if Result <> '' then exit;
  if IsXPlaneRoot(PreviousXPlane) then begin Result := PreviousXPlane; exit; end;
  Result := XPlaneFromInstallList;
  if Result = '' then Result := XPlaneFromSteam;
end;

function XPlaneRootValue: String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(XPlanePage.Values[0]));
end;

{ The plugin folder: X-Plane 12\Resources\plugins\XPCameraStudio. }
function XPlaneDir(Param: String): String;
begin
  Result := AddBackslash(XPlaneRootValue)+'Resources\plugins\XPCameraStudio';
end;

function CommunityFromConfig(const Filename: String): String;
var Lines: TArrayOfString; I, Quote: Integer; Line, Packages: String;
begin
  Result := '';
  if not LoadStringsFromFile(Filename, Lines) then exit;
  for I := 0 to GetArrayLength(Lines)-1 do begin
    Line := Trim(Lines[I]);
    if Pos('InstalledPackagesPath ', Line) = 1 then begin
      Quote := Pos('"', Line);
      if Quote = 0 then continue;
      Packages := Copy(Line, Quote+1, Length(Line));
      Quote := Pos('"', Packages);
      if Quote = 0 then continue;
      Packages := Copy(Packages, 1, Quote-1);
      { Preserve an existing package's location; otherwise prefer the 2024-only folder if present. }
      if DirExists(AddBackslash(Packages)+'Community\fs-camera-studio') then
        Result := AddBackslash(Packages)+'Community'
      else if DirExists(AddBackslash(Packages)+'Community2024') then
        Result := AddBackslash(Packages)+'Community2024'
      else if DirExists(AddBackslash(Packages)+'Community') then
        Result := AddBackslash(Packages)+'Community';
      exit;
    end;
  end;
end;

function DetectCommunity: String;
begin
  Result := ExpandConstant('{param:COMMUNITY|}');
  if Result <> '' then exit;
  if DirExists(PreviousCommunity) then begin Result := PreviousCommunity; exit; end;
  Result := CommunityFromConfig(ExpandConstant('{userappdata}\Microsoft Flight Simulator 2024\UserCfg.opt'));
  if Result = '' then
    Result := CommunityFromConfig(ExpandConstant('{localappdata}\Packages\Microsoft.Limitless_8wekyb3d8bbwe\LocalCache\UserCfg.opt'));
end;

function ValidateTarget: String;
var Community, Target, Other: String;
begin
  Result := '';
  Community := RemoveBackslashUnlessRoot(Trim(CommunityPage.Values[0]));
  if (Community = '') or not DirExists(Community) then begin
    Result := 'Select your existing MSFS 2024 Community folder. Launch the simulator once if it has not created one yet.'; exit;
  end;
  if (CompareText(ExtractFileName(Community), 'Community') <> 0) and
     (CompareText(ExtractFileName(Community), 'Community2024') <> 0) then begin
    Result := 'Select the folder named Community or Community2024, not the package folder inside it.'; exit;
  end;
  Community := RemoveBackslashUnlessRoot(ExpandFileName(Community));
  Target := AddBackslash(Community)+'fs-camera-studio';
  if (PreviousCommunity <> '') and (CompareText(Community, PreviousCommunity) <> 0) and
     DirExists(AddBackslash(PreviousCommunity)+'fs-camera-studio') then begin
    Result := 'An installer-managed copy exists in '+PreviousCommunity+'. Upgrade there, or uninstall that copy before choosing another folder.'; exit;
  end;
  if CompareText(ExtractFileName(Community), 'Community') = 0 then
    Other := AddBackslash(ExtractFileDir(Community))+'Community2024\fs-camera-studio'
  else Other := AddBackslash(ExtractFileDir(Community))+'Community\fs-camera-studio';
  if DirExists(Other) or IsLink(Other) then begin
    Result := 'Another FS Camera Studio copy exists at '+Other+'. Remove or disable that duplicate before installing here.'; exit;
  end;
  if not PlainTree(Target) then begin
    Result := 'This package contains a junction or symbolic link. Disable its Addons Linker/development link first. Setup will not modify linked files.'; exit;
  end;
  if DirExists(Target) and
     (not FileExists(Target+'\modules\FSCameraStudio.wasm') or not FileExists(Target+'\manifest.json')) then begin
    Result := 'The existing fs-camera-studio folder is not a recognized package. Move it aside before installing.'; exit;
  end;
  CommunityPage.Values[0] := Community;
  WizardForm.DirEdit.Text := Target;
end;

function ValidateXPlane: String;
var Root, Target: String;
begin
  Result := '';
  Root := XPlaneRootValue;
  if not IsXPlaneRoot(Root) then begin
    Result := 'Select your X-Plane 12 folder: the one that contains X-Plane.exe and the Resources folder.'; exit;
  end;
  Root := RemoveBackslashUnlessRoot(ExpandFileName(Root));
  XPlanePage.Values[0] := Root;
  Target := XPlaneDir('');
  if (PreviousXPlane <> '') and (CompareText(Root, PreviousXPlane) <> 0) and
     DirExists(AddBackslash(PreviousXPlane)+'Resources\plugins\XPCameraStudio') then begin
    Result := 'An installer-managed XP Camera Studio exists in '+PreviousXPlane+'. Upgrade there, or uninstall that copy before choosing another X-Plane folder.'; exit;
  end;
  if IsLink(AddBackslash(Root)+'Resources\plugins') or not PlainTree(Target) then begin
    Result := 'The X-Plane plugins folder or XPCameraStudio is a junction or symbolic link, or contains one. Setup will not modify linked files.'; exit;
  end;
  if DirExists(Target) and not FileExists(Target+'\win_x64\XPCameraStudio.xpl') then begin
    Result := 'The existing Resources\plugins\XPCameraStudio folder is not a recognized XP Camera Studio plugin. Move it aside before installing.'; exit;
  end;
end;

{ First run, or an upgrade from an MSFS-only setup: select the simulators found on this PC. A silent setup installs
  for X-Plane only when /XPLANE= or /COMPONENTS= asks for it, never just because X-Plane was found. }
procedure ChooseDefaultComponents;
var Sims: String; HasMsfs, HasXPlane: Boolean;
begin
  if ExpandConstant('{param:COMPONENTS|}') <> '' then exit;  { the command line chose }
  Sims := GetPreviousData('Simulators', '');
  if Sims <> '' then begin
    if Pos('msfs', Sims) > 0 then WizardSelectComponents('msfs') else WizardSelectComponents('!msfs');
    if Pos('xplane', Sims) > 0 then WizardSelectComponents('xplane') else WizardSelectComponents('!xplane');
    exit;
  end;
  if WizardSilent then begin
    if ExpandConstant('{param:XPLANE|}') <> '' then WizardSelectComponents('xplane');
    exit;
  end;
  HasMsfs := (PreviousCommunity <> '') or (CommunityPage.Values[0] <> '');
  HasXPlane := XPlanePage.Values[0] <> '';
  if HasXPlane then WizardSelectComponents('xplane');
  if HasXPlane and not HasMsfs then WizardSelectComponents('!msfs');
end;

procedure InitializeWizard;
begin
  PreviousCommunity := GetPreviousData('Community', '');
  PreviousXPlane := GetPreviousData('XPlane', '');
  CommunityPage := CreateInputDirPage(wpSelectComponents, 'Choose your MSFS 2024 Community folder',
    'Steam, Microsoft Store and custom package locations are supported.',
    'Confirm the detected folder or browse to the Community folder shown by MSFS. Setup creates fs-camera-studio inside it. Close MSFS before continuing.', False, '');
  CommunityPage.Add('Community folder:');
  CommunityPage.Values[0] := DetectCommunity;
  XPlanePage := CreateInputDirPage(CommunityPage.ID, 'Choose your X-Plane 12 folder',
    'Steam and standalone X-Plane 12 installs are supported. X-Plane 12.4.4 or newer is required.',
    'Confirm the detected folder or browse to the folder that contains X-Plane.exe. Setup puts XP Camera Studio in Resources\plugins\XPCameraStudio. Close X-Plane before continuing.', False, '');
  XPlanePage.Add('X-Plane 12 folder:');
  XPlanePage.Values[0] := DetectXPlane;
  ChooseDefaultComponents;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = CommunityPage.ID) and not MsfsSelected) or ((PageID = XPlanePage.ID) and not XPlaneSelected);
end;

{ Without MSFS, the app folder is setup's own folder (guide, manual, build info): nothing goes to a Community folder. }
procedure UseControlFolderWithoutMsfs;
begin
  if not MsfsSelected then WizardForm.DirEdit.Text := ExpandConstant('{localappdata}\FS Camera Studio');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Error := '';
  if CurPageID = wpSelectComponents then begin
    if not MsfsSelected and not XPlaneSelected then Error := 'Choose at least one simulator.';
    UseControlFolderWithoutMsfs;
  end
  else if CurPageID = CommunityPage.ID then Error := ValidateTarget
  else if CurPageID = XPlanePage.ID then Error := ValidateXPlane;
  Result := Error = '';
  if not Result then SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
end;

procedure CopyBackup(const Source, Dest: String);
var Item: TFindRec; FromFile, ToFile: String;
begin
  if not ForceDirectories(Dest) then RaiseException('Cannot create backup folder: '+Dest);
  if FindFirst(AddBackslash(Source)+'*', Item) then begin
    try
      repeat
        if (Item.Name <> '.') and (Item.Name <> '..') then begin
          FromFile := AddBackslash(Source)+Item.Name; ToFile := AddBackslash(Dest)+Item.Name;
          if (Item.Attributes and $400) <> 0 then RaiseException('Linked file found during backup.');
          if (Item.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then CopyBackup(FromFile, ToFile)
          else begin
            if not FileCopy(FromFile, ToFile, True) then RaiseException('Backup failed: '+FromFile);
            if GetSHA256OfFile(FromFile) <> GetSHA256OfFile(ToFile) then RaiseException('Backup verification failed: '+FromFile);
          end;
        end;
      until not FindNext(Item);
    finally FindClose(Item); end;
  end;
end;

procedure NewBackupDir;
var Base: String; Suffix: Integer;
begin
  if BackupDir <> '' then exit;
  Base := ExpandConstant('{localappdata}\FS Camera Studio\Backups\')+GetDateTimeString('yyyymmdd-hhnnss','-',':');
  BackupDir := Base; Suffix := 0;
  while DirExists(BackupDir) do begin Suffix := Suffix+1; BackupDir := Base+'-'+IntToStr(Suffix); end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var Target: String;
begin
  Result := '';
  if not MsfsSelected and not XPlaneSelected then begin Result := 'Choose at least one simulator.'; exit; end;
  UseControlFolderWithoutMsfs;
  if MsfsSelected then begin
    Result := ValidateTarget;
    if Result <> '' then exit;
    if not SimulatorStopped then begin
      Result := 'Close Microsoft Flight Simulator 2024 before installing. Setup could not confirm that the simulator is stopped.'; exit;
    end;
  end;
  if XPlaneSelected then begin
    Result := ValidateXPlane;
    if Result <> '' then exit;
    if not XPlaneStopped then begin
      Result := 'Close X-Plane before installing. Setup could not confirm that X-Plane is stopped.'; exit;
    end;
  end;
  Target := WizardDirValue;
  if MsfsSelected and DirExists(Target) and (BackedUpTarget <> Target) then begin
    try
      NewBackupDir;
      CopyBackup(Target, BackupDir+'\fs-camera-studio');
      BackedUpTarget := Target;
      Log('Verified rollback package: '+BackupDir);
    except Result := 'The previous package could not be backed up. Nothing has been replaced. '+GetExceptionMessage; exit; end;
  end;
  Target := XPlaneDir('');
  if XPlaneSelected and DirExists(Target) and (BackedUpXPlane <> Target) then begin
    try
      NewBackupDir;
      CopyBackup(Target, BackupDir+'\XPCameraStudio');
      BackedUpXPlane := Target;
      Log('Verified rollback plugin: '+BackupDir);
    except Result := 'The previous X-Plane plugin could not be backed up. Nothing has been replaced. '+GetExceptionMessage; end;
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
var Sims: String;
begin
  Sims := '';
  if MsfsSelected then begin
    Sims := 'msfs';
    SetPreviousData(PreviousDataKey, 'Community', CommunityPage.Values[0]);
  end else SetPreviousData(PreviousDataKey, 'Community', PreviousCommunity);
  if XPlaneSelected then begin
    if Sims <> '' then Sims := Sims + ',';
    Sims := Sims + 'xplane';
    SetPreviousData(PreviousDataKey, 'XPlane', XPlaneRootValue);
  end else SetPreviousData(PreviousDataKey, 'XPlane', PreviousXPlane);
  SetPreviousData(PreviousDataKey, 'Simulators', Sims);
end;

#include "verify.iss"

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    if MsfsSelected and not SimulatorStopped then RaiseException('MSFS started during setup. Close it and run setup again.');
    if XPlaneSelected and not XPlaneStopped then RaiseException('X-Plane started during setup. Close it and run setup again.');
    if not PlainTree(WizardDirValue) then RaiseException('The destination changed into a linked directory.');
    if XPlaneSelected and not PlainTree(XPlaneDir('')) then RaiseException('The X-Plane plugin folder changed into a linked directory.');
  end;
  if CurStep = ssPostInstall then begin
    VerifyPayload;
    if BackupDir <> '' then
      SaveStringToFile(ExpandConstant('{localappdata}\FS Camera Studio\LAST-BACKUP.txt'),BackupDir+#13#10,False);
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := SimulatorStopped and XPlaneStopped and PlainTree(ExpandConstant('{app}'));
  if not Result then SuppressibleMsgBox('Close MSFS and X-Plane, and ensure this package is not a junction or symbolic link before uninstalling. Saved profiles will be kept.',mbError,MB_OK,IDOK);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfoInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'FS Camera Studio {#ReleaseVersion}'+NewLine+NewLine;
  if MsfsSelected then Result := Result+'MSFS 2024 package:'+NewLine+Space+WizardDirValue+NewLine+NewLine;
  if XPlaneSelected then Result := Result+'X-Plane 12 plugin (XP Camera Studio):'+NewLine+Space+XPlaneDir('')+NewLine+NewLine;
  Result := Result+
    'Existing package or plugin: backed up before replacement.'+NewLine+
    'Saved aircraft/airport cameras, settings and bindings: preserved.'+NewLine+
    'Uninstall: Windows Settings > Apps > FS Camera Studio (Tester).'+NewLine+NewLine+
    'Close the simulator before installing. This is a tester build; live flight verification is ongoing.';
end;
