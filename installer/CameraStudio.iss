; Build with tools/build-installer.py. Payload includes come from a verified package receipt.
#include "release.iss"
#ifndef SimulatorProcessName
  #define SimulatorProcessName "FlightSimulator2024.exe"
#endif

[Setup]
AppId={{8C662A8B-F01B-43BE-9104-2D63CA78C921}
AppName=FS Camera Studio (Tester)
AppVersion={#ReleaseVersion}
AppVerName=FS Camera Studio {#ReleaseVersion}
AppPublisher=Devinci
VersionInfoVersion={#NumericVersion}
VersionInfoDescription=FS Camera Studio tester setup for MSFS 2024
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
  CommunityPage: TInputDirWizardPage;
  PreviousCommunity, BackupDir, BackedUpTarget: String;

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

function SimulatorStopped: Boolean;
var Locator, Services, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''{#SimulatorProcessName}''');
    Result := Processes.Count = 0;
  except Log('Could not verify whether MSFS is running: ' + GetExceptionMessage); end;
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

procedure InitializeWizard;
begin
  PreviousCommunity := GetPreviousData('Community', '');
  CommunityPage := CreateInputDirPage(wpInfoBefore, 'Choose your MSFS 2024 Community folder',
    'Steam, Microsoft Store and custom package locations are supported.',
    'Confirm the detected folder or browse to the Community folder shown by MSFS. Setup creates fs-camera-studio inside it. Close MSFS before continuing.', False, '');
  CommunityPage.Add('Community folder:');
  CommunityPage.Values[0] := DetectCommunity;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Result := True;
  if CurPageID = CommunityPage.ID then begin
    Error := ValidateTarget;
    Result := Error = '';
    if not Result then SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
  end;
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
var Target, Base: String; Suffix: Integer;
begin
  Result := ValidateTarget;
  if Result <> '' then exit;
  if not SimulatorStopped then begin
    Result := 'Close Microsoft Flight Simulator 2024 before installing. Setup could not confirm that the simulator is stopped.'; exit;
  end;
  Target := WizardDirValue;
  if DirExists(Target) and (BackedUpTarget <> Target) then begin
    try
      Base := ExpandConstant('{localappdata}\FS Camera Studio\Backups\')+GetDateTimeString('yyyymmdd-hhnnss','-',':');
      BackupDir := Base; Suffix := 0;
      while DirExists(BackupDir) do begin Suffix := Suffix+1; BackupDir := Base+'-'+IntToStr(Suffix); end;
      CopyBackup(Target, BackupDir+'\fs-camera-studio');
      BackedUpTarget := Target;
      Log('Verified rollback package: '+BackupDir);
    except Result := 'The previous package could not be backed up. Nothing has been replaced. '+GetExceptionMessage; end;
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Community', CommunityPage.Values[0]);
end;

#include "verify.iss"

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then begin
    if not SimulatorStopped then RaiseException('MSFS started during setup. Close it and run setup again.');
    if not PlainTree(WizardDirValue) then RaiseException('The destination changed into a linked directory.');
  end;
  if CurStep = ssPostInstall then begin
    VerifyPayload;
    if BackupDir <> '' then
      SaveStringToFile(ExpandConstant('{localappdata}\FS Camera Studio\LAST-BACKUP.txt'),BackupDir+#13#10,False);
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := SimulatorStopped and PlainTree(ExpandConstant('{app}'));
  if not Result then SuppressibleMsgBox('Close MSFS and ensure this package is not a junction or symbolic link before uninstalling. Saved profiles will be kept.',mbError,MB_OK,IDOK);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfoInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'FS Camera Studio {#ReleaseVersion}'+NewLine+NewLine+
    'Install package:'+NewLine+Space+WizardDirValue+NewLine+NewLine+
    'Existing package: backed up before replacement.'+NewLine+
    'Saved aircraft/airport cameras, settings and bindings: preserved.'+NewLine+
    'Uninstall: Windows Settings > Apps > FS Camera Studio (Tester).'+NewLine+NewLine+
    'Close MSFS before installing. This is a tester build; live flight verification is ongoing.';
end;
