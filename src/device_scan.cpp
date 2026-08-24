#include "device_scan.h"
#include <mmdeviceapi.h>
#include <propsys.h>
#include <windows.h>
#include <cctype>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// 设备扫描：枚举 WASAPI 渲染端点 + ASIO 驱动(注册表)，做名字匹配。
//   ASIO 驱动名 ↔ WASAPI 端点名没有官方映射，用「核心词子串 + 内置别名表」启发式，
//   未命中则标记为 WASAPI 独占(控制台可手动覆盖)。
// ============================================================================

// 内置别名：ASIO 驱动核心词 → 端点名应包含的词（覆盖常见硬件品牌/型号差异）
struct Alias { const char* driverCore; const char* endpointCore; };
static const Alias kAliases[] = {
    { "madiface",  "rme"      },   // RME MADIface ↔ RME ADI-2 / Babyface 等
    { "madiface",  "adi-2"    },
    { "realtek",   "realtek"  },
    { "focusrite", "focusrite"},
    { "scarlett",  "scarlett" },
    { "rme",       "rme"      },
};

// 小写 ASCII 工具
static std::string toLowerAscii(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}
static std::wstring toLowerW(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)tolower((wchar_t)c);
    return r;
}

// 从 ASIO 驱动名提取「核心词」：去掉常见词/空格，取最长的词
static std::string asioCore(const std::string& driverName) {
    std::string low = toLowerAscii(driverName);
    static const char* kDrop[] = { "asio", "driver", "usb", "audio", "thunderbolt", "firewire", "pcie" };
    for (const char* d : kDrop) {
        std::string ds(d);
        size_t p;
        while ((p = low.find(ds)) != std::string::npos) low.erase(p, ds.size());
    }
    // 按空格/分隔符切词，取最长的一段(>=4 字符)
    std::string best, cur;
    for (char c : low) {
        if (isalnum((unsigned char)c)) { cur += c; }
        else {
            if (cur.size() > best.size()) best = cur;
            cur.clear();
        }
    }
    if (cur.size() > best.size()) best = cur;
    return best.size() >= 4 ? best : std::string();
}

// 判断驱动名是否匹配端点名
static bool matchAsioToEndpoint(const std::string& driverName, const std::wstring& endpointName) {
    std::string core = asioCore(driverName);
    std::wstring en = toLowerW(endpointName);
    if (!core.empty()) {
        std::wstring wc(core.begin(), core.end());
        if (en.find(wc) != std::wstring::npos) return true;   // 直接子串命中
    }
    // 别名表：驱动核心词命中别名 → 检查端点是否含别名端点词
    for (const auto& a : kAliases) {
        std::string dc = toLowerAscii(a.driverCore);
        if (!core.empty() && core.find(dc) != std::string::npos) {
            std::wstring ec(a.endpointCore, a.endpointCore + strlen(a.endpointCore));
            ec = toLowerW(ec);
            if (en.find(ec) != std::wstring::npos) return true;
        }
    }
    return false;
}

// 纯注册表枚举 ASIO 驱动名
std::vector<std::string> ListAsioDriverNames() {
    std::vector<std::string> names;
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD i = 0;
        char name[256];
        DWORD len;
        while (true) {
            len = sizeof(name);
            LONG r = RegEnumKeyExA(hk, i++, name, &len, nullptr, nullptr, nullptr, nullptr);
            if (r != ERROR_SUCCESS) break;
            names.emplace_back(name);
        }
        RegCloseKey(hk);
    }
    return names;
}

// 扫描主体（在 MTA 线程执行）
static std::vector<DeviceEntry> scanInner(std::string& err) {
    std::vector<DeviceEntry> out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { err = "CoInitializeEx(MTA) 失败"; return out; }
    if (hr == RPC_E_CHANGED_MODE) { err = "当前线程非 MTA"; return out; }   // 由外层线程保证

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) {
        err = "创建 MMDeviceEnumerator 失败";
        CoUninitialize();
        return out;
    }

    std::vector<std::string> asioDrivers = ListAsioDriverNames();

    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;
            DeviceEntry e;
            LPWSTR idStr = nullptr;
            if (SUCCEEDED(dev->GetId(&idStr)) && idStr) { e.id = idStr; CoTaskMemFree(idStr); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                // 友好名优先，回退设备描述。
                // GUID 注意含 0x46：a45c254e-df1c-4efd-8020-67d146a850e0
                static const PROPERTYKEY kFriendly = { {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14 };
                static const PROPERTYKEY kDeviceDesc = { {0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 2 };
                const PROPERTYKEY* keys[2] = { &kFriendly, &kDeviceDesc };
                for (int ki = 0; ki < 2 && e.name.empty(); ++ki) {
                    PROPVARIANT v; PropVariantInit(&v);
                    HRESULT hg = ps->GetValue(*keys[ki], &v);
                    if (SUCCEEDED(hg) && v.vt == VT_LPWSTR && v.pwszVal)
                        e.name = v.pwszVal;
                    PropVariantClear(&v);
                }
                ps->Release();
            }
            if (e.name.empty()) e.name = L"(未命名端点)";
            // 名字匹配
            for (const auto& dn : asioDrivers) {
                if (matchAsioToEndpoint(dn, e.name)) {
                    e.asio = true;
                    e.asioDriver = dn;
                    break;
                }
            }
            out.push_back(std::move(e));
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    CoUninitialize();
    return out;
}

std::vector<DeviceEntry> ScanOutputDevices(std::string& err) {
    // 当前线程可能已是 STA(桥主线程)：委托 MTA 工作线程执行
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        auto r = scanInner(err);
        CoUninitialize();
        return r;
    }
    if (hr == RPC_E_CHANGED_MODE) {
        std::vector<DeviceEntry> result;
        std::string err2;
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done) {
            std::thread t([&] {
                HRESULT h2 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (SUCCEEDED(h2)) { result = scanInner(err2); CoUninitialize(); }
                SetEvent(done);
            });
            WaitForSingleObject(done, 15000);
            t.join();
            CloseHandle(done);
        }
        err = err2;
        return result;
    }
    err = "CoInitializeEx(MTA) 失败";
    return {};
}
