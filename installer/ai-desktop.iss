; ai-desktop Windows installer (Inno Setup 6), built by .github/workflows/release.yml.
;
; It packs the output of scripts/setup.ps1 -- <StageDir>\app (binary, runtime
; DLLs, dashboard) and <StageDir>\ai-desktop.cmd -- so an installed copy has exactly
; the layout a setup.ps1 install has. The server serves the dashboard next to its
; binary from any working directory; the Start-menu shortcut still starts it in
; <app>\app, and ai-desktop.cmd keeps <app>\app (runtime DLLs) off PATH.
;
; Per-user install (no admin rights, no UAC prompt). The optional "add to PATH"
; task edits the user PATH only and undoes that on uninstall.
;
; Build locally:
;   ISCC.exe /DAppVersion=0.1.0 /DStageDir=C:\path\to\stage /ODist installer\ai-desktop.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is not defined: pass /DStageDir=<output of scripts/setup.ps1 -Prefix ...>
#endif

[Setup]
; Never change AppId: it is how a newer installer finds and upgrades an installed copy.
AppId={{5EA66451-99A0-4CBF-8A48-261C812EA959}
AppName=ai-desktop
AppVersion={#AppVersion}
AppVerName=ai-desktop {#AppVersion}
AppPublisher=Stefan Bartl
AppPublisherURL=https://github.com/WKDSteve/ai-desktop
AppSupportURL=https://github.com/WKDSteve/ai-desktop/issues
AppUpdatesURL=https://github.com/WKDSteve/ai-desktop/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\ai-desktop
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=ai-desktop-setup-{#AppVersion}-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesEnvironment=yes
; The icon (assets/ai-desktop.svg rendered to .ico) for the setup, the Start menu and Apps & features.
SetupIconFile=..\assets\ai-desktop.ico
UninstallDisplayIcon={app}\app\ai-desktop.ico

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add ai-desktop to the user &PATH (the ""ai-desktop"" command in new terminals)"; Flags: unchecked

[Files]
Source: "{#StageDir}\app\*"; DestDir: "{app}\app"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\ai-desktop.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\ai-desktop.ico"; DestDir: "{app}\app"; Flags: ignoreversion

[InstallDelete]
; The program was called loomAI before: an upgrade of that installation takes its old files and its shortcut away,
; so that no second program is left beside the new one.
Type: files; Name: "{app}\app\loomai.exe"
Type: files; Name: "{app}\loomai.cmd"
Type: files; Name: "{autoprograms}\loomAI.lnk"

[Icons]
Name: "{autoprograms}\ai-desktop"; Filename: "{app}\app\ai-desktop.exe"; WorkingDir: "{app}\app"; IconFilename: "{app}\app\ai-desktop.ico"; Comment: "Start the local ai-desktop server on 127.0.0.1:8080"
Name: "{autodesktop}\ai-desktop"; Filename: "{app}\app\ai-desktop.exe"; WorkingDir: "{app}\app"; IconFilename: "{app}\app\ai-desktop.ico"; Comment: "Start the local ai-desktop server on 127.0.0.1:8080"; Tasks: desktopicon

[Run]
Filename: "{app}\app\ai-desktop.exe"; WorkingDir: "{app}\app"; Description: "Start ai-desktop now"; Flags: postinstall nowait skipifsilent unchecked

[Code]
const
  EnvKey = 'Environment';

function SamePath(const A, B: string): Boolean;
begin
  Result := CompareText(RemoveBackslashUnlessRoot(A), RemoveBackslashUnlessRoot(B)) = 0;
end;

{ True if Dir is one of the ';'-separated entries of Path. }
function PathContains(const Path, Dir: string): Boolean;
var
  Rest, Item: string;
  P: Integer;
begin
  Result := False;
  Rest := Path;
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, MaxInt);
    end;
    if (Item <> '') and SamePath(Item, Dir) then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

{ Path is read and written unexpanded (RegQueryStringValue does not expand
  REG_EXPAND_SZ), so other entries keep their %VAR% references. }
procedure AddToUserPath(const Dir: string);
var
  Path: string;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Path := '';
  if PathContains(Path, Dir) then
    Exit;
  if (Path <> '') and (Path[Length(Path)] <> ';') then
    Path := Path + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Path + Dir);
end;

procedure RemoveFromUserPath(const Dir: string);
var
  Path, Rest, Item, NewPath: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Exit;
  Rest := Path;
  NewPath := '';
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Copy(Rest, P + 1, MaxInt);
    end;
    if (Item <> '') and (not SamePath(Item, Dir)) then
    begin
      if NewPath <> '' then
        NewPath := NewPath + ';';
      NewPath := NewPath + Item;
    end;
  end;
  if NewPath <> Path then
    RegWriteExpandStringValue(HKCU, EnvKey, 'Path', NewPath);
end;

{ The family folder of appkit (docs/FAMILY.md of appkit): APPKIT_FAMILY_DIR, else
  %LOCALAPPDATA%\appkit-family. The other apps of the family (tasks-desktop) read
  installed\ai-desktop.json there to know that ai-desktop is installed. }
function FamilyManifest: string;
var
  Dir: string;
begin
  Dir := GetEnv('APPKIT_FAMILY_DIR');
  if Dir = '' then
    Dir := ExpandConstant('{localappdata}') + '\appkit-family';
  Result := AddBackslash(Dir) + 'installed\ai-desktop.json';
end;

function JsonString(const S: string): string;
begin
  Result := S;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
  Result := '"' + Result + '"';
end;

{ Written through a temporary file; a failure only means the other apps do not
  offer ai-desktop, so it never stops the setup. UTF-8 with a byte order mark,
  which the reader of appkit allows. }
procedure RegisterInFamily;
var
  Path, Tmp: string;
  Lines: TArrayOfString;
begin
  Path := FamilyManifest;
  Tmp := Path + '.tmp';
  SetArrayLength(Lines, 7);
  Lines[0] := '{';
  Lines[1] := '  "schema": "appkit-installed/1",';
  Lines[2] := '  "app": "ai-desktop",';
  Lines[3] := '  "version": ' + JsonString('{#AppVersion}') + ',';
  Lines[4] := '  "program": ' + JsonString(ExpandConstant('{app}\app\ai-desktop.exe')) + ',';
  Lines[5] := '  "env_prefix": "AI_DESKTOP"';
  Lines[6] := '}';
  if not ForceDirectories(ExtractFileDir(Path)) then
    Exit;
  if not SaveStringsToUTF8File(Tmp, Lines, False) then
    Exit;
  DeleteFile(Path);
  if not RenameFile(Tmp, Path) then
    DeleteFile(Tmp);
end;

{ The command in any terminal (appkit's docs/FAMILY.md, "The command in the terminal"): the starters
  ai-desktop.cmd and ai-desktop (for Git Bash) go into <family folder>\bin, one folder on the user PATH for all apps of
  the family. The program writes them itself (ai-desktop cli-install), so their text exists once and a test covers it. }
function StarterDir: string;
begin
  Result := ExpandFileName(AddBackslash(ExtractFileDir(ExtractFileDir(FamilyManifest))) + 'bin');
end;

{ The entry as the PATH should hold it: %LOCALAPPDATA%\... for a folder below the local application data (so it
  follows the profile, like the entry of every other app of the family), else the whole path. }
function StarterPathEntry: string;
var
  Local, Dir: string;
begin
  Dir := StarterDir;
  Local := RemoveBackslashUnlessRoot(ExpandConstant('{localappdata}'));
  if (Length(Dir) > Length(Local)) and (CompareText(Copy(Dir, 1, Length(Local)), Local) = 0) and
     (Dir[Length(Local) + 1] = '\') then
    Result := '%LOCALAPPDATA%' + Copy(Dir, Length(Local) + 1, MaxInt)
  else
    Result := Dir;
end;

function PathHasStarterDir: Boolean;
var
  Path: string;
begin
  Result := False;
  if RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Result := PathContains(Path, StarterDir) or PathContains(Path, StarterPathEntry);
end;

procedure InstallCommand;
var
  Code: Integer;
begin
  { A failure only means the command is missing; it never stops the setup. }
  if not Exec(ExpandConstant('{app}\app\ai-desktop.exe'), 'cli-install', ExpandConstant('{app}\app'), SW_HIDE,
              ewWaitUntilTerminated, Code) or (Code <> 0) then
    Log('ai-desktop cli-install failed (' + IntToStr(Code) + '): no command in the terminal');
  { The user PATH is written only when it does not name the folder yet (and keeps its REG_EXPAND_SZ type); a test
    of the setup with APPKIT_FAMILY_DIR set never touches the PATH of the machine. }
  if (GetEnv('APPKIT_FAMILY_DIR') = '') and DirExists(StarterDir) and (not PathHasStarterDir) then
    AddToUserPath(StarterPathEntry);
end;

procedure RemoveCommand;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{app}\app\ai-desktop.exe'), 'cli-uninstall', ExpandConstant('{app}\app'), SW_HIDE,
       ewWaitUntilTerminated, Code);
  { The PATH entry goes with the last starter of the family (the program removes the empty folder). }
  if (GetEnv('APPKIT_FAMILY_DIR') = '') and (not DirExists(StarterDir)) then
  begin
    RemoveFromUserPath(StarterPathEntry);
    RemoveFromUserPath(StarterDir);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddToUserPath(ExpandConstant('{app}'));
  if CurStep = ssPostInstall then
  begin
    RegisterInFamily;
    InstallCommand;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { Before the files go: the program takes its own starters away. }
  if CurUninstallStep = usUninstall then
    RemoveCommand;
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveFromUserPath(ExpandConstant('{app}'));
    DeleteFile(FamilyManifest);
  end;
end;
