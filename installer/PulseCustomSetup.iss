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
DisableDirPage=yes
DefaultGroupName=Pulse 定制版
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=Pulse 定制版
UninstallDisplayIcon={app}\pulse.exe
UninstallFilesDir={app}\.installer
UsePreviousAppDir=no
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

function DefaultInstallationDirectory(): String;
begin
  Result := ExpandConstant('{localappdata}\Programs\PulseCustom');
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

function InstallationBusy(Directory: String): Boolean;
var
  Locator, Services, Processes, Process: Variant;
  Index: Integer;
  ExecutablePath, Prefix: String;
begin
  Result := True;
  Prefix := Lowercase(AddBackslash(Directory));
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process');
    for Index := 0 to Processes.Count - 1 do begin
      Process := Processes.ItemIndex(Index);
      if (Process.ProcessId <> GetCurrentProcessId()) and not VarIsNull(Process.ExecutablePath) then begin
        ExecutablePath := Lowercase(String(Process.ExecutablePath));
        if Pos(Prefix, ExecutablePath) = 1 then exit;
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
  if CompareText(Directory, DefaultInstallationDirectory()) <> 0 then
    Result := '为保护原版和其他程序，只允许安装到当前用户的 Programs\PulseCustom 目录。'
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
  Result := (CompareText(Directory, DefaultInstallationDirectory()) = 0) and
    DirectorySafe(Directory) and DirectoryTreeSafe(Directory, 0) and InstallationOwned(Directory);
  if Result then Result := not InstallationBusy(Directory);
  if not Result then
    MsgBox('无法安全卸载：请关闭此安装目录中的 Pulse 定制版及其辅助程序，并检查安装目录身份。', mbError, MB_OK);
end;
