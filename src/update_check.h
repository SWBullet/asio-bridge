#pragma once
#include <atomic>
#include <mutex>
#include <string>

// ============================================================================
// 在线升级检查（预留接口 + 完整链路）
// ----------------------------------------------------------------------------
// 更新源（自动回退）：
//   ① asio_bridge.cfg 配置 update_url=...（可指向国内服务器 / Gitee / 自建）
//   ② 内置 GitHub Releases API（默认，海外可达时可用）
// 清单格式（HTTP GET 返回文本，三种均可解析）：
//   JSON:  {"version":"1.0.1","url":"https://.../setup.exe","sha256":"<64hex>"}
//   INI:   version=1.0.1
//          url=https://...
//          sha256=<64hex>
//   （GitHub Releases API 的 JSON 自动适配其 tag_name/browser_download_url 字段）
// 网络失败/超时 → 静默跳过（不影响音频主功能），下次轮询再试。
// ============================================================================

// 软件当前版本（打包/发布时同步修改）
static const char* kAppVersion = "1.0.1";

// 更新检查状态（控制台只读，检查线程写）
struct UpdateState {
    std::atomic<bool>  checking{false};   // 正在检查
    std::atomic<bool>  available{false};  // 发现新版本
    std::atomic<bool>  downloading{false};// 正在下载
    std::atomic<bool>  error{false};      // 检查/下载出错
    std::atomic<bool>  quitRequested{false}; // 升级安装已启动，主程序应退出
    // 以下字符串由检查线程写、控制台线程读：用简单互斥保护，避免撕裂
    std::mutex*        mutex = nullptr;
    std::string        latestVersion;     // 远端版本号
    std::string        downloadUrl;       // 安装包直链
    std::string        sha256;            // 期望 SHA256（空=不校验）
    std::string        message;           // 人读信息（错误/状态）
};

// 装配 UpdateState::mutex（指向文件级静态锁）。必须在 startWebConsole()
// 之前调用：Web 线程启动第一毫秒就可能读 update 状态（UI 轮询 /api/status），
// 锁未装配时 lock_guard(*nullptr) 会崩溃。幂等，可重复调用。
void primeUpdateState(UpdateState* st);

// 启动后台检查线程（常驻，周期轮询）。stopFlag 置位后退出。
// cfgUpdateUrl: 配置文件里的 update_url（空=用内置 GitHub 源）
void startUpdateChecker(UpdateState* st, std::atomic<bool>* stopFlag,
                        const std::string& cfgUpdateUrl);

// 手动立即检查一次（控制台「检查更新」按钮）——在检查线程内执行
void requestUpdateCheck(UpdateState* st);

// 下载并启动升级（控制台「升级」按钮）——在检查线程内执行：
//   下载 setup.exe → 校验 SHA256（若提供）→ 启动安装程序(静默) → 置位退出请求
void requestUpdateDownload(UpdateState* st);
