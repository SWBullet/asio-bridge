@echo off
rem Command-line rebuild of asio_bridge without CMake (MSVC x64 Release).
rem Usage: cmd /c tools\rebuild.cmd
rem NOTE: pass env paths with backslashes; output paths use forward slashes
rem       (bash-safety). advapi32/user32 are needed by ASIO SDK's asiolist.cpp
rem       (registry + CharLowerBuffA) -- CMake/VS adds them by default, raw cl.exe does not.
rem       winhttp/bcrypt are needed by update_check.cpp (online update: HTTP + SHA256),
rem       shell32 for ShellExecuteW (launching the installer).
rem       /MT = static CRT (self-contained exe, no VC++ runtime needed; matches CMake).
setlocal
set MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207
set WIN=C:\Program Files (x86)\Windows Kits\10
rem ensure cl.exe is reachable even outside a VS developer prompt
set PATH=%MSVC%\bin\Hostx64\x64;%PATH%
set INCLUDE=%MSVC%\include;%WIN%\Include\10.0.26100.0\ucrt;%WIN%\Include\10.0.26100.0\um;%WIN%\Include\10.0.26100.0\shared;%WIN%\Include\10.0.26100.0\winrt;%WIN%\Include\10.0.26100.0\cppwinrt
set LIB=%MSVC%\lib\x64;%WIN%\Lib\10.0.26100.0\ucrt\x64;%WIN%\Lib\10.0.26100.0\um\x64
cd /d E:\Harness\asio-bridge
if not exist build\obj mkdir build\obj
set SRC=src
set ASIO=third_party\ASIOSDK2.3
set CF=/nologo /std:c++17 /utf-8 /EHsc /O2 /MT /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /W3 /I%SRC% /I%ASIO%\common /I%ASIO%\host /I%ASIO%\host\pc
cl %CF% /c %SRC%\main.cpp %SRC%\wasapi_process_capture.cpp %SRC%\asio_render.cpp %SRC%\wasapi_output.cpp %SRC%\device_scan.cpp %SRC%\web_console.cpp %SRC%\update_check.cpp %ASIO%\common\asio.cpp %ASIO%\host\pc\asiolist.cpp /Fo:build/obj/
if errorlevel 1 (echo BUILD FAIL: main batch & exit /b 1)
rem asiodrivers.cpp needs windows.h force-included (per CMakeLists)
cl %CF% /FIwindows.h /c %ASIO%\host\asiodrivers.cpp /Fo:build/obj/asiodrivers.obj
if errorlevel 1 (echo BUILD FAIL: asiodrivers & exit /b 1)
cd build\obj
cl /nologo /Fe:../Release/asio_bridge_new.exe main.obj wasapi_process_capture.obj asio_render.obj wasapi_output.obj device_scan.obj web_console.obj update_check.obj asio.obj asiolist.obj asiodrivers.obj /link ole32.lib oleaut32.lib uuid.lib avrt.lib ws2_32.lib mmdevapi.lib advapi32.lib user32.lib shell32.lib winhttp.lib bcrypt.lib
if errorlevel 1 (echo LINK FAIL & exit /b 1)
echo BUILD OK: build\Release\asio_bridge_new.exe
echo (swap into place: stop the running bridge, then rename over asio_bridge.exe)
