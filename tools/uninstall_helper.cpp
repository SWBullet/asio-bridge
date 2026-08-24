// ============================================================================
// uninstall_helper.cpp — ASIO Bridge 一键干净卸载助手(完善版)
// ----------------------------------------------------------------------------
// 功能(一键):
//   1. 停止 asio_bridge.exe 进程(若在运行)
//   2. 移除开机自启计划任务 "ASIO Bridge"
//   3. 删除桌面/开始菜单快捷方式
//   4. 删除安装目录(注册表 Inno Setup 定位 / 常用路径 / 自身所在目录)
//   5. 若助手位于安装目录内, 自动计划延迟删除自身 + 目录
// 交互:
//   - 确认框: 列出将执行的每项操作 + 检测到的实况
//   - 结果框: 逐项打勾反馈实际结果
//   - 命令行加 --silent: 免确认静默执行(脚本/自动化)
//
// 构建(需 VS2022 命令行环境, 与 rebuild.cmd 同工具链):
//   rc.exe /fo uninstall_helper.res uninstall_helper.rc
//   cl /nologo /std:c++17 /utf-8 /O2 /EHsc /DUNICODE /D_UNICODE uninstall_helper.cpp ^
//      uninstall_helper.res /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib advapi32.lib
// 产物: uninstall_helper.exe (GUI 无黑框, 带图标)
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

// ---------- 1) 进程 ----------
static bool IsBridgeRunning() {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"asio_bridge.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool KillBridgeProcess() {
    bool killed = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"asio_bridge.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); killed = true; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    Sleep(500);   // 等待进程真正退出, 避免文件被锁
    return killed;
}

// ---------- 2) 计划任务 ----------
static bool RemoveScheduledTask() {
    wchar_t cmd[] = L"schtasks /Delete /TN \"ASIO Bridge\" /F";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);   // 0 = 删除成功; 1 = 任务不存在
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return code == 0;
    }
    return false;
}

// ---------- 3) 快捷方式 ----------
static bool RemoveShortcuts() {
    bool any = false;
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::wstring p = std::wstring(buf) + L"\\ASIO Bridge.lnk";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(p.c_str()); any = true; }
    }
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        std::wstring p = std::wstring(buf) + L"\\ASIO Bridge.lnk";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(p.c_str()); any = true; }
    }
    return any;
}

// ---------- 4) 定位安装目录 ----------
static std::wstring FindInstallDir() {
    const wchar_t* roots[] = {
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };
    for (auto root : roots) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &hk) != ERROR_SUCCESS) continue;
        DWORD i = 0;
        wchar_t name[256];
        while (RegEnumKeyW(hk, i++, name, 256) == ERROR_SUCCESS) {
            HKEY hk2 = nullptr;
            if (RegOpenKeyExW(hk, name, 0, KEY_READ, &hk2) != ERROR_SUCCESS) continue;
            DWORD type = 0;
            wchar_t disp[256] = {}; DWORD len = sizeof(disp);
            if (RegQueryValueExW(hk2, L"DisplayName", nullptr, &type, (BYTE*)disp, &len) == ERROR_SUCCESS &&
                wcsstr(disp, L"ASIO Bridge")) {
                wchar_t loc[MAX_PATH] = {}; len = sizeof(loc);
                if (RegQueryValueExW(hk2, L"InstallLocation", nullptr, &type, (BYTE*)loc, &len) == ERROR_SUCCESS && loc[0]) {
                    RegCloseKey(hk2); RegCloseKey(hk); return loc;
                }
                wchar_t uninst[MAX_PATH] = {}; len = sizeof(uninst);
                if (RegQueryValueExW(hk2, L"UninstallString", nullptr, &type, (BYTE*)uninst, &len) == ERROR_SUCCESS) {
                    std::wstring u = uninst;
                    size_t q1 = u.find(L'"');
                    size_t q2 = q1 != std::wstring::npos ? u.find(L'"', q1 + 1) : std::wstring::npos;
                    if (q1 != std::wstring::npos && q2 != std::wstring::npos) {
                        std::wstring exe = u.substr(q1 + 1, q2 - q1 - 1);
                        auto dir = std::filesystem::path(exe).parent_path();
                        if (!dir.empty()) { RegCloseKey(hk2); RegCloseKey(hk); return dir.wstring(); }
                    }
                }
            }
            RegCloseKey(hk2);
        }
        RegCloseKey(hk);
    }
    const wchar_t* common[] = { L"%LOCALAPPDATA%\\ASIO Bridge", L"%ProgramFiles%\\ASIO Bridge" };
    for (auto c : common) {
        wchar_t buf[MAX_PATH];
        if (ExpandEnvironmentStringsW(c, buf, MAX_PATH) && GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES)
            return buf;
    }
    return L"";
}

