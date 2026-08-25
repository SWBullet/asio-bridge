' Hidden launcher for asio_bridge.
' 路径自适应：优先安装版(本脚本同级的父目录\asio_bridge.exe)，
' 回退开发版(父目录\build\Release\asio_bridge.exe)，移动目录不失效。
' 日志文件在用户主目录。
Dim fso, ws, here, root, exe, logFile, instExe, devExe
Set fso = CreateObject("Scripting.FileSystemObject")
Set ws = CreateObject("Wscript.Shell")

here = fso.GetParentFolderName(fso.GetAbsolutePathName(WScript.ScriptFullName))
root = fso.GetParentFolderName(here)
instExe = root & "\asio_bridge.exe"                  ' 安装版: {app}\asio_bridge.exe
devExe  = root & "\build\Release\asio_bridge.exe"    ' 开发版: 仓库\build\Release\asio_bridge.exe

If fso.FileExists(instExe) Then
    exe = instExe
ElseIf fso.FileExists(devExe) Then
    exe = devExe
Else
    WScript.Echo "asio_bridge.exe not found (tried: " & instExe & " / " & devExe & ")"
    WScript.Quit 1
End If

logFile = ws.ExpandEnvironmentStrings("%USERPROFILE%") & "\asio_bridge.log"

' 窗口样式 0 = 隐藏；--hidden 双保险(即使窗口已建也立即隐藏)
ws.Run """" & exe & """ --log """ & logFile & """ --hidden --buffer 128 --dither", 0, False
