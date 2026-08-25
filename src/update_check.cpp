#include "update_check.h"
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// ============================================================================
// 在线升级检查 + 下载 + 静默升级（后台线程常驻）
// ============================================================================

namespace {

const char* kGitHubApi =
    "https://api.github.com/repos/SWBullet/asio-bridge/releases/latest";

// ---- 小工具 ----

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r' || s[e-1] == '\n')) --e;
    return s.substr(b, e - b);
}

// 从 JSON 字符串取 "key":"value"（宽松：允许单引号、无引号值）
bool jsonString(const std::string& js, const std::string& key, std::string& out) {
    std::string k = "\"" + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return false;
    size_t c = js.find(':', p + k.size());
    if (c == std::string::npos) return false;
    c = js.find_first_not_of(" \t\r\n", c + 1);
    if (c == std::string::npos) return false;
    if (js[c] == '"') {
        size_t e = js.find('"', c + 1);
        if (e == std::string::npos) return false;
        out = js.substr(c + 1, e - c - 1);
        return true;
    }
    size_t e = js.find_first_of(",}\r\n", c);
    out = js.substr(c, (e == std::string::npos) ? std::string::npos : e - c);
    return true;
}

// INI 行解析：key=value
bool iniValue(const std::string& line, const std::string& key, std::string& out) {
    if (line.size() < key.size() + 2) return false;
    if (line.compare(0, key.size(), key) != 0) return false;
    if (line[key.size()] != '=') return false;
    out = trim(line.substr(key.size() + 1));
    return !out.empty();
}

// 版本号比较：v1.2.3 → (1,2,3)，纯数值逐段比较。返回 a>b
bool versionGreater(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v, int out[4]) {
        std::string s = v;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        int seg = 0, cur = 0;
        for (char ch : s) {
            if (ch == '.' || ch == '-') { out[seg++] = cur; cur = 0; if (seg >= 4) break; }
            else if (ch >= '0' && ch <= '9') cur = cur * 10 + (ch - '0');
        }
        if (seg < 4) out[seg] = cur;
        return seg;
    };
    int pa[4] = {0}, pb[4] = {0};
    parts(a, pa); parts(b, pb);
    for (int i = 0; i < 4; ++i) {
        if (pa[i] != pb[i]) return pa[i] > pb[i];
    }
    return false;
}

// WinHTTP GET：返回正文（失败返回空）
std::string httpGet(const std::string& url, int timeoutMs = 10000) {
    std::string result;
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    uc.dwSchemeLength = 0;
    std::wstring wurl(url.begin(), url.end());
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        fprintf(stderr, "[upd] WinHttpCrackUrl err=%lu\n", (unsigned long)GetLastError());
        return result;
    }
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    HINTERNET hInt = WinHttpOpen(L"asio-bridge-updater/1.0",
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hInt) return result;
    HINTERNET hConn = WinHttpConnect(hInt, host, uc.nPort, 0);
    if (hConn) {
        std::wstring verb(L"GET");
        HINTERNET hReq = WinHttpOpenRequest(hConn, verb.c_str(), path, nullptr, nullptr,
                                            nullptr, https ? WINHTTP_FLAG_SECURE : 0);
        if (hReq) {
            // GitHub Releases 下载直链会 302 重定向到 CDN：必须跟随重定向
            DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY,
                             &redirPolicy, sizeof(redirPolicy));
            std::wstring hdrs(L"Accept: application/json\r\nUser-Agent: asio-bridge-updater\r\n");
            if (WinHttpSendRequest(hReq, hdrs.c_str(), (DWORD)hdrs.size(), nullptr, 0, 0, 0)) {
                if (WinHttpReceiveResponse(hReq, nullptr)) {
                    char buf[4096];
                    DWORD got = 0;
                    while (WinHttpReadData(hReq, buf, sizeof(buf), &got) && got > 0) {
                        result.append(buf, got);
                        got = 0;
                    }
                } else {
                    fprintf(stderr, "[upd] WinHttpReceiveResponse err=%lu\n", (unsigned long)GetLastError());
                }
            } else {
                fprintf(stderr, "[upd] WinHttpSendRequest err=%lu\n", (unsigned long)GetLastError());
            }
            WinHttpCloseHandle(hReq);
        } else {
            fprintf(stderr, "[upd] WinHttpOpenRequest err=%lu\n", (unsigned long)GetLastError());
        }
        WinHttpCloseHandle(hConn);
    }
    WinHttpCloseHandle(hInt);
    return result;
}