// ---------- 5) 安全护栏 ----------
// 只允许删除「名称含 ASIO Bridge 且非系统用户目录」的目标, 防止误删桌面/文档等
static bool LooksLikeInstallDir(const std::wstring& dir) {
    std::wstring lower = dir;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    if (lower.find(L"asio bridge") == std::wstring::npos) return false;
    const wchar_t* kSystem[] = { L"\\desktop", L"\\documents", L"\\downloads",
                                 L"\\pictures", L"\\videos", L"\\music", L"\\temp" };
    for (auto s : kSystem) {
        if (lower.find(s) != std::wstring::npos) return false;
    }
    return true;
}

// ---------- 6) 删除目录 + 自删 ----------
static bool CleanupDirAndSelf(const std::wstring& dir, const std::wstring& self) {
    std::error_code ec;
    bool selfInside = self.size() >= dir.size() &&
                      _wcsnicmp(self.c_str(), dir.c_str(), dir.size()) == 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().wstring() == self) continue;
        std::error_code ec2;
        std::filesystem::remove_all(entry.path(), ec2);
    }
    if (selfInside) {
        // 计划: 3 秒后删除自身 + 整目录
        wchar_t cmd[MAX_PATH * 2];
        wsprintfW(cmd, L"cmd /c ping 127.0.0.1 -n 3 > nul & del /f /q \"%s\" & rmdir /s /q \"%s\"",
                  self.c_str(), dir.c_str());
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        return true;
    }
    std::filesystem::remove_all(dir, ec);
    return !ec || ec == std::errc::no_such_file_or_directory;
}

// ---------- 主流程 ----------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    // --silent: 免确认静默执行
    bool silent = lpCmdLine && wcsstr(lpCmdLine, L"--silent") != nullptr;

    // 预检测(供确认框展示实况)
    bool procRunning = IsBridgeRunning();
    std::wstring self;
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    self = p;

    std::wstring dir = FindInstallDir();
    if (dir.empty()) {
        auto parent = std::filesystem::path(self).parent_path().wstring();
        size_t pos = parent.find(L"ASIO Bridge");
        if (pos != std::wstring::npos) dir = parent;
    }
    bool dirOK = !dir.empty() && LooksLikeInstallDir(dir);

    // 确认框
    if (!silent) {
        std::wstring msg = L"ASIO Bridge · 一键干净卸载\n\n将执行以下操作:\n";
        msg += procRunning ? L"  · 停止桥进程（当前在运行）\n"
                           : L"  · 停止桥进程（未运行）\n";
        msg += L"  · 移除开机自启计划任务「ASIO Bridge」\n";
        msg += L"  · 删除桌面/开始菜单快捷方式\n";
        msg += dirOK ? (L"  · 删除安装目录:\n      " + dir + L"\n")
                     : L"  · 安装目录: 未找到（跳过）\n";
        msg += L"\n是否继续?";
        if (MessageBoxW(nullptr, msg.c_str(), L"ASIO Bridge 卸载",
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;   // 用户取消
    }

    // 执行
    bool killed = KillBridgeProcess();
    bool task = RemoveScheduledTask();
    bool sc = RemoveShortcuts();
    bool deleted = dirOK && CleanupDirAndSelf(dir, self);

    // 结果框
    if (!silent) {
        std::wstring r = L"ASIO Bridge 卸载完成\n\n";
        r += killed ? L"  [✓] 桥进程: 已停止\n" : L"  [–] 桥进程: 未运行\n";
        r += task   ? L"  [✓] 计划任务: 已移除\n" : L"  [–] 计划任务: 未找到\n";
        r += sc     ? L"  [✓] 快捷方式: 已删除\n" : L"  [–] 快捷方式: 未找到\n";
        if (deleted)          r += L"  [✓] 安装目录: 已删除\n";
        else if (dirOK)       r += L"  [✗] 安装目录: 删除失败(可能有文件被占用)\n";
        else                  r += L"  [–] 安装目录: 未找到\n";
        MessageBoxW(nullptr, r.c_str(), L"ASIO Bridge 卸载", MB_OK | MB_ICONINFORMATION);
    }
    return 0;
}
