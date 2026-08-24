// ============================================================================
// uninstall_helper.cpp — ASIO Bridge 一键干净卸载助手
// ----------------------------------------------------------------------------
// 功能(一键):
//   1. 停止 asio_bridge.exe 进程(若在运行)
//   2. 移除开机自启计划任务 "ASIO Bridge"
//   3. 删除桌面/开始菜单快捷方式
//   4. 删除安装目录(注册表 Inno Setup 定位 / 常用路径 / 自身所在目录)
//   5. 若助手位于安装目录内, 自动计划延迟删除自身 + 目录
// 运行方式: 双击运行, 弹确认框, 点「是」即完成干净卸载。
//
// 构建(需 VS2022 命令行环境, 与 rebuild.cmd 同工具链):
//   cl /nologo /std:c++17 /utf-8 /O2 /EHsc /DUNICODE /D_UNICODE uninstall_helper.cpp ^
//      /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib advapi32.lib
// 产物: uninstall_helper.exe (GUI 无黑框)
// ============================================================================
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <filesystem>
#include <string>

// 1) 结束 asio_bridge 进程
static void KillBridgeProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"asio_bridge.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    Sleep(500);   // 等待进程真正退出, 避免文件被锁
}

// 2) 移除开机自启计划任务
static void RemoveScheduledTask() {
    wchar_t cmd[] = L"schtasks /Delete /TN \"ASIO Bridge\" /F";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// 3) 删除桌面/开始菜单快捷方式
static void RemoveShortcuts() {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, buf)))
        DeleteFileW((std::wstring(buf) + L"\\ASIO Bridge.lnk").c_str());
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT, buf)))
        DeleteFileW((std::wstring(buf) + L"\\ASIO Bridge.lnk").c_str());
}

// 4) 定位安装目录: 注册表(Inno Setup) → 常用路径
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

// 安全护栏: 只允许删除「名称含 ASIO Bridge 且非系统用户目录」的目标,
// 防止误删桌面/文档等任意目录
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

// 5) 删除目录内容(跳过自身), 并计划延迟自删+删目录
static void CleanupDirAndSelf(const std::wstring& dir, const std::wstring& self) {
    std::error_code ec;
    bool selfInside = self.size() >= dir.size() &&
                      _wcsnicmp(self.c_str(), dir.c_str(), dir.size()) == 0;
    // 删除顶层子项(跳过自身); remove_all 递归处理嵌套
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().wstring() == self) continue;
        std::error_code ec2;
        std::filesystem::remove_all(entry.path(), ec2);
    }
    if (selfInside) {
        // 计划: 等待 3 秒后删除自身 + 整目录(经典自删技巧)
        wchar_t cmd[MAX_PATH * 2];
        wsprintfW(cmd, L"cmd /c ping 127.0.0.1 -n 3 > nul & del /f /q \"%s\" & rmdir /s /q \"%s\"",
                  self.c_str(), dir.c_str());
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    } else {
        std::filesystem::remove_all(dir, ec);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    // --silent: 免确认自动执行(脚本/测试用)
    bool silent = lpCmdLine && wcsstr(lpCmdLine, L"--silent") != nullptr;

    KillBridgeProcess();
    RemoveScheduledTask();
    RemoveShortcuts();

    std::wstring self;
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    self = p;

    std::wstring dir = FindInstallDir();
    // 兜底: 若助手在某个 ASIO Bridge 目录内且注册表未记录, 用自身所在目录
    if (dir.empty()) {
        auto parent = std::filesystem::path(self).parent_path().wstring();
        size_t pos = parent.find(L"ASIO Bridge");
        if (pos != std::wstring::npos) dir = parent;
    }

    std::wstring msg = L"ASIO Bridge 干净卸载\n\n已停止桥进程、移除开机自启任务、删除快捷方式。";
    if (!dir.empty() && LooksLikeInstallDir(dir)) {
        msg += L"\n\n将删除安装目录:\n" + dir;
        if (self.size() >= dir.size() && _wcsnicmp(self.c_str(), dir.c_str(), dir.size()) == 0)
            msg += L"\n(卸载助手位于该目录, 将自动清理自身)";
        if (silent || MessageBoxW(nullptr, (msg + L"\n\n确定删除吗?").c_str(), L"ASIO Bridge 卸载",
                                  MB_YESNO | MB_ICONWARNING) == IDYES) {
            CleanupDirAndSelf(dir, self);
            if (!silent)
                MessageBoxW(nullptr, L"已干净卸载。", L"ASIO Bridge 卸载", MB_OK | MB_ICONINFORMATION);
        }
    } else {
        if (!silent) {
            msg += dir.empty() ? L"\n\n未找到安装目录(可能是绿色版/未注册)。无需删除文件。"
                               : L"\n\n目标目录不符合安全校验, 已跳过删除。";
            MessageBoxW(nullptr, msg.c_str(), L"ASIO Bridge 卸载", MB_OK | MB_ICONINFORMATION);
        }
    }
    return 0;
}