// SHA256 文件校验（CNG）。返回小写 hex；失败返回空
std::string sha256File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    do {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
        BYTE buf[65536];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
            if (BCryptHashData(hash, buf, got, 0) != 0) { hex.clear(); break; }
        }
        if (!hex.empty() || GetLastError() == ERROR_HANDLE_EOF) {
            BYTE digest[32];
            if (BCryptFinishHash(hash, digest, 32, 0) == 0) {
                static const char* kHex = "0123456789abcdef";
                for (int i = 0; i < 32; ++i) {
                    hex += kHex[digest[i] >> 4];
                    hex += kHex[digest[i] & 15];
                }
            }
        }
    } while (false);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(h);
    return hex;
}

// 解析清单 → (version, url, sha256)。返回 true 表示清单有效
bool parseManifest(const std::string& body, std::string& ver, std::string& url, std::string& sha) {
    // GitHub Releases API JSON：tag_name / assets[].browser_download_url
    if (jsonString(body, "tag_name", ver)) {
        jsonString(body, "browser_download_url", url);
        // sha256 通常不在 API 里；发布时可把 hash 写进 release body，此处尽力找
        size_t p = body.find("sha256");
        if (p != std::string::npos) {
            size_t q = body.find(':', p);
            if (q != std::string::npos) {
                std::string t = trim(body.substr(q + 1));
                if (t.size() >= 64) { t = t.substr(0, 64); bool hexok = true;
                    for (char c : t) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) { hexok=false; break; }
                    if (hexok) sha = t; }
            }
        }
        return true;
    }
    // 自建清单：INI 风格
    std::string line, kv;
    for (char ch : body) {
        if (ch == '\n') {
            if (iniValue(line, "version", kv)) ver = kv;
            else if (iniValue(line, "url", kv)) url = kv;
            else if (iniValue(line, "sha256", kv)) sha = kv;
            line.clear();
        } else line += ch;
    }
    if (!line.empty()) {
        if (iniValue(line, "version", kv)) ver = kv;
        else if (iniValue(line, "url", kv)) url = kv;
        else if (iniValue(line, "sha256", kv)) sha = kv;
    }
    return !ver.empty();
}

} // namespace

