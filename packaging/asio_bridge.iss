; ============================================================================
; ASIO Bridge · Inno Setup 一键安装脚本
; 用法:
;   1. 安装 Inno Setup 6 (https://jrsoftware.org/isinfo.php)
;   2. 先用 CMake 构建 Release(静态链接, 免 VC++ 运行库):
;        cmake -B build -S . && cmake --build build --config Release
;   3. 用 Inno Setup 编译本脚本:
;        ISCC.exe packaging\asio_bridge.iss
;    产物: packaging\output\asio_bridge_setup.exe
; 说明:
;   - 默认装到 %LOCALAPPDATA%\ASIO Bridge(用户可写, cfg/日志正常落盘)
;   - 安装器以管理员运行(用于安装开机自启计划任务)
;   - 可选勾选「开机自动启动」「桌面快捷方式」
; ============================================================================
#define MyAppName "ASIO Bridge"
#define MyAppVersion "1.0.10"
#define MyAppPublisher "文超工作室"
#define MyAppExeName "asio_bridge.exe"

[Setup]
AppId={{8F3A5C1E-6D2B-4A9C-B4E7-1F2D3A4B5C6D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\ASIO Bridge
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename=asio_bridge_setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: unchecked
Name: "autostart"; Description: "开机自动启动(计划任务, 隐藏运行)"; GroupDescription: "附加任务:"; Flags: unchecked

[Files]
; 主程序(单文件, 静态链接, 无 VC++ 运行库依赖)
Source: "..\build\Release\asio_bridge.exe"; DestDir: "{app}"; Flags: ignoreversion
; 一键干净卸载助手
Source: "..\packaging\output\uninstall_helper.exe"; DestDir: "{app}"; Flags: ignoreversion
; 工具脚本
Source: "..\tools\install_autostart.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\start_hidden.vbs"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\test_render.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\detect_source.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\watch_bridge.ps1"; DestDir: "{app}\tools"; Flags: ignoreversion
; 文档
Source: "..\docs\使用教程.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\Bug修复说明-驱动自动适配.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\docs\更新说明-设备自动适配强化.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
; Web 控制台 UI(磁盘版优先于内嵌, 可即时修改刷新浏览器生效; 删除后自动回退内嵌版)
Source: "..\web\index.html"; DestDir: "{app}\web"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--hidden"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Parameters: "--hidden"; Tasks: desktopicon

[Run]
; 安装完可选择直接启动桥(隐藏窗口)
Filename: "{app}\{#MyAppExeName}"; Parameters: "--hidden"; Description: "立即运行 ASIO Bridge"; Flags: nowait postinstall skipifsilent unchecked
; 勾选「开机自动启动」时安装计划任务(隐藏窗口)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\install_autostart.ps1"""; Tasks: autostart; Flags: runhidden

[UninstallRun]
; 卸载时移除计划任务(忽略错误)
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\tools\install_autostart.ps1"" -Remove"; Flags: runhidden
