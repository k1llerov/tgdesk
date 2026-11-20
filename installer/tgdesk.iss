; TgDesk installer (Inno Setup 6). Built by scripts/build.ps1 -Installer or by GitHub Actions.
; Installs for the current user only (no administrator rights needed).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Dist
  #define Dist "..\dist"
#endif

[Setup]
AppId={{6E0B7F3A-4C1D-4B8E-9A55-2F7D3C1E9B40}
AppName=TgDesk
AppVersion={#AppVersion}
AppVerName=TgDesk {#AppVersion}
AppPublisher=TgDesk contributors
AppPublisherURL=https://github.com/k1llerov/tgdesk
AppSupportURL=https://github.com/k1llerov/tgdesk/issues
AppUpdatesURL=https://github.com/k1llerov/tgdesk/releases
DefaultDirName={localappdata}\Programs\TgDesk
DefaultGroupName=TgDesk
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#Dist}
OutputBaseFilename=TgDesk-Setup-{#AppVersion}
SetupIconFile=..\app\tgdesk.ico
UninstallDisplayIcon={app}\TgDesk.exe
UninstallDisplayName=TgDesk
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
LicenseFile=..\LICENSE
CloseApplications=no

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
ru.Autostart=Запускать TgDesk вместе с Windows (в трее)
en.Autostart=Start TgDesk with Windows (in the tray)
ru.Launch=Запустить TgDesk
en.Launch=Launch TgDesk
ru.FairUse=TgDesk даёт управление этим компьютером через Telegram. Устанавливай его только на свой компьютер или с согласия его владельца. Во время трансляции экрана на ПК всегда видна плашка, а все действия записываются в журнал.
en.FairUse=TgDesk gives control of this computer through Telegram. Install it only on your own computer or with its owner's consent. While the screen is streamed, a badge is always visible on the PC, and every action is logged.
ru.DeleteData=Удалить также настройки, пользователей и журналы TgDesk (%APPDATA%\TgDesk)?%n%nТокен бота будет удалён. Если не уверен — нажми «Нет».
en.DeleteData=Also delete TgDesk settings, users and logs (%APPDATA%\TgDesk)?%n%nThe bot token will be removed. If unsure, choose "No".

[Tasks]
Name: "autostart"; Description: "{cm:Autostart}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Dist}\TgDesk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\tgdesk.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\bot\*"; DestDir: "{app}\bot"; Flags: ignoreversion recursesubdirs
Source: "{#Dist}\webapp\*"; DestDir: "{app}\webapp"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\TgDesk"; Filename: "{app}\TgDesk.exe"
Name: "{autodesktop}\TgDesk"; Filename: "{app}\TgDesk.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "TgDesk"; ValueData: """{app}\TgDesk.exe"" --tray"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\TgDesk.exe"; Description: "{cm:Launch}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/im TgDesk.exe /f"; Flags: runhidden; RunOnceId: "KillApp"
Filename: "{sys}\taskkill.exe"; Parameters: "/im TgDeskBot.exe /f /t"; Flags: runhidden; RunOnceId: "KillBot"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\bot"
Type: filesandordirs; Name: "{app}\webapp"

[Code]
procedure StopTgDesk;
var Code: Integer;
begin
  { stop the app and the bot (with its cloudflared child) so the files can be replaced }
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im TgDesk.exe /f', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im TgDeskBot.exe /f /t', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(500);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not WizardSilent() then
    Result := MsgBox(CustomMessage('FairUse'), mbInformation, MB_OKCANCEL) = IDOK;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopTgDesk;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    { autostart may also have been turned on later from the app }
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'TgDesk');
  if CurUninstallStep = usPostUninstall then
  begin
    if (not UninstallSilent()) and (MsgBox(CustomMessage('DeleteData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(ExpandConstant('{userappdata}\TgDesk'), True, True, True);
  end;
end;