// ============================================================================
// 后台线程主体
// ============================================================================
static void updateLoop(UpdateState* st, std::atomic<bool>* stop,
                       const std::string& cfgUrl) {
    std::string lastMsg;
    while (!stop->load(std::memory_order_relaxed)) {
        // 待办请求：手动检查 / 手动升级
        bool wantCheck = st->checking.exchange(false);
        bool wantDownload = st->downloading.exchange(false);
        if (wantDownload && st->available.load()) {
            // ---- 下载 + 校验 + 启动安装 ----
            std::string url, ver, sha, msg;
            {
                std::lock_guard<std::mutex> lk(*st->mutex);
                url = st->downloadUrl; ver = st->latestVersion; sha = st->sha256;
            }
            if (url.empty()) {
                { std::lock_guard<std::mutex> lk(*st->mutex); st->message = "清单缺少下载地址"; }
                st->error.store(true, std::memory_order_relaxed);
                continue;
            }
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            std::wstring file = std::wstring(tmp) + L"asio_bridge_setup_" +
                                std::wstring(ver.begin(), ver.end()) + L".exe";
            {
                std::lock_guard<std::mutex> lk(*st->mutex);
                st->message = "正在下载 v" + ver + " …";
            }
            // URLDownloadToFile 需要 wininet；此处用 WinHTTP 写文件避免引入依赖
            std::string body = httpGet(url, 60000);
            if (body.empty()) {
                { std::lock_guard<std::mutex> lk(*st->mutex); st->message = "下载失败（网络错误或超时）"; }
                st->error.store(true, std::memory_order_relaxed);
                continue;
            }
            // 注意：安装包是二进制，httpGet 的 std::string 可容纳，但 WinHTTP ReadData
            // 循环在收到 0 字节后停止，二进制不受影响。这里直接写文件。
            {
                HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) {
                    { std::lock_guard<std::mutex> lk(*st->mutex); st->message = "写入临时文件失败"; }
                    st->error.store(true, std::memory_order_relaxed);
                    continue;
                }
                DWORD w = 0;
                WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
                CloseHandle(h);
            }
            // 校验
            if (!sha.empty()) {
                std::string got = sha256File(file);
                std::string low = sha;
                for (auto& c : low) c = (char)tolower((unsigned char)c);
                if (got != low) {
                    DeleteFileW(file.c_str());
                    { std::lock_guard<std::mutex> lk(*st->mutex); st->message = "安装包校验失败（SHA256 不符）"; }
                    st->error.store(true, std::memory_order_relaxed);
                    continue;
                }
            }
            // 静默启动安装程序
            ShellExecuteW(nullptr, L"open", file.c_str(),
                          L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /FORCECLOSEAPPLICATIONS",
                          nullptr, SW_HIDE);
            {
                std::lock_guard<std::mutex> lk(*st->mutex);
                st->message = "升级安装已启动，程序即将退出…";
            }
            st->quitRequested.store(true, std::memory_order_release);
            return;   // 退出检查线程
        }
        if (wantCheck || st->available.load() == false) {
            // ---- 检查更新（周期轮询；发现新版本后停止轮询，等用户操作）----
            st->error.store(false, std::memory_order_relaxed);
            std::string source = cfgUrl.empty() ? kGitHubApi : cfgUrl;
            {
                std::lock_guard<std::mutex> lk(*st->mutex);
                st->message = "正在检查更新…";
            }
            std::string body = httpGet(source);
            std::string ver, url, sha;
            bool ok = !body.empty() && parseManifest(body, ver, url, sha);
            if (ok) {
                std::lock_guard<std::mutex> lk(*st->mutex);
                st->latestVersion = ver;
                st->downloadUrl = url;
                st->sha256 = sha;
                bool newer = versionGreater(ver, kAppVersion);
                st->available.store(newer, std::memory_order_relaxed);
                st->message = newer ? ("发现新版本 v" + ver) : ("已是最新版本 v" + std::string(kAppVersion));
            } else {
                std::lock_guard<std::mutex> lk(*st->mutex);
                st->message = "检查更新失败（网络不可达？）";
                st->error.store(true, std::memory_order_relaxed);
            }
            lastMsg = st->message;
        }
        // 周期轮询：已是最新 → 6 小时；发现新版 → 停（等用户）；出错 → 30 分钟
        DWORD waitMs = 3600000 * 6;
        if (st->available.load()) waitMs = 3600000 * 6;   // 保持低频
        if (st->error.load()) waitMs = 1800000;
        // 分段睡（1 秒 × N），保证 stop/检查/下载请求能被及时响应
        for (DWORD done = 0; done < waitMs && !stop->load(std::memory_order_relaxed); done += 1000) {
            // 用户点了「检查更新/下载升级」→ 立即醒来处理
            if (st->checking.load(std::memory_order_relaxed) ||
                st->downloading.load(std::memory_order_relaxed))
                break;
            Sleep(1000);
        }
    }
}

// ============================================================================
void startUpdateChecker(UpdateState* st, std::atomic<bool>* stopFlag,
                        const std::string& cfgUpdateUrl) {
    static std::mutex sMutex;   // 字符串保护区（控制台读同一把锁）
    st->mutex = &sMutex;
    std::thread t([st, stopFlag, cfgUpdateUrl] {
        updateLoop(st, stopFlag, cfgUpdateUrl);
    });
    t.detach();
}

void requestUpdateCheck(UpdateState* st) {
    st->checking.store(true, std::memory_order_relaxed);
}

void requestUpdateDownload(UpdateState* st) {
    st->downloading.store(true, std::memory_order_relaxed);
}
