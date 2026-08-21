' Hidden launcher for asio_bridge.
' Paths are derived from this script's own location (tools\..\build\Release),
' so moving the repo does not break the launcher.
' Log file is placed in the user's home directory.
Dim fso, ws, here, root, exe, logFile
Set fso = CreateObject("Scripting.FileSystemObject")
Set ws = CreateObject("Wscript.Shell")

here = fso.GetParentFolderName(fso.GetAbsolutePathName(WScript.ScriptFullName))
root = fso.GetParentFolderName(here)
exe = root & "\build\Release\asio_bridge.exe"
logFile = ws.ExpandEnvironmentStrings("%USERPROFILE%") & "\asio_bridge.log"

If Not fso.FileExists(exe) Then
    WScript.Echo "asio_bridge.exe not found: " & exe & " (build it first)"
    WScript.Quit 1
End If

ws.Run """" & exe & """ --log """ & logFile & """ --buffer 128 --dither", 0, False
