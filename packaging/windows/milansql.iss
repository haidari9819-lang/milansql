; ============================================================
; milansql.iss — Inno Setup script for MilanSQL Windows Installer
; Requires: Inno Setup 6+ (https://jrsoftware.org/isinfo.php)
; Build: ISCC milansql.iss
; ============================================================

[Setup]
AppName=MilanSQL
AppVersion=5.7.0
AppPublisher=Mirwais Haidari
AppPublisherURL=https://haidari9819-lang.github.io/milansql/
AppSupportURL=https://github.com/haidari9819-lang/milansql/issues
AppUpdatesURL=https://github.com/haidari9819-lang/milansql/releases
DefaultDirName={autopf}\MilanSQL
DefaultGroupName=MilanSQL
AllowNoIcons=yes
OutputDir=dist
OutputBaseFilename=milansql-setup-5.7.0
SetupIconFile=
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin
DisableProgramGroupPage=no
LicenseFile=..\..\LICENSE
InfoAfterFile=..\..\INSTALL.md

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german";  MessagesFile: "compiler:Languages\German.isl"

[Tasks]
Name: "desktopicon";     Description: "Create a &desktop icon";     GroupDescription: "Additional icons:"; Flags: unchecked
Name: "addtopath";       Description: "Add MilanSQL to system &PATH"; GroupDescription: "System integration:"; Flags: checkedonce
Name: "installservice";  Description: "Install as Windows &Service";  GroupDescription: "System integration:"; Flags: unchecked

[Files]
Source: "..\..\build\milansql.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\README.md";          DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE";            DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\INSTALL.md";         DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\data"; Permissions: everyone-modify

[Icons]
Name: "{group}\MilanSQL Server";       Filename: "{app}\milansql.exe"; Parameters: "--server --port 4406 --http --http-port 8080"; WorkingDir: "{app}\data"; Comment: "Start MilanSQL Server"
Name: "{group}\MilanSQL Client";       Filename: "{app}\milansql.exe"; Parameters: "--client --port 4406"; WorkingDir: "{app}"; Comment: "Connect to MilanSQL"
Name: "{group}\Uninstall MilanSQL";    Filename: "{uninstallexe}"
Name: "{autodesktop}\MilanSQL Server"; Filename: "{app}\milansql.exe"; Parameters: "--server --port 4406 --http --http-port 8080"; WorkingDir: "{app}\data"; Tasks: desktopicon

[Registry]
; Service registration
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Services\MilanSQL"; ValueType: string; ValueName: "ImagePath"; ValueData: """{app}\milansql.exe"" --server --port 4406 --http --http-port 8080 --data-dir ""{app}\data"""; Flags: uninsdeletekey; Tasks: installservice
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Services\MilanSQL"; ValueType: string; ValueName: "DisplayName"; ValueData: "MilanSQL Database Engine"; Tasks: installservice
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Services\MilanSQL"; ValueType: dword;  ValueName: "Start"; ValueData: "2"; Tasks: installservice

; App info
Root: HKLM; Subkey: "SOFTWARE\MilanSQL"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\MilanSQL"; ValueType: string; ValueName: "Version"; ValueData: "5.7.0"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\MilanSQL"; ValueType: string; ValueName: "DefaultPort"; ValueData: "4406"; Flags: uninsdeletekey

[Run]
; Start server after installation (optional)
Filename: "{app}\milansql.exe"; Description: "Start MilanSQL Server now"; Parameters: "--server --port 4406 --http --http-port 8080 --data-dir ""{app}\data"""; WorkingDir: "{app}\data"; Flags: postinstall nowait skipifsilent shellexec

[UninstallRun]
; Stop server before uninstall
Filename: "taskkill"; Parameters: "/F /IM milansql.exe"; Flags: runhidden

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  OldPath, NewPath: string;
begin
  if CurStep = ssPostInstall then begin
    if IsTaskSelected('addtopath') then begin
      OldPath := ExpandConstant('{reg:HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment,Path|}');
      if Pos(ExpandConstant('{app}'), OldPath) = 0 then begin
        NewPath := OldPath + ';' + ExpandConstant('{app}');
        RegWriteStringValue(HKLM,
          'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
          'Path', NewPath);
      end;
    end;
  end;
end;
