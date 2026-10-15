#ifndef StageDir
  #error StageDir is required
#endif
#ifndef PayloadList
  #error PayloadList is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef PackageVersion
  #error PackageVersion is required
#endif
#ifndef PackageCommit
  #error PackageCommit is required
#endif
#ifndef OutputName
  #error OutputName is required
#endif

[Setup]
AppId={{BF0350A3-5F01-44CD-AC64-C9AF030CC430}
AppName=Pulse 定制版
AppVersion={#PackageVersion}
AppVerName=Pulse 定制版 {#PackageVersion} ({#PackageCommit})
AppPublisher=Pulse Custom
DefaultDirName={localappdata}\Programs\PulseCustom
DisableDirPage=no
DefaultGroupName=Pulse 定制版
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=Pulse 定制版
UninstallDisplayIcon={app}\pulse.exe
UninstallFilesDir={app}\.installer
UsePreviousAppDir=yes
UsePreviousGroup=no
UsePreviousTasks=no
CloseApplications=no
RestartApplications=no
RestartIfNeededByRun=no
SetupLogging=yes
Compression=lzma2
SolidCompression=yes
DiskSpanning=no
WizardStyle=modern
LicenseFile={#StageDir}\LICENSE-Pulse.txt
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}

[Tasks]
Name: desktopicon; Description: "创建 Pulse 定制版桌面快捷方式"; Flags: unchecked

[Files]
#include PayloadList

[Icons]
Name: "{userprograms}\Pulse 定制版\Pulse 定制版"; Filename: "{app}\pulse.exe"; WorkingDir: "{app}"
Name: "{userprograms}\Pulse 定制版\卸载 Pulse 定制版"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Pulse 定制版"; Filename: "{app}\pulse.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Code]
const
  InstallationIdentity = 'BF0350A3-5F01-44CD-AC64-C9AF030CC430';
  InvalidFileAttributes = $FFFFFFFF;
  ReparsePointAttribute = $400;

function GetFileAttributes(FileName: string): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function GetCurrentProcessId(): Cardinal;
  external 'GetCurrentProcessId@kernel32.dll stdcall';

function PathWithin(Directory, ProtectedDirectory: String): Boolean;
begin
  ProtectedDirectory := RemoveBackslashUnlessRoot(ProtectedDirectory);
  Result := (CompareText(Directory, ProtectedDirectory) = 0) or
    (Pos(Lowercase(AddBackslash(ProtectedDirectory)), Lowercase(AddBackslash(Directory))) = 1);
end;

function InstallationDirectoryAllowed(Directory: String): Boolean;
var
  Profile: String;
begin
  Result := False;
  if Length(Directory) < 4 then exit;
  if (Directory[2] <> ':') or (Directory[3] <> '\') or
     (Pos(Uppercase(Copy(Directory, 1, 1)), 'ABCDEFGHIJKLMNOPQRSTUVWXYZ') = 0) then exit;
  if CompareText(Directory, RemoveBackslashUnlessRoot(ExpandFileName(Directory))) <> 0 then exit;
  if PathWithin(Directory, ExpandConstant('{win}')) or
     PathWithin(Directory, ExpandConstant('{commonpf}')) or
     PathWithin(Directory, ExpandConstant('{commonpf32}')) or
     PathWithin(Directory, ExpandConstant('{commonappdata}')) or
     PathWithin(Directory, ExpandConstant('{userappdata}')) or
     PathWithin(Directory, ExpandConstant('{localappdata}\Pulse')) or
     PathWithin(Directory, ExpandConstant('{localappdata}\PulseCustom')) then exit;
  if (CompareText(Directory, ExpandConstant('{localappdata}')) = 0) or
     (CompareText(Directory, ExpandConstant('{userdesktop}')) = 0) or
     (CompareText(Directory, ExpandConstant('{userdocs}')) = 0) then exit;
  Profile := GetEnv('USERPROFILE');
  if (Profile <> '') and (CompareText(Directory, RemoveBackslashUnlessRoot(Profile)) = 0) then exit;
  Result := True;
end;

function DirectorySafe(Directory: String): Boolean;
var
  Parent: String;
  Attributes: Cardinal;
begin
  Result := False;
  while Directory <> '' do begin
    Attributes := GetFileAttributes(Directory);
    if (Attributes <> InvalidFileAttributes) and
       ((Attributes and ReparsePointAttribute) <> 0) then exit;
    Parent := ExtractFileDir(Directory);
    if Parent = Directory then break;
    Directory := Parent;
  end;
  Result := True;
end;

function InstallationOwned(Directory: String): Boolean;
var
  Identity: AnsiString;
  Found: TFindRec;
begin
  Result := True;
  if not DirExists(Directory) then exit;
  if LoadStringFromFile(AddBackslash(Directory) + '.installer\pulse-custom-installation.id', Identity) then begin
    Result := Trim(String(Identity)) = InstallationIdentity;
    exit;
  end;
  if FindFirst(AddBackslash(Directory) + '*', Found) then begin
    try
      repeat
        if (Found.Name <> '.') and (Found.Name <> '..') then begin
          Result := False;
          break;
        end;
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
end;

function DirectoryTreeSafe(Directory: String; Depth: Integer): Boolean;
var
  Found: TFindRec;
begin
  Result := False;
  if Depth > 64 then exit;
  if not DirExists(Directory) then begin
    Result := True;
    exit;
  end;
  if FindFirst(AddBackslash(Directory) + '*', Found) then begin
    try
      repeat
        if (Found.Name <> '.') and (Found.Name <> '..') then begin
          if (Found.Attributes and ReparsePointAttribute) <> 0 then exit;
          if (Found.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            if not DirectoryTreeSafe(AddBackslash(Directory) + Found.Name, Depth + 1) then exit;
        end;
      until not FindNext(Found);
    finally
      FindClose(Found);
    end;
  end;
  Result := True;
end;

function OwnUninstallLaunchers(Services: Variant; Directory: String): String;
var
  Process: Variant;
  OriginalPath, CurrentPath, OriginalHash, ChildCreated, ParentCreated: String;
  ParentId, CurrentId: Cardinal;
  Depth: Integer;
begin
  Result := '';
  if not IsUninstaller then exit;
  OriginalPath := ExpandConstant('{uninstallexe}');
  if CompareText(ExtractFileDir(OriginalPath), AddBackslash(Directory) + '.installer') <> 0 then exit;
  CurrentId := GetCurrentProcessId();
  Process := Services.Get('Win32_Process.Handle="' + IntToStr(CurrentId) + '"');
  if VarIsNull(Process.ExecutablePath) or VarIsNull(Process.CreationDate) then exit;
  CurrentPath := String(Process.ExecutablePath);
  if CompareText(CurrentPath, OriginalPath) = 0 then exit;
  OriginalHash := GetSHA256OfFile(OriginalPath);
  if (Length(OriginalHash) <> 64) or (GetSHA256OfFile(CurrentPath) <> OriginalHash) then exit;
  ChildCreated := String(Process.CreationDate);
  ParentId := Process.ParentProcessId;
  for Depth := 0 to 7 do begin
    if (ParentId = 0) or (ParentId = CurrentId) then exit;
    Process := Services.Get('Win32_Process.Handle="' + IntToStr(ParentId) + '"');
    if VarIsNull(Process.ExecutablePath) or VarIsNull(Process.CreationDate) then exit;
    CurrentPath := String(Process.ExecutablePath);
    ParentCreated := String(Process.CreationDate);
    if (CompareText(CurrentPath, OriginalPath) <> 0) or
       (CompareText(ParentCreated, ChildCreated) > 0) then exit;
    if Pos(';' + IntToStr(ParentId) + ';', Result) <> 0 then exit;
    Result := Result + ';' + IntToStr(ParentId) + ';';
    Log(Format('已核验本次卸载启动器 PID=%d，路径=%s；不视为运行中的应用。', [ParentId, CurrentPath]));
    ChildCreated := ParentCreated;
    ParentId := Process.ParentProcessId;
  end;
end;

function InstallationBusy(Directory: String): Boolean;
var
  Locator, Services, Processes, Process: Variant;
  Index: Integer;
  ExecutablePath, Prefix, Launchers: String;
begin
  Result := True;
  Prefix := Lowercase(AddBackslash(Directory));
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Launchers := OwnUninstallLaunchers(Services, Directory);
    Processes := Services.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process');
    for Index := 0 to Processes.Count - 1 do begin
      Process := Processes.ItemIndex(Index);
      if (Process.ProcessId <> GetCurrentProcessId()) and
         (Pos(';' + IntToStr(Process.ProcessId) + ';', Launchers) = 0) and
         not VarIsNull(Process.ExecutablePath) then begin
        ExecutablePath := Lowercase(String(Process.ExecutablePath));
        if Pos(Prefix, ExecutablePath) = 1 then begin
          Log(Format('安装目录仍有运行进程 PID=%d，路径=%s；拒绝继续。', [Process.ProcessId, ExecutablePath]));
          exit;
        end;
      end;
    end;
    Result := False;
  except
    Log('无法可靠检查安装目录中的运行进程，拒绝继续。');
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Directory: String;
begin
  Result := '';
  Directory := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if not InstallationDirectoryAllowed(Directory) then
    Result := '请选择可写的本地磁盘绝对路径及独立子目录；不允许磁盘根目录、系统目录、用户配置目录或常用资料根目录。'
  else if not DirectorySafe(Directory) or not DirectoryTreeSafe(Directory, 0) then
    Result := '安装目录或父目录含有重解析点，已停止安装。'
  else if not InstallationOwned(Directory) then
    Result := '目标目录已有不属于本安装器的内容。请先自行备份并移走这些文件。'
  else if InstallationBusy(Directory) then
    Result := '请自行退出此安装目录运行的 Pulse 定制版及其辅助程序后重试；安装器不会结束任何进程。';
end;

function InitializeUninstall(): Boolean;
var
  Directory: String;
begin
  Directory := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  Result := InstallationDirectoryAllowed(Directory) and
    DirectorySafe(Directory) and DirectoryTreeSafe(Directory, 0) and InstallationOwned(Directory);
  if Result then Result := not InstallationBusy(Directory);
  if not Result then
    SuppressibleMsgBox('无法安全卸载：请关闭此安装目录中的 Pulse 定制版及其辅助程序，并检查安装目录身份。', mbError, MB_OK, IDOK);
end;
