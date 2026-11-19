#ifndef PayloadDir
  #error PayloadDir must point to a built PoolFS release.
#endif
#ifndef PoolFSVersion
  #error PoolFSVersion must contain the installer version.
#endif

[Setup]
AppId={{E3E04D44-5CF7-4B5D-B45C-55C7B1016509}
AppName=PoolFS
AppVersion={#PoolFSVersion}
AppPublisher=PoolFS
AppPublisherURL=https://poolfs.com/
AppSupportURL=https://poolfs.com/docs/gui
AppUpdatesURL=https://poolfs.com/#downloads
VersionInfoVersion={#PoolFSVersion}
DefaultDirName={localappdata}\Programs\PoolFS
DefaultGroupName=PoolFS
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
AllowNetworkDrive=no
AllowUNCPath=no
UninstallDisplayIcon={app}\poolfs-manager.exe
CloseApplications=no
RestartApplications=no
SetupMutex=PoolFS.Setup.E3E04D44
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputBaseFilename=poolfs-setup-x64

[Files]
Source: "{#PayloadDir}\poolfs-manager.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-gui-native.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-partitions.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-format.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-mount.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfsctl.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-check.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PayloadDir}\poolfs-scrub.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\PoolFS"; Filename: "{app}\poolfs-manager.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\PoolFS"; Filename: "{app}\poolfs-manager.exe"; WorkingDir: "{app}"

[Run]
Filename: "https://winfsp.dev/rel/"; Description: "Open the WinFsp download page (required to mount pools)"; Flags: postinstall shellexec skipifsilent unchecked; Check: WinFspMissing
Filename: "{app}\poolfs-manager.exe"; Description: "Open PoolFS Manager"; Flags: postinstall nowait skipifsilent unchecked; Check: not WinFspMissing

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  InUseMessage = 'Unmount any pools using this installation and close PoolFS Manager and command-line tools before continuing.';
  GenericWrite = $40000000;
  ShareReadWriteDelete = 7;
  OpenExisting = 3;
  FileAttributeNormal = $80;

function OpenForWrite(FileName: String; Access, ShareMode: DWORD;
  Security: UINT_PTR; Disposition, Flags: DWORD; Template: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseFileHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function FileInUse(const Name: String): Boolean;
var
  Path: String;
  Handle: THandle;
begin
  Path := ExpandConstant('{app}\') + Name;
  Result := False;
  if not FileExists(Path) then
    Exit;
  Handle := OpenForWrite(Path, GenericWrite, ShareReadWriteDelete, 0,
    OpenExisting, FileAttributeNormal, 0);
  Result := Handle = THandle(-1);
  if not Result then
    CloseFileHandle(Handle)
  else
    Log('PoolFS file cannot be updated: ' + Path);
end;

function InstalledFilesInUse: Boolean;
begin
  Result := FileInUse('poolfs-manager.exe') or FileInUse('poolfs-gui-native.dll') or
    FileInUse('poolfs-mount.exe') or FileInUse('poolfsctl.exe') or
    FileInUse('poolfs-format.exe') or FileInUse('poolfs-check.exe') or
    FileInUse('poolfs-scrub.exe') or FileInUse('poolfs-partitions.ps1');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if InstalledFilesInUse then
    Result := InUseMessage;
end;

function InitializeUninstall: Boolean;
begin
  Result := not InstalledFilesInUse;
  if not Result then
    SuppressibleMsgBox(InUseMessage, mbError, MB_OK, IDOK);
end;

function WinFspMissing: Boolean;
var
  RuntimeDir: String;
begin
  if RegQueryStringValue(HKLM32, 'SOFTWARE\WinFsp', 'InstallDir', RuntimeDir) then
    if FileExists(AddBackslash(RuntimeDir) + 'bin\winfsp-x64.dll') then
    begin
      Result := False;
      Exit;
    end;
  Result := not FileExists(ExpandConstant('{commonpf32}\WinFsp\bin\winfsp-x64.dll'));
end;

procedure CurUninstallStepChanged(Step: TUninstallStep);
var
  Names: TArrayOfString;
  Entry, ManagerPath: String;
  I: Integer;
begin
  if Step <> usPostUninstall then
    Exit;
  if not RegGetValueNames(HKCU, RunKey, Names) then
    Exit;
  ManagerPath := LowerCase(ExpandConstant('{app}\poolfs-manager.exe'));
  Log('Checking PoolFS sign-in entries for ' + ManagerPath);
  for I := 0 to GetArrayLength(Names) - 1 do
    if (Length(Names[I]) = 23) and (Pos('poolfs-', LowerCase(Names[I])) = 1) then
    begin
      if not RegQueryStringValue(HKCU, RunKey, Names[I], Entry) then
        Continue;
      Entry := LowerCase(Entry);
      if (Pos('"' + ManagerPath + '" --startup-mount ', Entry) = 1) or
         (Pos(ManagerPath + ' --startup-mount ', Entry) = 1) then
      begin
        if RegDeleteValue(HKCU, RunKey, Names[I]) then
          Log('Removed a PoolFS sign-in entry.')
        else
          Log('Could not remove a PoolFS sign-in entry.');
      end;
    end;
end;
