; Inno Setup script for the Windows installer.
;
;   iscc /DSourceDir=C:\path\to\dist /DAppName="disquisition" /DAppVersion=312 ^
;        /DOutputName=disquisition-desktop-windows-x64-setup scripts\windows-installer.iss
;
; Relative paths resolve against this script's folder, so pass SourceDir as an
; absolute path. The installer is written to the repository root.
;
; It installs for the current user, without asking for administrator rights,
; into a folder the user owns. That keeps the app's own updater working: it
; replaces files in the install folder in place, as it does for the .zip.

#ifndef SourceDir
  #error Pass /DSourceDir=<folder with disquisition-desktop.exe>
#endif
#ifndef AppName
  #define AppName "disquisition"
#endif
#ifndef AppVersion
  #define AppVersion "0"
#endif
#ifndef OutputName
  #define OutputName "disquisition-desktop-windows-x64-setup"
#endif
#define AppExe "disquisition-desktop.exe"

[Setup]
; Channels install side by side: "disquisition" and "disquisition staging".
AppId={#AppName}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName}
AppPublisher=disquisition
AppPublisherURL=https://disquisition.app
AppSupportURL=https://github.com/mmattbtw/disquisition/issues
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=..\resources\windows\disquisition.ico
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
OutputDir=..
OutputBaseFilename={#OutputName}

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; The app registers these on every start as well; the installer adds them up
; front so links work before the first launch, and removes them on uninstall.
Root: HKA; Subkey: "Software\Classes\disquisition"; ValueType: string; ValueName: ""; ValueData: "URL:{#AppName} invite"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\disquisition"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\disquisition\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"",0"
Root: HKA; Subkey: "Software\Classes\disquisition\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" -- ""%1"""

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Downloaded and replaced files from in-app updates.
Type: filesandordirs; Name: "{app}\.disquisition-update"
