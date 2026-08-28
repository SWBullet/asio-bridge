// Bridge: 目标应用渲染流 → (进程回环按 PID 旁路取流) → ASIO/WASAPI 独占输出
// 免虚拟声卡、免引擎混音/APO；自动静音目标端点消除双重声。
#include "asio_render.h"
#include "audio_output.h"
#include "device_scan.h"
#include "wasapi_output.h"
#include "dsp_tube.h"
#include "dsp_thickness.h"
#include "dsp_softclip.h"   // 末端软过载限幅器（电子管软过载特性，永不超过 0.999）
#include "rate_lock.h"
#include "resampler.h"
#include "ring_buffer.h"
#include "wasapi_process_capture.h"
#include "util.h"
#include "update_check.h"
#include "web_console.h"
#include <windows.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr：RAII COM 指针，消灭手写 Release
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <fcntl.h>
#include <io.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

static std::atomic<bool> g_stop{false};

// ===== 崩溃线程身份注册表 =====
// 崩溃过滤器运行于「崩溃线程自身」，把线程 ID 翻译成身份写进 crash.log，
// 直接回答「到底哪个线程在驱动内部炸了」——旧全局 g_where 标签跨线程污染，
// 驱动线程崩溃全被误记成 main:cfg（主线程 5 秒一次 saveConfig 刷新的陈旧值）。
static volatile DWORD g_tidMain = 0, g_tidCap = 0, g_tidPull = 0,
                      g_tidDisc = 0, g_tidWeb = 0, g_tidDevNote = 0;
static const char* tidName(DWORD t) {
    if (t == g_tidMain)   return "main";
    if (t == g_tidCap)    return "capture";
    if (t == g_tidPull)   return "asio-callback";
    if (t == g_tidDisc)   return "discovery";
    if (t == g_tidWeb)    return "web-console";
    if (t == g_tidDevNote)return "device-notify";
    return "driver/com-rpc/other";
}

// 设备事件监听（Core Audio 属性监听模型）：渲染设备增删/状态变化/默认设备
// 变化 → 触发链路重建 + 设备列表重扫标记。回调在 MTA 线程池线程上执行，
// 只写原子量、零 COM。
class DeviceNotifier : public IMMNotificationClient {
public:
    DeviceNotifier(std::atomic<bool>* restart, std::atomic<bool>* devicesDirty, std::atomic<bool>* stop)
        : ref_(1), restart_(restart), devicesDirty_(devicesDirty), stop_(stop) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IMMNotificationClient) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        (void)id; (void)state;
        bump();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        (void)id;
        bump();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        (void)id;
        bump();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        (void)id;
        if (flow == eRender && role == eConsole) bump();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
        (void)id; (void)key;
        return S_OK;
    }
private:
    void bump() {
        if (!g_tidDevNote) g_tidDevNote = GetCurrentThreadId();   // 回调线程池线程身份
        if (stop_->load(std::memory_order_relaxed)) return;
        restart_->store(true, std::memory_order_relaxed);          // 重建链路
        devicesDirty_->store(true, std::memory_order_relaxed);     // 设备列表待重扫
    }
    LONG ref_;
    std::atomic<bool>* restart_;
    std::atomic<bool>* devicesDirty_;
    std::atomic<bool>* stop_;
};

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

static void usage() {
    printf(
        "ASIO Bridge - 应用 PCM 直通 ASIO / WASAPI 独占输出\n"
        "用法:\n"
        "  asio_bridge                    默认(后台隐藏): 进程回环采集 -> 自动选择输出设备\n"
        "                                (全手动选择输出设备: ASIO 优先, 控制台下拉指定; 无 ASIO 时可选 WASAPI 独占)\n"
        "  asio_bridge --show            显示控制台窗口（默认后台隐藏，调试/看日志用）\n"
        "  asio_bridge --list             列出 ASIO 驱动\n"
        "  asio_bridge --tone             1kHz 正弦测试: 直接经 ASIO 输出(验证 ASIO 链路)\n"
        "  asio_bridge --driver <名字>    指定 ASIO 驱动 (默认自动选择第一个可用驱动)\n"
        "  asio_bridge --rate <Hz>        --tone 模式的采样率 (默认 44100)\n"
        "  asio_bridge --buffer <帧数>    ASIO 缓冲帧数 (默认驱动值, 低延迟可试 128/64)\n"
        "  asio_bridge --log <文件>      输出追加写入日志文件（后台/自启运行用）\n"
        "  asio_bridge --no-dither      关闭 TPDF 抖动（默认开启）\n"
        "  asio_bridge --resampler-test  重采样器正弦离线自检（数值验证）\n"
        "  asio_bridge --pll-test        速率锁+水位闭环联合仿真自检（数值验证）\n"
        "  asio_bridge --capture-test    Bridge 采集正弦自检（渲染→按 PID 回环自采比对）\n"
        "  asio_bridge --crash-test      启动后故意崩溃，验证崩溃自愈链路（诊断用）\n"
        "  asio_bridge --respawn <毫秒>  （内部）崩溃自愈拉起前等待旧实例退出\n"
        "Ctrl+C 停止；崩溃时自动重启（5 分钟内最多 5 次，防死循环），详见 restarts.log\n");
}

// ===== 频谱观察(瀑布图)FFT =====
static constexpr int kFftN = 256;       // FFT 点数
static constexpr int kFftBins = 128;    // 频率 bin 数(0..Nyquist)

// 迭代 radix-2 复 FFT(就地)
static void fftRadix2(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float tr = re[i]; re[i] = re[j]; re[j] = tr; float ti = im[i]; im[i] = im[j]; im[j] = ti; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.28318530717958647692f / (float)len;
        float wlr = cosf(ang), wli = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                float vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nwr = wr * wlr - wi * wli;
                float nwi = wr * wli + wi * wlr;
                wr = nwr; wi = nwi;
            }
        }
    }
}

// 计算频谱幅值(Hann 窗 + FFT + 幅值),mag 输出 n/2 个 bin
static void computeSpectrum(const float* in, float* mag, float* re, float* im, int n) {
    for (int i = 0; i < n; ++i) {
        float wnd = 0.5f * (1.0f - cosf(6.28318530717958647692f * (float)i / (float)(n - 1)));
        re[i] = in[i] * wnd;
        im[i] = 0.0f;
    }
    fftRadix2(re, im, n);
    for (int k = 0; k < n / 2; ++k)
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
}

// ===== 崩溃定位面包屑 =====
// 各阶段进入前更新 g_where,未处理异常过滤器把崩溃位置 + 异常地址写 crash.log,
// 用于定位空指针解引用(0xc0000005)到底发生在哪个环节。
// 线程局部(TLS)：过滤器在崩溃线程上执行，读到的是该线程自己的最后标签；
// 未标记线程（驱动内部线程/COM RPC 线程池）显示 "unlabeled-thread"，
// 配合 tidName() 即可定位崩溃线程身份
static __declspec(thread) const char* g_where = "unlabeled-thread";

// 把地址解析成 "模块名+0x偏移"——空指针 CALL 的调用点属于哪个 DLL 一目了然。
// 崩溃上下文只能用内核/简单 Win32 API,绝不触碰 C++ 对象或分配内存。
static void resolveModule(void* addr, char* out, size_t outCap) {
    (void)outCap;
    if (!addr) { lstrcpyA(out, "null"); return; }
    HMODULE hmod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &hmod) && hmod) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(hmod, path, MAX_PATH);
        const char* name = path;
        if (n) {
            for (const char* s = path + n; s > path; --s)
                if (s[-1] == '\\' || s[-1] == '/') { name = s; break; }
        }
        ULONGLONG base = (ULONGLONG)(ULONG_PTR)hmod;
        ULONGLONG off  = (ULONGLONG)(ULONG_PTR)addr - base;
        // wsprintfA 不支持 64 位格式符：偏移 < 4GB 用 32 位,否则拼高低两段
        if (off <= 0xFFFFFFFFull)
            wsprintfA(out, "%s+0x%X", name, (DWORD)off);
        else
            wsprintfA(out, "%s+0x%X%08X", name, (DWORD)(off >> 32), (DWORD)off);
    } else {
        wsprintfA(out, "0x%p", addr);
    }
}

// 把崩溃日志路径固定到 exe 同目录(build/Release),而不是进程 CWD——
// 之前从 pwsh Start-Process 启动时 CWD 是 E:\Harness,crash.log 落到那里,找错位置。
static void crashLogPath(wchar_t* out, size_t cap) {
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap) { lstrcpyW(out, L"crash.log"); return; }
    wchar_t* slash = out + n;
    while (slash > out && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
    lstrcpyW(slash, L"crash.log");
}

// ===== 崩溃自愈:驱动级空指针崩溃后自动拉起新实例 =====
// RME 驱动掉线时会清空自身函数表,驱动线程回头调用空指针把整个进程带走
// (crash.log: madiface_asio_amd.dll+0x4385, rip=0)。进程内无法拦截驱动线程的
// 崩溃,唯一出路是 crashFilter 里拉起新实例接管。为防"启动即崩"死循环,
// 用 exe 同目录 restarts.log 记录拉起时间戳:300 秒窗口内最多 5 次,超了就放弃。
// 全程只用 Win32 API(与 crashFilter 约束一致:绝不碰 CRT 堆,防死锁)。
static volatile LONG g_bridgeLive = 0;   // 桥服务真正跑起来后才允许自愈拉起

static void restartsLogPath(wchar_t* out, size_t cap) {
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap) { lstrcpyW(out, L"restarts.log"); return; }
    wchar_t* slash = out + n;
    while (slash > out && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
    lstrcpyW(slash, L"restarts.log");
}

static int appendU64(char* out, int pos, ULONGLONG v) {
    char tmp[24]; int t = 0;
    do { tmp[t++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (t) out[pos++] = tmp[--t];
    out[pos++] = '\n';
    return pos;
}

// 记录本次拉起并判断是否仍在允许窗口内;返回 false 表示疑似死循环,放弃拉起
static bool crashLoopAllowed() {
    const ULONGLONG windowMs = 300000;   // 5 分钟窗口
    const int maxRestarts = 5;
    ULONGLONG now = GetTickCount64();
    wchar_t path[MAX_PATH];
    restartsLogPath(path, MAX_PATH);
    char buf[512] = {0};
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD rd = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(h);
    }
    ULONGLONG keep[16]; int cnt = 0;
    for (char* s = buf; *s && cnt < 16; ) {
        while (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        ULONGLONG v = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (ULONGLONG)(*s - '0'); ++s; ++digits; }
        if (digits && now - v < windowMs) keep[cnt++] = v;   // 只保留窗口内的
        while (*s && *s != '\n') ++s;
    }
    if (cnt >= maxRestarts) return false;
    char out[512]; int pos = 0;
    for (int i = 0; i < cnt; ++i) pos = appendU64(out, pos, keep[i]);
    pos = appendU64(out, pos, now);
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, out, (DWORD)pos, &wr, nullptr);
        CloseHandle(h);
    }
    return true;
}

static void relaunchSelf() {
    if (!g_bridgeLive) return;             // 启动早期崩溃:不拉起,让问题暴露
    if (!crashLoopAllowed()) return;       // 死循环保护:窗口内次数用尽
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t cmd[MAX_PATH + 32];
    lstrcpyW(cmd, L"\"");
    lstrcatW(cmd, exe);
    lstrcatW(cmd, L"\" --respawn 3000");   // 等旧实例(互斥锁/静音恢复)完全退出
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // CREATE_NO_WINDOW:自愈拉起不弹黑框(与 start_hidden.vbs 静默部署一致)
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    char buf[768];
    void* ret = ep->ContextRecord && ep->ContextRecord->Rsp
                ? *(void**)ep->ContextRecord->Rsp : nullptr;   // 栈顶返回地址(空指针 CALL 的调用点)
    char mRet[128], mRip[128], mAddr[128];
    resolveModule(ret, mRet, sizeof(mRet));
    resolveModule(ep->ContextRecord ? (void*)ep->ContextRecord->Rip : nullptr, mRip, sizeof(mRip));
    resolveModule(ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr,
                  mAddr, sizeof(mAddr));
    // 过滤器运行于崩溃线程：tid + 身份 + TLS 标签共同回答「哪个线程在哪炸的」；
    // t=GetTickCount64 时间戳可与 stdout 日志的统计行(每 2 秒)对时
    DWORD tid = GetCurrentThreadId();
    int len = wsprintfA(buf,
                        "t=%lu tid=%lu(%s) where=%s code=0x%08X addr=%p(%s) rip=%p(%s) ret=%p(%s)\n",
                        (unsigned long)GetTickCount64(),
                        tid, tidName(tid),
                        g_where,
                        (unsigned)(ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0),
                        ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr, mAddr,
                        ep->ContextRecord ? (void*)ep->ContextRecord->Rip : nullptr, mRip,
                        ret, mRet);
    wchar_t cpath[MAX_PATH];
    crashLogPath(cpath, MAX_PATH);
    HANDLE h = CreateFileW(cpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, buf, (DWORD)len, &w, nullptr);
        CloseHandle(h);
    }
    relaunchSelf();                      // 崩溃自愈:拉起新实例接管(带防死循环)
    return EXCEPTION_EXECUTE_HANDLER;   // 仍让系统走默认崩溃流程(WER)
}

// 分数重采样器离线自检：正弦信号下验证直通精确性、速率偏移失真与饥饿行为。
// 与真链路共用同一 FractionalResampler 实现——数值过关才允许上线。
static int resamplerSelfTest() {
    const double Fs = 44100.0;
    const double f = 1000.0;
    const size_t need = 256;                    // 帧/回调（典型 ASIO 缓冲）
    const size_t outFrames = 441000;            // 10 秒输出
    const size_t inFrames = (size_t)((double)outFrames * 1.002) + need;
    std::vector<float> in(inFrames * 2);
    for (size_t i = 0; i < inFrames; ++i) {
        float v = 0.5f * (float)sin(2.0 * 3.14159265358979323846 * f * (double)i / Fs);
        in[i * 2] = v; in[i * 2 + 1] = v;
    }
    std::vector<float> out(need * 2);
    FractionalResampler rs;
    rs.setup(need);
    bool allPass = true;

    // 两种质量档都验证：0=线性（低延迟），32=sinc（高精度）
    for (int mode : {0, 32}) {
        rs.taps = mode;
        const char* tag = mode == 0 ? "线性" : "sinc";
        bool modePass = true;

        // 测试 1：ratio=1.0 直通——与理想正弦对比（sinc 有 15 帧固有群延迟，
        // 逐位比对会错位，故统一用理想正弦参考）
        {
            rs.reset(); rs.ratio = 1.0;
            size_t ip = 0, om = 0;
            double maxE = 0.0, sumSq = 0.0; size_t n = 0;
            while (ip < inFrames) {
                size_t want = rs.wantIn(need);
                if (ip + want > inFrames) break;
                rs.process(&in[ip * 2], want, out.data(), need);
                for (size_t j = 0; j < need; ++j) {
                    double ref = 0.5 * sin(2.0 * 3.14159265358979323846 * f * (double)(om + j) / Fs);
                    double e0 = fabs((double)out[j * 2] - ref);
                    double e1 = fabs((double)out[j * 2 + 1] - ref);
                    if (e0 > maxE) maxE = e0;
                    if (e1 > maxE) maxE = e1;
                    sumSq += e0 * e0 + e1 * e1; n += 2;
                }
                om += need; ip += want;
            }
            double rms = sqrt(sumSq / n);
            bool pass = rms < 1e-3 && maxE < 5e-3;
            printf("[自检1-%s] ratio=1.0 直通: 最大误差 %.2e, RMS %.2e, %zu 采样 => %s\n",
                   tag, maxE, rms, n, pass ? "PASS" : "FAIL");
            modePass &= pass;
        }

        // 测试 2：ratio=1.0002 与理想 1000.2Hz 正弦对比（输出帧 m ↔ 输入帧位 m×ratio）
        {
            rs.reset(); rs.ratio = 1.0002;
            size_t ip = 0, om = 0;
            double maxE = 0.0, sumSq = 0.0; size_t n = 0;
            while (ip < inFrames) {
                size_t want = rs.wantIn(need);
                if (ip + want > inFrames) break;
                rs.process(&in[ip * 2], want, out.data(), need);
                for (size_t j = 0; j < need; ++j) {
                    double ref = 0.5 * sin(2.0 * 3.14159265358979323846 * f * 1.0002
                                           * (double)(om + j) / Fs);
                    double e0 = fabs((double)out[j * 2] - ref);
                    double e1 = fabs((double)out[j * 2 + 1] - ref);
                    if (e0 > maxE) maxE = e0;
                    if (e1 > maxE) maxE = e1;
                    sumSq += e0 * e0 + e1 * e1; n += 2;
                }
                om += need; ip += want;
            }
            double rms = sqrt(sumSq / n);
            bool pass = rms < 1e-3 && maxE < 5e-3;
            printf("[自检2-%s] ratio=1.0002 理想对比: 最大误差 %.2e, RMS %.2e, %zu 采样 => %s\n",
                   tag, maxE, rms, n, pass ? "PASS" : "FAIL");
            modePass &= pass;
        }

        // 测试 3：比率摆动（模拟控制器行为）——输出有界、无 NaN
        {
            rs.reset();
            size_t ip = 0;
            bool bounded = true;
            for (size_t k = 0; k < 5000; ++k) {
                rs.ratio = ((k / 1000) % 2 == 0) ? 1.0003 : 0.9997;
                size_t want = rs.wantIn(need);
                if (ip + want > inFrames) ip = 0;
                rs.process(&in[ip * 2], want, out.data(), need);
                for (size_t j = 0; j < need * 2; ++j)
                    if (fabsf(out[j]) > 0.51f || out[j] != out[j]) bounded = false;
                ip += want;
            }
            printf("[自检3-%s] 比率摆动 0.9997~1.0003: %s\n", tag, bounded ? "PASS" : "FAIL");
            modePass &= bounded;
        }

        // 测试 4：饥饿钳制（输入 5 帧 / 需求 256 帧）——保持无 NaN、有界
        {
            rs.reset(); rs.ratio = 1.0;
            float tiny[10];
            for (int i = 0; i < 10; ++i) tiny[i] = 0.1f;
            bool ok = true;
            for (size_t k = 0; k < 100; ++k) {
                rs.process(tiny, 5, out.data(), need);
                for (size_t j = 0; j < need * 2; ++j)
                    if (fabsf(out[j]) > 0.11f || out[j] != out[j]) ok = false;
            }
            printf("[自检4-%s] 饥饿输入(5/256帧): %s\n", tag, ok ? "PASS" : "FAIL");
            modePass &= ok;
        }

        allPass &= modePass;
    }

    printf(allPass ? "== 重采样器自检全部通过 ==\n" : "== 自检存在失败项 ==\n");
    return allPass ? 0 : 1;
}

// 300B 电子管染色自检（--tube-test）：
// 用正弦过 tube300B，数值量测 2 次/3 次谐波，验证：
//   ① 2 次谐波远大于 3 次(偶次为主 = 暖)
//   ② 2 次谐波随幅度线性增长(动态温暖)
//   ③ 无直流(去直流成立)
static int tubeSelfTest() {
    int fails = 0;
    const int N = 65536;
    const int K = 1024;      // 测试音周期数(基波 ≈ 689 Hz @44.1k,远高于 DC blocker 5Hz)
    const int WARMUP = 8000; // DC blocker 收敛预热(约 180ms)
    const double twoPi = 6.28318530717958647692;
    const double warmth = 0.5;
    printf("[染色自检] TubeWarmth warmth=%.2f (含 DC blocker, 基波 %d 周期)\n", warmth, K);
    double prev2 = -1.0;
    double amps[] = {0.1, 0.3, 0.6};
    for (int k = 0; k < 3; ++k) {
        double A = amps[k];
        TubeWarmth tw;
        double dc = 0, f1 = 0, f2 = 0, f3 = 0;
        int cnt = 0;
        for (int i = 0; i < WARMUP + N; ++i) {
            double t = twoPi * K * i / N;
            float x = (float)(A * sin(t));
            float y = tw.process(x, (float)warmth);
            if (i < WARMUP) continue;   // 跳过 DC blocker 预热
            dc += y; f1 += y * (float)sin(t);
            f2 += y * (float)cos(2.0 * t); f3 += y * (float)sin(3.0 * t);
            ++cnt;
        }
        dc /= cnt; f1 = 2.0 * f1 / cnt; f2 = 2.0 * f2 / cnt; f3 = 2.0 * f3 / cnt;
        double r2 = fabs(f2 / f1), r3 = fabs(f3 / f1);
        printf("  A=%.2f: 基波=%.4f 2次=%.2f%% 3次=%.4f%% 直流=%.7f\n",
               A, f1, r2 * 100.0, r3 * 100.0, dc);
        if (r2 < r3 * 5.0) { printf("  => FAIL: 2次未明显大于3次\n"); fails++; }
        if (k > 0 && r2 < prev2 * 1.2) { printf("  => FAIL: 2次未随幅度增长\n"); fails++; }
        if (fabs(dc) > 0.001) { printf("  => FAIL: 直流残留过大\n"); fails++; }
        prev2 = r2;
    }
    printf(fails == 0 ? "== 染色自检通过 ==\n" : "== 染色自检存在 %d 项失败 ==\n", fails);
    return fails;
}

// 速率锁 + 水位闭环联合仿真（--pll-test）：
// 用真实的 FractionalResampler + RateLock 与主循环相同的闭环公式，
// 模拟采集时钟 44100×(1+25ppm) 投递、ASIO 时钟 44100×(1-15ppm) 消费，
// 运行 1200 秒；验证 ratio 收敛到理论时钟比、水位有界、稳态零欠载。
static int pllSelfTest() {
    const double rc = 44100.0 * (1.0 + 25e-6);   // 采集时钟
    const double ra = 44100.0 * (1.0 - 15e-6);   // ASIO 时钟
    const double truth = rc / ra;                 // 理论 ratio ≈ 1.00004
    const size_t need = 256;                      // 帧/回调
    const double qpcFreq = 1e7;                   // 仿真 QPC 10MHz

    FractionalResampler rs;
    rs.setup(need);
    RateLock lock;

    const size_t chunk = 441 * 2;                       // 采集块采样数（~10ms×2ch）
    const double capPeriod = (double)(chunk / 2) / rc;   // 采集块间隔（秒）
    const double asioPeriod = (double)need / ra;         // ASIO 回调间隔（秒）

    double simT = 0.0;
    double nextCap = 0.0, nextAsio = 0.0, nextTick = 0.0;
    uint64_t inTot = 0, outTot = 0, capCb = 0, asioCb = 0;
    double ring = 4096.0;      // 采样（初始预填充 = 8×512 目标水位）
    const double setpoint = 4096.0;
    double wmAvg = 4096.0;
    double ratio = 1.0, ratioBase = 1.0;
    uint64_t underruns = 0;
    double minRing = ring, maxRing = ring;
    double sumWarm = 0.0; uint64_t nWarm = 0, tickCount = 0;
    uint64_t capEvents = 0, asioEvents = 0, conSum = 0, wantSum = 0;

    std::vector<float> feed(need * 2, 0.5f);
    std::vector<float> dst(need * 2);

    // 均匀随机抖动（LCG，零均值无偏）。关键：抖动加在固定设备时钟栅格上
    // （t = n×P + εn，εn 独立同分布），而非累加在周期上——真实设备的
    // 事件围绕晶体时钟栅格抖动，相位不会随机游走（累加式抖动会让
    // 300 秒窗口端点漂移 ±20ms，把速率测量噪声放大到 ±70ppm）。
    uint32_t lcg = 12345u;
    auto rnd01 = [&]() { lcg = lcg * 1664525u + 1013904223u; return (double)(lcg >> 8) / 16777216.0; };
    double capGrid = 0.0, asioGrid = 0.0;
    // 事件驱动仿真（时间精确，无步进量化偏差——步进式会引入
    // 每事件 +0.05ms 的系统性时延，把实测速率压低约 0.5%）
    while (simT < 1200.0) {
        double next = nextCap < nextAsio ? nextCap : nextAsio;
        if (nextTick < next) next = nextTick;
        if (next >= 1200.0) break;
        simT = next;
        // 采集投递（±1ms 均匀随机抖动模拟 WASAPI 周期抖动）
        if (simT >= nextCap) {
            ring += (double)chunk;
            inTot += chunk;
            capEvents++;
            if ((++capCb) % 25 == 0)
                lock.pushIn((uint64_t)(simT * qpcFreq), inTot);
            nextCap = capGrid + (rnd01() * 2.0 - 1.0) * 0.001;
            capGrid += capPeriod;
        }
        // ASIO 消费（±0.5ms 均匀随机抖动）
        if (simT >= nextAsio) {
            rs.ratio = ratio;
            size_t want = rs.wantIn(need);
            size_t wantS = want * 2;
            size_t avail = ring >= (double)wantS ? wantS : (size_t)ring;
            if (avail < wantS) underruns++;
            ring -= (double)avail;
            conSum += avail;
            wantSum += want;
            asioEvents++;
            rs.process(feed.data(), avail / 2, dst.data(), need);
            outTot += (uint64_t)need * 2;
            if ((++asioCb) % 25 == 0)
                lock.pushOut((uint64_t)(simT * qpcFreq), outTot);
            nextAsio = asioGrid + (rnd01() * 2.0 - 1.0) * 0.0005;
            asioGrid += asioPeriod;
        }
        // 控制节拍（2 秒，与主循环相同公式）
        if (simT >= nextTick) {
            tickCount++;
            wmAvg += (ring - wmAvg) * 0.1;
            double ih = 0.0, oh = 0.0;
            if (lock.update(qpcFreq, &ih, &oh) && ih > 1000.0 && oh > 1000.0) {
                double b = ih / oh;
                if (tickCount <= 4 || tickCount % 150 == 0)
                    printf("[调试]   锁: in=%.2f out=%.2f base=%.7f err=%.0f\n",
                           ih, oh, b, wmAvg - setpoint);
                if (b < 0.999) b = 0.999;
                if (b > 1.001) b = 1.001;
                ratioBase = b;
            }
            double err = wmAvg - setpoint;
            double cap = (err > 1024.0 || err < -1024.0) ? 0.001 : 0.0003;
            double r = ratioBase + err * 5e-7;
            if (r > ratioBase + cap) r = ratioBase + cap;
            if (r < ratioBase - cap) r = ratioBase - cap;
            ratio = r;
            nextTick += 2.0;
        }
        if (ring > maxRing) maxRing = ring;
        if (ring < minRing) minRing = ring;
        if (simT >= 600.0) { sumWarm += ratio; nWarm++; }
    }

    bool pass = true;
    double avgWarm = nWarm ? sumWarm / (double)nWarm : 1.0;
    printf("[速率锁] 理论 ratio = %.7f (采集 +25ppm / ASIO -15ppm)\n", truth);
    printf("[速率锁] 600~1200 秒平均 ratio = %.7f (误差 %+.2f ppm)\n",
           avgWarm, (avgWarm - truth) * 1e6);
    if (fabs(avgWarm - truth) * 1e6 > 20.0) { printf("[速率锁] 收敛超差 => FAIL\n"); pass = false; }
    printf("[速率锁] 水位范围 %.0f ~ %.0f (目标 %.0f), 欠载 %llu, 节拍 %llu\n",
           minRing, maxRing, setpoint, underruns, tickCount);
    printf("[速率锁] 采集事件 %llu × %zu = %llu 采样, ASIO 事件 %llu, 消费 %llu 采样, 平均 want %.2f\n",
           capEvents, chunk, capEvents * (uint64_t)chunk, asioEvents, conSum,
           asioEvents ? (double)wantSum / (double)asioEvents : 0.0);
    if (minRing < 1000.0 || maxRing > 20000.0) { printf("[速率锁] 水位越界 => FAIL\n"); pass = false; }
    if (underruns != 0) { printf("[速率锁] 稳态存在欠载 => FAIL\n"); pass = false; }
    printf(pass ? "== 速率锁仿真自检通过 ==\n" : "== 速率锁仿真自检失败 ==\n");
    return pass ? 0 : 1;
}

// 进程回环采集数值自检（--capture-test）：
//   本进程渲染 997Hz 正弦（优先送入 CABLE Input 静默端点，不打扰耳朵；
//   无则用默认设备并降幅）→ 按自身 PID 进程回环采集 → 幅度/频率比对。
static int captureSelfTest() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("[Bridge 自检] CoInitializeEx 失败 0x%08lX\n", (unsigned)hr); return 1; }

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        printf("[Bridge 自检] 创建 MMDeviceEnumerator 失败\n"); CoUninitialize(); return 1;
    }

    // 0) 诊断基线：用同一异步激活机制激活默认渲染设备（默认参数）
    {
        class DiagHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
        public:
            DiagHandler() : ref_(1), event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
            ~DiagHandler() { if (event_) CloseHandle(event_); }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
                if (riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
                    riid == __uuidof(IAgileObject) || riid == __uuidof(IUnknown)) {
                    *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); AddRef(); return S_OK;
                }
                *ppv = nullptr; return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
            ULONG STDMETHODCALLTYPE Release() override {
                ULONG r = InterlockedDecrement(&ref_);
                if (r == 0) delete this;
                return r;
            }
            HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
                IUnknown* unk = nullptr;
                HRESULT r = E_UNEXPECTED;
                if (SUCCEEDED(op->GetActivateResult(&r, &unk))) { hr_ = r; if (unk) unk->Release(); }
                else hr_ = r;
                SetEvent(event_);
                return S_OK;
            }
            HRESULT Wait(DWORD ms) { WaitForSingleObject(event_, ms); return hr_; }
            LONG ref_ = 1;
            HANDLE event_;
            HRESULT hr_ = E_UNEXPECTED;
        };
        IMMDevice* defDev = nullptr;
        LPWSTR devId = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &defDev)) &&
            SUCCEEDED(defDev->GetId(&devId))) {
            PROPVARIANT empty;
            PropVariantInit(&empty);   // VT_EMPTY = 默认激活
            DiagHandler* dh = new DiagHandler();
            IActivateAudioInterfaceAsyncOperation* dop = nullptr;
            HRESULT dhAct = ActivateAudioInterfaceAsync(devId, __uuidof(IAudioClient), &empty, dh, &dop);
            HRESULT dhRes = dh->Wait(5000);
            printf("[Bridge 自检] 基线(异步激活默认设备): 同步 0x%08lX / 完成 0x%08lX\n",
                   (unsigned long)dhAct, (unsigned long)dhRes);
            if (dop) dop->Release();
            dh->Release();
            CoTaskMemFree(devId);
            defDev->Release();
        } else {
            printf("[Bridge 自检] 基线激活: 无法获取默认设备 ID\n");
        }
    }

    // 1) 选渲染端点：优先 CABLE Input（静默目标），失败则逐个尝试其余
    //    活动渲染端点（低音量）——回环自采只关心本进程音频，不挑设备
    IMMDevice* renderDev = nullptr;
    std::vector<IMMDevice*> candidates;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0; coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            IPropertyStore* store = nullptr;
            std::wstring name;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
                PROPVARIANT v; PropVariantInit(&v);
                static const PROPERTYKEY kName = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
                if (SUCCEEDED(store->GetValue(kName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
                PropVariantClear(&v); store->Release();
            }
            if (wcsstr(name.c_str(), L"CABLE Input")) candidates.insert(candidates.begin(), dev);
            else candidates.push_back(dev);
        }
        coll->Release();
    }
    bool silentTarget = false;
    IAudioClient* rc = nullptr;
    uint32_t mixRate = 44100;
    size_t candIdx = 0;
    for (; candIdx < candidates.size(); ++candIdx) {
        renderDev = candidates[candIdx];
        rc = nullptr;
        if (FAILED(renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&rc))) continue;
        WAVEFORMATEX* mf = nullptr;
        if (SUCCEEDED(rc->GetMixFormat(&mf)) && mf) {
            hr = rc->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mf, nullptr);
            mixRate = mf->nSamplesPerSec;
            CoTaskMemFree(mf);
        } else {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) break;
        rc->Release();
        rc = nullptr;
    }
    for (size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i] != renderDev) candidates[i]->Release();
    if (!rc || !renderDev) {
        printf("[Bridge 自检] 没有任何可用的渲染端点（音频服务/设备状态异常？）\n");
        en->Release(); CoUninitialize(); return 1;
    }
    silentTarget = (candIdx == 0);
    {
        IPropertyStore* store = nullptr;
        std::wstring devName = L"?";
        if (SUCCEEDED(renderDev->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT v; PropVariantInit(&v);
            static const PROPERTYKEY kName = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}}, 14};
            if (SUCCEEDED(store->GetValue(kName, &v)) && v.vt == VT_LPWSTR) devName = v.pwszVal;
            PropVariantClear(&v); store->Release();
        }
        printf("[Bridge 自检] 渲染端点: %ls\n", devName.c_str());
    }
    IAudioRenderClient* rr = nullptr;
    rc->GetService(__uuidof(IAudioRenderClient), (void**)&rr);
    HANDLE rEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    rc->SetEventHandle(rEvt);

    const double fTest = 997.0;
    const double amp = silentTarget ? 0.5 : 0.08;
    printf("[Bridge 自检] 渲染 %g Hz 正弦 @ %u Hz (振幅 %.2f, %s) → 按 PID 回环自采\n",
           fTest, mixRate, amp, silentTarget ? "静默目标 CABLE Input" : "默认设备(低音量)");

    std::atomic<bool> renderRun{true};
    std::thread renderThread([&] {
        UINT32 bufFrames = 0; rc->GetBufferSize(&bufFrames);
        double phase = 0.0;
        rc->Start();
        while (renderRun.load()) {
            if (WaitForSingleObject(rEvt, 100) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0; rc->GetCurrentPadding(&pad);
            UINT32 avail = bufFrames - pad;
            if (avail == 0) continue;
            BYTE* p = nullptr;
            if (SUCCEEDED(rr->GetBuffer(avail, &p))) {
                float* fp = (float*)p;
                for (UINT32 i = 0; i < avail; ++i) {
                    float s = (float)(amp * sin(phase));
                    phase += 2.0 * 3.14159265358979323846 * fTest / mixRate;
                    if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                    fp[i * 2] = s; fp[i * 2 + 1] = s;
                }
                rr->ReleaseBuffer(avail, 0);
            }
        }
        rc->Stop();
    });
    Sleep(400);

    // 2) 进程回环采集本进程（激活失败则多 PID 诊断：本进程 / explorer / audiodg）
    std::vector<float> capBuf;
    WasapiProcessCapture pc;
    std::string err;
    DWORD diagPids[3] = { GetCurrentProcessId(), 0, 0 };
    {
        // 找 explorer / audiodg 的 PID 作对照
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0 && !diagPids[1]) diagPids[1] = pe.th32ProcessID;
                    if (_wcsicmp(pe.szExeFile, L"audiodg.exe") == 0 && !diagPids[2]) diagPids[2] = pe.th32ProcessID;
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }
    bool opened = false;
    for (int di = 0; di < 3 && !opened; ++di) {
        if (!diagPids[di]) continue;
        opened = pc.open(diagPids[di], [&](const float* d, uint32_t frames) {
                capBuf.insert(capBuf.end(), d, d + (size_t)frames * 2);
            }, err);
        if (!opened && di == 0) {
            printf("[Bridge 自检] 本进程激活失败(%s)，多 PID 诊断...\n", err.c_str());
            opened = false;
        }
    }
    if (!opened) {
        printf("[Bridge 自检] 全部 PID 激活失败: %s（音频服务状态异常？建议重启系统）\n", err.c_str());
        renderRun.store(false); renderThread.join();
        CloseHandle(rEvt); rc->Release(); renderDev->Release(); en->Release(); CoUninitialize();
        return 1;
    }
    const uint32_t capRate = pc.format().sampleRate;
    Sleep(3000);
    pc.close();
    renderRun.store(false);
    renderThread.join();

    {
        FILE* f = nullptr;
        if (fopen_s(&f, "captest.raw", "wb") == 0 && f) {
            fwrite(capBuf.data(), sizeof(float), capBuf.size(), f);
            fclose(f);
            printf("[Bridge 自检] 已转储 %zu 采样 -> captest.raw（当前目录）\n", capBuf.size());
        }
    }

    // 3) 分析：RMS / 峰值 / 零交叉频率（只用左声道，步长 2）
    //    跳过前 0.5s：回环流启动时引擎有 ~30ms 预卷静音，±1LSB 量化闪烁
    //    会产生假过零，污染频率估计
    double sumSq = 0.0, mx = 0.0;
    size_t zc = 0, nCh = 0;
    size_t start = (size_t)capRate / 2 * 2;   // 0.5s
    for (size_t i = start; i + 1 < capBuf.size(); i += 2) {
        double v = capBuf[i];
        sumSq += v * v;
        if (fabs(v) > mx) mx = fabs(v);
        if (i >= 2) {
            double prev = capBuf[i - 2];
            if (prev < 0.0 && v >= 0.0) ++zc;
        }
        ++nCh;
    }
    double rms = nCh ? sqrt(sumSq / nCh) : 0.0;
    double freq = nCh ? (double)zc * (double)capRate / (double)nCh : 0.0;
    const double expectRms = amp / 1.41421356237;
    printf("[Bridge 自检] 采集 %u Hz / %u 通道, %zu 采样: RMS %.4f (期望 %.4f), 峰值 %.3f, 频率 %.2f Hz (期望 %.1f)\n",
           capRate, pc.format().channels, nCh, rms, expectRms, mx, freq, fTest);

    bool pass = true;
    if (capRate != mixRate) { printf("[Bridge 自检] 速率不一致 => FAIL\n"); pass = false; }
    if (rms < expectRms * 0.8 || rms > expectRms * 1.2) { printf("[Bridge 自检] 幅度超差 => FAIL\n"); pass = false; }
    if (mx < amp * 0.85) { printf("[Bridge 自检] 峰值过低 => FAIL\n"); pass = false; }
    if (fabs(freq - fTest) > 5.0) { printf("[Bridge 自检] 频率超差 => FAIL\n"); pass = false; }

    // 4) 端点静音语义验证（tap→redirect 机制）：数值实验已证明回环取点在
    //    「会话静音之后、终点主音量之前」——会话级静音会哑掉捕获（不可用），
    //    终点主音量静音既消除双重声又完全不影响捕获。此阶段验证：
    //    MuteTargetEndpoints 静音 → 回环捕获 RMS 不变 → RestoreEndpointMutes 恢复。
    {
        printf("[Bridge 自检] 阶段4：静音目标端点后回环捕获必须不受影响...\n");
        std::atomic<bool> run2{true};
        std::thread render2([&] {
            UINT32 bf = 0; rc->GetBufferSize(&bf);
            double phase = 0.0;
            rc->Start();
            while (run2.load()) {
                if (WaitForSingleObject(rEvt, 100) != WAIT_OBJECT_0) continue;
                UINT32 pad = 0; rc->GetCurrentPadding(&pad);
                UINT32 avail = bf - pad;
                if (avail == 0) continue;
                BYTE* p = nullptr;
                if (SUCCEEDED(rr->GetBuffer(avail, &p))) {
                    float* fp = (float*)p;
                    for (UINT32 i = 0; i < avail; ++i) {
                        float s = (float)(amp * sin(phase));
                        phase += 2.0 * 3.14159265358979323846 * fTest / mixRate;
                        if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                        fp[i * 2] = s; fp[i * 2 + 1] = s;
                    }
                    rr->ReleaseBuffer(avail, 0);
                }
            }
            rc->Stop();
        });
        Sleep(400);
        // 先读起点静音态：端点若已被外部静音（如用户混音器操作），
        // MuteTargetEndpoints 会正确地不做任何事（0 新增）——不算失败，
        // 恢复断言也应以「保持外部静音态」为准
        bool wasMutedBefore = false;
        {
            IAudioEndpointVolume* ev = nullptr;
            if (SUCCEEDED(renderDev->Activate(__uuidof(IAudioEndpointVolume),
                                              CLSCTX_ALL, nullptr, (void**)&ev)) && ev) {
                BOOL m = FALSE;
                ev->GetMute(&m);
                wasMutedBefore = m != FALSE;
                ev->Release();
            }
        }
        std::vector<EndpointMuteEntry> mutes = MuteTargetEndpoints(GetCurrentProcessId());
        printf("[Bridge 自检] 已静音目标端点 %zu 个（外部预静音态: %s）\n",
               mutes.size(), wasMutedBefore ? "是" : "否");
        bool ok4 = true;
        if (!wasMutedBefore && mutes.empty()) {
            printf("[Bridge 自检] 端点未静音却也静不到 => FAIL\n");
            ok4 = false;
        }
        {
            IAudioEndpointVolume* ev = nullptr;
            if (SUCCEEDED(renderDev->Activate(__uuidof(IAudioEndpointVolume),
                                              CLSCTX_ALL, nullptr, (void**)&ev)) && ev) {
                BOOL m = FALSE;
                ev->GetMute(&m);
                printf("[Bridge 自检] 终点静音态检查: %s（期望 TRUE）%s\n",
                       m ? "TRUE" : "FALSE", m ? "✓" : "✗");
                if (!m) ok4 = false;
                ev->Release();
            }
        }

        std::vector<float> cap2;
        WasapiProcessCapture pc2;
        std::string err2;
        if (!pc2.open(GetCurrentProcessId(), [&](const float* d, uint32_t frames) {
                cap2.insert(cap2.end(), d, d + (size_t)frames * 2);
            }, err2)) {
            printf("[Bridge 自检] 阶段4捕获打开失败: %s => FAIL\n", err2.c_str());
            ok4 = false;
        } else {
            Sleep(1500);
            pc2.close();
            double sumSq2 = 0.0;
            size_t n2 = 0;
            size_t start2 = (size_t)pc2.format().sampleRate / 2 * 2;
            for (size_t i = start2; i + 1 < cap2.size(); i += 2) {
                double v = cap2[i];
                sumSq2 += v * v;
                ++n2;
            }
            double rms2 = n2 ? sqrt(sumSq2 / n2) : 0.0;
            bool inRange2 = rms2 > expectRms * 0.8 && rms2 < expectRms * 1.2;
            printf("[Bridge 自检] 终点静音后捕获 RMS %.4f（期望 %.4f）%s\n",
                   rms2, expectRms,
                   inRange2 ? "=> 回环取点在终点主音量之前 ✓（redirect 机制成立）"
                            : "=> 回环取点在终点主音量之后 ✗");
            if (!inRange2) ok4 = false;
        }

        RestoreEndpointMutes(mutes);
        {
            IAudioEndpointVolume* ev = nullptr;
            if (SUCCEEDED(renderDev->Activate(__uuidof(IAudioEndpointVolume),
                                              CLSCTX_ALL, nullptr, (void**)&ev)) && ev) {
                BOOL m = TRUE;
                ev->GetMute(&m);
                bool expectAfter = wasMutedBefore;   // 我们没动过的外部静音应保持
                printf("[Bridge 自检] 恢复后终点静音态: %s（期望 %s）%s\n",
                       m ? "TRUE" : "FALSE", expectAfter ? "TRUE" : "FALSE",
                       ((m != FALSE) == expectAfter) ? "✓" : "✗");
                if ((m != FALSE) != expectAfter) ok4 = false;
                ev->Release();
            }
        }

        run2.store(false);
        render2.join();
        if (!ok4) pass = false;
    }

    CloseHandle(rEvt);
    rc->Release();
    renderDev->Release();
    en->Release();
    CoUninitialize();
    printf(pass ? "== 进程回环采集自检通过 ==\n" : "== 进程回环采集自检失败 ==\n");
    return pass ? 0 : 1;
}

// CAudioLimiter 位置实测（--limiter-test）：
//   渲染幅度 1.5（超过满刻度 1.0）的 50Hz 正弦 → 进程回环自采 → 看采集峰值。
//   峰值≈1.5 → 取点在限幅器之前（桥绕过端点限幅器）
//   峰值被压到≈1.0 → 取点在限幅器之后（桥继承端点限幅）
//   渲染端点在测试期间静音，不打扰耳朵。
static int limiterSelfTest() {
    using Microsoft::WRL::ComPtr;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("[限幅器测试] CoInitializeEx 失败 0x%08lX\n", (unsigned)hr); return 1; }

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)en.GetAddressOf()))) {
        printf("[限幅器测试] 创建 MMDeviceEnumerator 失败\n"); CoUninitialize(); return 1;
    }

    // 1) 选第一个活动渲染端点
    ComPtr<IMMDevice> renderDev;
    {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, coll.GetAddressOf())) || !coll) {
            printf("[限幅器测试] 无活动渲染端点\n"); CoUninitialize(); return 1;
        }
        UINT count = 0;
        coll->GetCount(&count);
        if (count == 0) { printf("[限幅器测试] 无活动渲染端点\n"); CoUninitialize(); return 1; }
        coll->Item(0, renderDev.GetAddressOf());
    }

    // 2) 静音该端点（避免 1.5 幅度大信号炸耳朵）
    ComPtr<IAudioEndpointVolume> ev;
    BOOL prevMute = FALSE;
    if (SUCCEEDED(renderDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                      nullptr, (void**)ev.GetAddressOf())) && ev) {
        ev->GetMute(&prevMute);
        ev->SetMute(TRUE, nullptr);
        printf("[限幅器测试] 已静音测试端点\n");
    }
    auto restoreMute = [&] { if (ev) ev->SetMute(prevMute, nullptr); };

    // 3) 渲染客户端（共享模式，需 float32 才能写 >1.0）
    ComPtr<IAudioClient> rc;
    WAVEFORMATEX* mf = nullptr;
    bool isFloat = false;
    if (SUCCEEDED(renderDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)rc.GetAddressOf())) &&
        SUCCEEDED(rc->GetMixFormat(&mf)) && mf) {
        if (mf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        } else if (mf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const WAVEFORMATEXTENSIBLE* wfe = (const WAVEFORMATEXTENSIBLE*)mf;
            static const GUID kFloatSub = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
            isFloat = IsEqualGUID(wfe->SubFormat, kFloatSub);
        }
    }
    if (!isFloat) {
        printf("[限幅器测试] 渲染端点混音格式非 float32（tag=%u），无法渲染 >1.0 信号\n",
               mf ? mf->wFormatTag : 0);
        if (mf) CoTaskMemFree(mf);
        restoreMute(); CoUninitialize();
        return 1;
    }
    const uint32_t rate = mf->nSamplesPerSec;
    const uint16_t ch = mf->nChannels;
    hr = rc->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        0, 0, mf, nullptr);
    CoTaskMemFree(mf);
    if (FAILED(hr)) {
        printf("[限幅器测试] 渲染 Initialize 失败 0x%08lX\n", (unsigned)hr);
        restoreMute(); CoUninitialize();
        return 1;
    }
    ComPtr<IAudioRenderClient> rr;
    rc->GetService(__uuidof(IAudioRenderClient), (void**)rr.GetAddressOf());
    HANDLE rEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    rc->SetEventHandle(rEvt);
    UINT32 bufFrames = 0;
    rc->GetBufferSize(&bufFrames);
    printf("[限幅器测试] 渲染 %u Hz / %u 通道 / float32，幅度 1.5 的 50Hz 正弦 → 按 PID 回环自采\n",
           rate, ch);

    const double amp = 1.5;
    const double f = 50.0;
    std::atomic<bool> run{true};
    std::thread renderThread([&] {
        double phase = 0.0;
        rc->Start();
        while (run.load()) {
            if (WaitForSingleObject(rEvt, 100) != WAIT_OBJECT_0) continue;
            UINT32 pad = 0;
            rc->GetCurrentPadding(&pad);
            UINT32 avail = bufFrames - pad;
            if (avail == 0) continue;
            BYTE* p = nullptr;
            if (SUCCEEDED(rr->GetBuffer(avail, &p))) {
                float* fp = (float*)p;
                for (UINT32 i = 0; i < avail; ++i) {
                    float s = (float)(amp * sin(phase));
                    phase += 2.0 * 3.14159265358979323846 * f / rate;
                    if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                    for (uint16_t c = 0; c < ch; ++c) fp[i * ch + c] = s;
                }
                rr->ReleaseBuffer(avail, 0);
            }
        }
        rc->Stop();
    });
    Sleep(400);

    // 4) 进程回环采集自身
    std::vector<float> capBuf;
    WasapiProcessCapture pc;
    std::string err;
    if (!pc.open(GetCurrentProcessId(), [&](const float* d, uint32_t frames) {
            capBuf.insert(capBuf.end(), d, d + (size_t)frames * pc.format().channels);
        }, err)) {
        printf("[限幅器测试] 回环采集打开失败: %s\n", err.c_str());
        run.store(false); renderThread.join();
        CloseHandle(rEvt);
        restoreMute(); CoUninitialize();
        return 1;
    }
    const uint32_t capRate = pc.format().sampleRate;
    const uint16_t capCh = pc.format().channels;
    Sleep(2000);
    pc.close();
    run.store(false);
    renderThread.join();

    // 5) 分析峰值（跳过前 0.5s 预滚）
    double peak = 0.0, sumSq = 0.0;
    size_t n = 0;
    size_t start = (size_t)capRate / 2 * capCh;
    for (size_t i = start; i < capBuf.size(); ++i) {
        double v = fabs((double)capBuf[i]);
        if (v > peak) peak = v;
        sumSq += (double)capBuf[i] * (double)capBuf[i];
        ++n;
    }
    double rms = n ? sqrt(sumSq / n) : 0.0;
    printf("[限幅器测试] 采集 %u Hz / %u 通道，%zu 采样：峰值 %.4f，RMS %.4f（渲染幅度 1.5，RMS 期望 %.4f）\n",
           capRate, capCh, n, peak, rms, amp / 1.41421356237);
    if (peak > 1.3) {
        printf("=> 结论：取点在端点限幅器之前——桥绕过 Windows 端点限幅器（大信号不被压限）\n");
    } else if (peak < 1.05) {
        printf("=> 结论：取点在端点限幅器之后——桥继承端点限幅（大信号被压到 ≈1.0）\n");
    } else {
        printf("=> 结论：中间态（峰值 %.4f），需复核\n", peak);
    }

    restoreMute();
    CloseHandle(rEvt);
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // --respawn MS:崩溃自愈内部参数——crashFilter 拉起的新实例先等旧实例
    // (单实例互斥锁/ASIO 句柄/端点静音恢复)完全退出再启动。等待后从
    // argv 移除该参数,不影响后续正常参数解析。
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--respawn") == 0) {
            Sleep(wcstoul(argv[i + 1], nullptr, 10));
            for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }
    // 单实例互斥锁:若已有桥实例在运行(如计划任务自动重启拉起的副本),
    // 立即退出,避免多个实例同时抢 ASIO 设备/端口/目标进程。
    // 注意:--list/--tone/--*-test 等诊断模式不需要互斥,放后面判断。
    HANDLE hSingle = CreateMutexW(nullptr, TRUE, L"Local\\asio_bridge_single_instance");
    bool singleInst = (hSingle && GetLastError() != ERROR_ALREADY_EXISTS);

    SetUnhandledExceptionFilter(crashFilter);   // 崩溃定位:写 crash.log
    g_tidMain = GetCurrentThreadId();           // 主线程身份注册（崩溃日志线程归属）

    SetConsoleOutputCP(65001);
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲，崩溃时也能看到输出
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    bool list = false, tone = false;
    bool crashTest = false;   // --crash-test:服务启动后故意空指针崩溃,验证自愈拉起链路
    bool showConsole = false;   // 默认后台隐藏控制台窗口；--show/--console 显式显示（调试用）
    std::string driver;       // 空=未指定(需控制台手动选择); --driver 显式指定 ASIO 驱动名
    double toneRate = 44100.0;
    long reqBuffer = 0;
    bool ditherFlag = true;   // TPDF 抖动默认开启；优先级:命令行 > 配置文件 > 默认
    bool ditherArgGiven = false;   // 命令行是否显式指定 --dither/--no-dither
    bool noMute = false;           // 诊断：跳过端点静音 hack（鉴别 RME 驱动崩溃触发源）

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--list") list = true;
        else if (a == L"--tone") tone = true;
        else if (a == L"--driver" && i + 1 < argc) driver = ws2s(argv[++i]);
        else if (a == L"--rate" && i + 1 < argc) toneRate = _wtof(argv[++i]);
        else if (a == L"--buffer" && i + 1 < argc) reqBuffer = _wtol(argv[++i]);
        else if (a == L"--dither") { ditherFlag = true; ditherArgGiven = true; }
        else if (a == L"--no-dither") { ditherFlag = false; ditherArgGiven = true; }
        else if (a == L"--no-mute") noMute = true;
        else if (a == L"--hidden") { /* 默认即隐藏，保留兼容（旧脚本/看门狗仍传此参） */ }
        else if (a == L"--show" || a == L"--console") showConsole = true;
        else if (a == L"--crash-test") crashTest = true;
        else if (a == L"--resampler-test") { return resamplerSelfTest(); }
        else if (a == L"--tube-test") { return tubeSelfTest(); }
        else if (a == L"--pll-test") { return pllSelfTest(); }
        else if (a == L"--capture-test") { return captureSelfTest(); }
        else if (a == L"--limiter-test") { return limiterSelfTest(); }
        else if (a == L"--conv-test") { return asioConversionSelfTest(); }
        else if (a == L"--log" && i + 1 < argc) {
            // 共享读写打开日志（允许外部实时查看），重定向 stdout
            HANDLE h = CreateFileW(argv[++i], FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                int fd = _open_osfhandle((intptr_t)h, _O_APPEND | _O_TEXT);
                if (fd != -1) {
                    _dup2(fd, _fileno(stdout));
                    setvbuf(stdout, nullptr, _IONBF, 0);
                }
            }
        }
        else if (a == L"--help" || a == L"-h") { usage(); return 0; }
    }

    if (!showConsole) {   // 默认隐藏控制台窗口：后台服务/自启/手动双击均生效；--show 才显示
        HWND hw = GetConsoleWindow();
        if (hw) ShowWindow(hw, SW_HIDE);
    }

    // 必须 STA：RME MADIface ASIO 的 COM 对象(ThreadingModel=Apartment)
    // 仅在 STA 公寓中暴露其 ASIO 接口（MTA 下返回 E_NOINTERFACE）。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { printf("CoInitializeEx 失败\n"); return 1; }

    if (list) {
        // 注意顺序：AsioDrivers 局部实例析构时会无条件 CoUninitialize()
        //（SDK 老代码 bug），放在最后避免影响 COM。
        printf("ASIO 驱动:\n%s", AsioRender::listDrivers().c_str());
        CoUninitialize();
        return 0;
    }

    std::string err;

    if (tone) {
        // 测试模式: 1kHz 正弦直接进 ASIO，验证 ASIO 链路
        // 未指定 --driver 时自动遍历注册表所有 ASIO 驱动，逐个尝试（坏的跳过）
        std::vector<std::string> toneCands;
        if (!driver.empty()) toneCands.push_back(driver);
        else toneCands = ListAsioDriverNames();
        if (toneCands.empty()) {
            printf("未找到任何 ASIO 驱动（--tone 需要 ASIO）\n");
            CoUninitialize();
            return 1;
        }
        printf("== ASIO 测试模式: 1kHz 正弦 @ %g Hz ==\n", toneRate);
        std::string toneErr;
        bool toneOk = false;
        for (const auto& td : toneCands) {
            printf("[设备] 尝试 ASIO: %s\n", td.c_str());
            double phase = 0.0;
            AsioRender asio;
            asio.setDither(ditherFlag);
            asio.setPullCallback([&](float* dst, size_t frames, size_t ch) -> size_t {
                for (size_t f = 0; f < frames; ++f) {
                    float s = 0.25f * (float)sin(phase);
                    phase += 2.0 * 3.14159265358979323846 * 1000.0 / toneRate;
                    if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                    for (size_t c = 0; c < ch; ++c) dst[f * ch + c] = s;
                }
                return frames;
            });
            std::string e2;
            if (asio.init(td, toneRate, e2, reqBuffer)) {
                printf("正在播放 1kHz 测试音，DAC 屏幕应显示 %g kHz。Ctrl+C 停止。\n",
                       asio.sampleRate() / 1000.0);
                while (!g_stop.load()) Sleep(100);
                asio.shutdown();
                toneOk = true;
                break;
            }
            printf("     初始化失败: %s\n", e2.c_str());
        }
        if (!toneOk) printf("所有 ASIO 驱动初始化失败: %s\n", toneErr.empty() ? "(无)" : toneErr.c_str());
        CoUninitialize();
        return toneOk ? 0 : 1;
    }

    // ===== 正式桥接模式（含端点速率变更自适应）=====
    // 单实例互斥:正式桥接模式独占 ASIO 设备/控制台端口/目标进程,
    // 已有实例在运行则本实例退出,避免多实例抢设备(计划任务自动重启会拉起副本)。
    if (!singleInst) {
        printf("[Bridge] 已有另一个 asio_bridge 实例在运行，本实例退出（单实例互斥锁）\n");
        if (hSingle) CloseHandle(hSingle);
        CoUninitialize();
        return 0;
    }

    RingBuffer rb;
    rb.init(1 << 17);   // 131072 采样 ≈ 3s @44.1k

    std::atomic<uint64_t> written{0}, consumed{0}, underruns{0}, dropped{0};
    std::atomic<uint64_t> lastUnderrunAt{0};   // 最近一次真欠载时刻(GetTickCount64 ms)，供滑动窗口 LED
    std::atomic<uint64_t> drainEvent{0};   // 快排丢弃量（监视线程消费打印后清零）
    std::atomic<float> peak{0.0f};
    std::atomic<bool> needRestart{false};
    std::atomic<bool> devicesDirty{false};   // 设备事件触发：设备列表待重扫(热插拔/驱动安装)
    std::atomic<bool> bridgeOn{true};   // ASIO Bridge 开关：true=桥接(端点静音), false=关闭(系统音量恢复)

    // 桥内延迟实测（Core Audio AudioTimeStamp 模型）：
    // 采集线程把「已写采样数 → QPC」时间戳发布到环；ASIO 回调按消费前沿
    // 反查写入时刻，差值即环形缓冲驻留时间（≈ 桥内真实延迟）
    struct WriteStamp { uint64_t qpc; uint64_t written; };
    static constexpr size_t kStampCap = 4096;
    std::vector<WriteStamp> stampBuf(kStampCap);
    std::atomic<uint64_t> stampWrite{0};
    std::atomic<int> totalLatencyMs{0};   // 端到端延迟（桥内驻留 + ASIO 设备延迟 + 采集包周期）

    // 控制台共享状态（HTTP 线程只读写这些原子量，零 COM 接触）
    std::atomic<size_t> wMult{8};          // 当前水位目标倍数
    std::atomic<size_t> floorMult{8};      // 下限倍数（控制台可调）
    std::atomic<double> driftPpm{0.0};     // 最近一次漂移读数
    std::atomic<int> ditherReq{0};         // 抖动开关请求：0=无 1=开 2=关（实时生效）
    std::atomic<bool> ditherOn{false};     // 实际生效状态（供控制台显示）
    std::atomic<bool> resetReq{false};     // 重置统计请求（控制台触发）
    // 分数重采样器状态（桥作用域，每次重建复位）
    std::atomic<double> ratio{1.0};
    std::atomic<int> srcTaps{32};    // 重采样质量档：0=线性（低延迟）32=sinc（高精度，默认）
    std::atomic<bool> tubeOn{false};                 // 300B 电子管染色开关
    std::atomic<float> tubeWarmth{0.3f};             // 染色量 0~1(0=干净,1=明显暖)
    std::atomic<bool> thicknessOn{false};            // 厚度与宽度开关
    std::atomic<float> thicknessDelayMs{20.0f};      // 预延时 2~100ms
    std::atomic<int> thicknessWidth{0};              // 宽度档位 0~3
    std::atomic<bool> boosterOn{false};              // 火箭推进器开关(湿增益)
    std::atomic<float> boosterDb{12.0f};             // 湿增益 0~18dB
    // 输出后端共享信息（由主循环在 backend init 后写入，DSP 链/统计读取）
    std::atomic<double> outSampleRate{44100.0};      // 实际输出采样率
    std::atomic<double> outDeviceLatencyMs{0.0};     // 输出设备延迟(毫秒)
    // 设备选择（控制台扫描/选择，主循环按选择创建后端）
    std::mutex g_devMutex;
    std::vector<DeviceEntry> g_devices;
    std::string g_selectedKey;   // 选中设备的稳定键(空=未选择)；与 g_devMutex 同锁，避免列表刷新后索引漂移
    // 在线升级（可选：cfg update_url 指定更新源；空=内置 GitHub Releases）
    UpdateState updateState;
    std::string updateUrl;   // cfg 读入（主线程独占，启动检查线程前设置）
    FractionalResampler rs;
    std::vector<float> rsIn_;
    std::vector<TubeWarmth> tubeState;   // 每声道一个 300B 染色实例(含 DC blocker 状态)
    std::vector<ThicknessWidth> thicknessState;   // 每声道一个「厚度与宽度」(预延时+高低切)
    float thicknessRate = 0.0f;                   // 上次滤波系数采样率(ASIO 回调独占)
    // 频谱观察(瀑布图):输入频谱(音乐原始)+ 残差频谱(新增谐波),各 kFftBins 个 bin
    std::vector<float> specIn(kFftBins), specRes(kFftBins);
    std::atomic<uint64_t> specSeq{0};    // 频谱更新序号
    std::vector<float> fftInBuf(kFftN), fftResBuf(kFftN);  // 采样累积缓冲
    std::vector<float> fftRe(kFftN), fftIm(kFftN);         // FFT 工作缓冲
    int fftPos = 0;                      // 累积计数(ASIO 回调线程独占)
    double wmAvg = 0.0;                    // 平滑水位（主线程独占，约 20s 时间常数）
    // 时钟速率锁（双环前馈）：实测输入/输出设备速率 → ratio 基值
    RateLock rateLock;
    std::atomic<double> ratioBase{1.0};    // 速率锁基值（跨重建保留）
    std::atomic<double> inRate{0.0};       // 实测输入速率 Hz（控制台显示）
    std::atomic<double> outRate{0.0};      // 实测输出速率 Hz（控制台显示）
    LARGE_INTEGER qpcFreqLI;
    QueryPerformanceFrequency(&qpcFreqLI);
    const double qpcFreq = (double)qpcFreqLI.QuadPart;
    std::atomic<long> asioRate{0}, asioBuffer{0}, asioType{0};
    std::atomic<uint32_t> capRate{0};
    std::atomic<uint64_t> wmNow{0};          // 实时水位（控制台直读，勿用计数器推算）
    std::atomic<uint64_t> rebuildCount{0};   // 重建计数（漂移窗口去污染用）
    std::atomic<bool> sessionActive{false};  // 会话运行中标志（重建期屏蔽失效回调）
    // 历史水位环形缓冲（2 秒一采样，容量 2 小时）
    std::vector<HistPoint> histBuf(kHistCap);
    std::atomic<uint64_t> histWrite{0};

    // Bridge 采集（Win11 进程回环）：按 PID 旁路监听目标进程的渲染流，
    // 目标进程退出/静音由失效回调触发重建
    WasapiProcessCapture pcap;
    pcap.setErrorCallback([&] {
        if (sessionActive.load(std::memory_order_acquire))
            needRestart.store(true);
    });
    std::atomic<DWORD> targetPid{0};        // 采集目标进程（控制台显示）
    std::atomic<bool> targetActive{false};  // 目标进程是否在产出音频
    std::atomic<DWORD> discoveredPid{0};    // 发现线程输出：当前最响渲染进程
    std::atomic<float> discoveredPeak{0.0f};// 对应峰值
    ULONGLONG lastTargetSwitchAt = 0;       // 上次目标切换时刻（20 秒冷却防抖）
    ULONGLONG lastWatchdogRebuild = 0;      // 上次看门狗重建时刻（20 秒退避）
    std::vector<EndpointMuteEntry> endpointMutes;  // 已静音端点（主线程独占，重建/退出时恢复）
    wchar_t procPref[64] = L"";             // 优先进程名子串（主线程独占读写）

    // 内嵌 Web 控制台（纯 socket 线程，零 COM）
    BridgeStatsPtrs web = {
        &written, &consumed, &underruns, &lastUnderrunAt, &dropped, &peak,
        &wMult, &floorMult, &driftPpm, &needRestart, &ditherReq, &ditherOn, &resetReq,
        &bridgeOn, &g_stop, &asioRate, &asioBuffer, &asioType, &capRate, &wmNow,
        &totalLatencyMs,
        &histBuf, &histWrite, &ratioBase, &inRate, &outRate,
        &srcTaps, &tubeOn, &tubeWarmth,
        &thicknessOn, &thicknessDelayMs, &thicknessWidth,
        &boosterOn, &boosterDb,
        &specIn, &specRes, &specSeq,
        &targetPid, &targetActive,
        &g_devices, &g_devMutex, &g_selectedKey,
        &updateState
    };
    // 装配升级状态锁：Web 线程启动第一毫秒就可能读 update 状态（UI 轮询
    // /api/status 会 lock(*update->mutex)），锁必须先于 startWebConsole 就位，
    // 否则窗口期轮询会解引用 nullptr 崩溃（mtx_do_lock 内 0xC0000005）。
    primeUpdateState(&updateState);
    startWebConsole(web);
    // 扫描输出设备(ASIO/WASAPI 判定)，供控制台设备列表展示
    {
        std::string scanErr;
        auto devs = ScanOutputDevices(scanErr);
        std::lock_guard<std::mutex> lk(g_devMutex);
        g_devices = std::move(devs);
        printf("[设备] 扫描到 %zu 个输出设备%s\n", g_devices.size(),
               scanErr.empty() ? "" : ("（" + scanErr + "）").c_str());
    }
    g_bridgeLive = 1;   // 自愈拉起解锁:服务主体已启动,此后崩溃值得自动重启
    if (crashTest) {    // 故意崩溃:验证 crashFilter→relaunchSelf→--respawn 全链路
        volatile int* p = nullptr;
        *p = 1;
    }

    // ===== 配置持久化:加载/保存控制台设置(exe 同目录 asio_bridge.cfg)=====
    // 下次服务重启时恢复上次操作(染色开关/暖度/水位下限/重采样档)。
    auto configPath = []() -> std::wstring {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p(buf);
        size_t slash = p.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"asio_bridge.cfg"
                                             : p.substr(0, slash + 1) + L"asio_bridge.cfg";
    };
    auto saveConfig = [&]() {
        g_where = "main:cfg";
        FILE* f = _wfopen(configPath().c_str(), L"w");
        if (!f) return;
        fprintf(f, "tube_on=%d\n", tubeOn.load(std::memory_order_relaxed) ? 1 : 0);
        fprintf(f, "tube_warmth=%.4f\n", tubeWarmth.load(std::memory_order_relaxed));
        fprintf(f, "thickness_on=%d\n", thicknessOn.load(std::memory_order_relaxed) ? 1 : 0);
        fprintf(f, "thickness_delay=%.1f\n", thicknessDelayMs.load(std::memory_order_relaxed));
        fprintf(f, "thickness_width=%d\n", thicknessWidth.load(std::memory_order_relaxed));
        fprintf(f, "booster_on=%d\n", boosterOn.load(std::memory_order_relaxed) ? 1 : 0);
        fprintf(f, "booster_db=%.1f\n", boosterDb.load(std::memory_order_relaxed));
        fprintf(f, "floor=%zu\n", floorMult.load(std::memory_order_relaxed));
        fprintf(f, "src_taps=%d\n", srcTaps.load(std::memory_order_relaxed));
        fprintf(f, "dither=%d\n", ditherOn.load(std::memory_order_relaxed) ? 1 : 0);
        if (!updateUrl.empty())
            fprintf(f, "update_url=%s\n", updateUrl.c_str());   // 保留在线升级更新源配置
        fprintf(f, "bridge_on=%d\n", bridgeOn.load(std::memory_order_relaxed) ? 1 : 0);  // 桥开关状态（首次运行默认待机）
        {   // 选中设备稳定键：空=未选择；与设备列表同锁，避免读撕裂
            std::lock_guard<std::mutex> lk(g_devMutex);
            fprintf(f, "selected_device_id=%s\n", g_selectedKey.c_str());  // 手动选定的输出设备键(持久化，列表刷新不漂移)
        }
        fclose(f);
    };
    auto loadConfig = [&]() -> bool {
        FILE* f = _wfopen(configPath().c_str(), L"r");
        if (!f) return false;   // 首次运行：下方置待机
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char key[64] = {0};
            char val[64] = {0};
            if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
                if (!strcmp(key, "tube_on")) tubeOn.store(atoi(val) != 0, std::memory_order_relaxed);
                else if (!strcmp(key, "tube_warmth")) {
                    float fv = (float)atof(val);
                    if (fv < 0.0f) fv = 0.0f;
                    if (fv > 1.0f) fv = 1.0f;
                    tubeWarmth.store(fv, std::memory_order_relaxed);
                }
                else if (!strcmp(key, "thickness_on")) thicknessOn.store(atoi(val) != 0, std::memory_order_relaxed);
                else if (!strcmp(key, "thickness_delay")) {
                    float fv = (float)atof(val);
                    if (fv < 2.0f) fv = 2.0f;
                    if (fv > 100.0f) fv = 100.0f;
                    thicknessDelayMs.store(fv, std::memory_order_relaxed);
                }
                else if (!strcmp(key, "thickness_width")) {
                    int v = atoi(val);
                    if (v < 0) v = 0;
                    if (v > 3) v = 3;
                    thicknessWidth.store(v, std::memory_order_relaxed);
                }
                else if (!strcmp(key, "booster_on")) boosterOn.store(atoi(val) != 0, std::memory_order_relaxed);
                else if (!strcmp(key, "booster_db")) {
                    float bv = (float)atof(val);
                    if (bv < 0.0f) bv = 0.0f;
                    if (bv > 18.0f) bv = 18.0f;
                    boosterDb.store(bv, std::memory_order_relaxed);
                }
                else if (!strcmp(key, "floor")) floorMult.store((size_t)atoi(val), std::memory_order_relaxed);
                else if (!strcmp(key, "src_taps")) srcTaps.store(atoi(val), std::memory_order_relaxed);
                else if (!strcmp(key, "dither") && !ditherArgGiven)
                    ditherFlag = (atoi(val) != 0);   // 配置次之：命令行显式指定则忽略
                else if (!strcmp(key, "update_url")) {
                    // 更新源现已固定为 GitHub + Gitee 双源（CloudBase 已退出更新链路）。
                    // 旧 cfg 中的 CloudBase 地址在此过滤掉，保存时不再写回，实现配置自动净化。
                    if (val[0] && !strstr(val, "tcloudbaseapp.com"))
                        updateUrl = val;
                }
                else if (!strcmp(key, "selected_device_id")) {
                    // 手动选定的输出设备稳定键（重启保持用户选择，列表刷新不漂移）
                    {
                        std::lock_guard<std::mutex> lk(g_devMutex);
                        g_selectedKey = val;
                    }
                }
                else if (!strcmp(key, "selected_device")) {
                    // 旧版数值索引兼容：扫描已在加载前完成，可映射到稳定键
                    int v = atoi(val);
                    if (v >= 0 && v < (int)g_devices.size()) {
                        std::lock_guard<std::mutex> lk(g_devMutex);
                        g_selectedKey = g_devices[(size_t)v].key;
                    }
                }
                else if (!strcmp(key, "bridge_on")) {
                    // 桥开关状态：恢复上次操作（首次运行无此项，保持默认待机）
                    bridgeOn.store(atoi(val) != 0, std::memory_order_relaxed);
                }
            }
        }
        fclose(f);
        return true;
    };
    bool hasCfg = loadConfig();
    if (!hasCfg) {
        // 首次运行（无 cfg）：默认待机，不主动初始化 ASIO/WASAPI 后端，
        // 避免新用户在尚未安装声卡驱动时看到一堆驱动初始化失败日志而误以为安装失败。
        // 用户装好驱动后，在 Web 控制台点击「开启桥」即可自动识别并工作。
        bridgeOn.store(false, std::memory_order_relaxed);
        printf("[桥] 首次运行：默认待机，请在 Web 控制台「输出设备」下拉框手动选择后开启桥\n");
    }
    // 静音残留自愈：上次进程被强杀/崩溃时，被静音端点的记录文件（mute_flag.txt）
    // 会残留。必须赶在首个会话建立前按记录解除静音——否则新会话会走到
    // 「本来已静音，无需记录」分支，桥关闭时恢复列表为空，系统音量卡死在
    // 静音态（即「开关联动系统音量失效」的根因）。
    {
        int recovered = RecoverOrphanMutes();
        if (recovered > 0)
            printf("[静音] 检测到上次异常退出的残留静音，已恢复 %d 个端点的音量\n", recovered);
        else if (recovered == 0)
            printf("[静音] 上次残留记录的端点已不可用（可能已拔出），清除残留标志\n");
    }
    ditherOn.store(ditherFlag, std::memory_order_relaxed);   // 控制台在首个会话前即显示正确状态
    // 在线升级检查线程（后台常驻，网络失败静默；更新源固定为 GitHub + Gitee 双源）
    startUpdateChecker(&updateState, &g_stop);
    ULONGLONG lastCfgSave = GetTickCount64();   // 配置定期保存节流(每 5 秒)

    // 目标发现线程（独立 MTA）：每 10 秒重新扫描「最响渲染进程」，
    // 结果经原子量异步发布，绝不阻塞 ASIO STA 主线程（阻塞实测会饿死回调）。
    // 同时注册设备事件监听（Core Audio 属性监听模型）：渲染设备增删/状态变化/
    // 默认设备变化即时触发重建，取代纯轮询的迟钝响应。
    // 防抖：候选进程需连续两次扫描胜出才发布，单次瞬态不触发切换。
    std::thread discoveryThread([&] {
        g_tidDisc = GetCurrentThreadId();   // 发现线程身份
        HRESULT hrc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hrc)) return;
        IMMDeviceEnumerator* devEnum = nullptr;
        DeviceNotifier* notifier = new DeviceNotifier(&needRestart, &devicesDirty, &g_stop);
        bool registered = false;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void**)&devEnum)) && devEnum &&
            SUCCEEDED(devEnum->RegisterEndpointNotificationCallback(notifier))) {
            registered = true;
            printf("[Bridge] 设备事件监听已注册（设备/默认端点变化即时重建）\n");
        }
        DWORD prevPid = 0xFFFFFFFF;   // 上一次扫描结果（防抖：连续两次一致才发布）
        while (!g_stop.load()) {
            float pk = 0.0f;
            DWORD pid = FindActiveAudioPid(procPref, &pk);
            if (pid == prevPid) {
                // 连续两次同一候选 → 确认发布
                discoveredPid.store(pid, std::memory_order_relaxed);
                discoveredPeak.store(pk, std::memory_order_relaxed);
            }
            prevPid = pid;
            for (int i = 0; i < 100 && !g_stop.load(); ++i) Sleep(100);
        }
        if (registered && devEnum) devEnum->UnregisterEndpointNotificationCallback(notifier);
        if (devEnum) devEnum->Release();
        notifier->Release();
        CoUninitialize();
    });

    bool first = true;
    ULONGLONG lastDevScan = 0;   // 设备重扫节流(秒)：插入风暴时只重扫一次
    while (!g_stop.load()) {
        // 在线升级：安装程序已启动 → 优雅退出（保留互斥锁释放/静音恢复流程）
        if (updateState.quitRequested.load(std::memory_order_acquire)) {
            printf("[升级] 升级安装已启动，桥退出中…\n");
            g_stop.store(true);
            break;
        }
        needRestart.store(false);
        // 设备事件（热插拔/新驱动安装/默认设备变化）→ 重扫设备列表：
        // 新声卡驱动加入后自动出现在控制台选择列表，并自动判定是否支持 ASIO
        if (devicesDirty.load(std::memory_order_relaxed)) {
            ULONGLONG now = GetTickCount64();
            if (now - lastDevScan >= 1000) {
                lastDevScan = now;
                devicesDirty.store(false, std::memory_order_relaxed);
                std::string scanErr;
                auto devs = ScanOutputDevices(scanErr);
                {
                    std::lock_guard<std::mutex> lk(g_devMutex);
                    g_devices = std::move(devs);
                    // 选中设备若已不在列表(被拔出/移除) → 清空选择，等待用户重新选定
                    if (!g_selectedKey.empty()) {
                        bool found = false;
                        for (const auto& d : g_devices)
                            if (d.key == g_selectedKey) { found = true; break; }
                        if (!found) g_selectedKey.clear();
                    }
                }
                printf("[设备] 设备变化，重新扫描到 %zu 个输出设备%s\n", g_devices.size(),
                       scanErr.empty() ? "" : ("（" + scanErr + "）").c_str());
            }
            // 未到节流间隔：保持标记，下一轮再重扫
        }
        // 恢复上一会话静音的目标端点（模式切换/重建/失败重试前必须先还原，
        // 否则目标端点会一直被静音）
        size_t restoredThisPass = 0;
        if (!endpointMutes.empty()) {
            restoredThisPass = endpointMutes.size();
            RestoreEndpointMutes(endpointMutes);
            printf("[静音] 已恢复 %zu 个端点的原音量\n", restoredThisPass);
        }
        // ASIO Bridge 关闭：系统音量已恢复，静默等待重新开启
        if (!bridgeOn.load(std::memory_order_relaxed)) {
            // 清空陈旧的会话显示值（桥已关闭，无活动会话）
            capRate.store(0, std::memory_order_relaxed);
            asioRate.store(0, std::memory_order_relaxed);
            asioBuffer.store(0, std::memory_order_relaxed);
            asioType.store(0, std::memory_order_relaxed);
            wmNow.store(0, std::memory_order_relaxed);
            targetActive.store(false, std::memory_order_relaxed);
            static int offStreak = 0;
            if ((offStreak++ % 25) == 0)
                printf(restoredThisPass > 0
                           ? "[桥] ASIO Bridge 已关闭（系统音量已恢复），等待开启…\n"
                           : "[桥] ASIO Bridge 已关闭，等待开启…\n");
            {
                ULONGLONG nowCfg = GetTickCount64();
                if (nowCfg - lastCfgSave >= 5000) { saveConfig(); lastCfgSave = nowCfg; }
            }
            Sleep(200);
            continue;
        }
        // 速率锁采样计数（本会话，每次重建归零；比率基值跨会话保留）
        uint64_t capCb = 0, asioCb = 0, inTotS = 0, outTotS = 0;
        // 会话内环形缓冲写入计数（延迟实测用：环内前沿 → 写入时刻映射，
        // 与累计计数解耦，避免历史丢弃量污染映射）
        std::atomic<uint64_t> ringW{0};
        stampWrite.store(0, std::memory_order_relaxed);   // 清空上一会话时间戳
        // 恢复重新 prime：静默→有声跃迁后先持稳输出，等缓冲回填再继续
        std::atomic<bool> priming{false};
        // 最近一次采集包的 QPC 时间戳（onData 写、ASIO 拉取回调读：跨线程，需原子）。
        // 用于区分「换歌间隙(采集停摆)」与「真时钟漂移欠载(采集在投递却供不上)」。
        std::atomic<uint64_t> lastCapQpc{0};
        ULONGLONG primeBegin = 0;   // prime 开始时刻（超时放弃用）
        size_t lastFloorMult = 0;   // 检测水位下限变化（即时重置目标倍数）
        std::string oerr;
        uint16_t capCh = 2;
        auto onData = [&](const float* d, uint32_t frames) {
                if (!g_tidCap) g_tidCap = GetCurrentThreadId();   // 采集线程身份
                g_where = "cap:push";
                // 静默→有声跃迁检测（>500ms 无采集即视为暂停）：触发重新 prime
                {
                    LARGE_INTEGER q;
                    QueryPerformanceCounter(&q);
                    uint64_t nowQ = (uint64_t)q.QuadPart;
                    uint64_t lp = lastCapQpc.load(std::memory_order_relaxed);
                    if (lp && nowQ - lp > (uint64_t)(qpcFreq * 0.5))
                        priming.store(true, std::memory_order_relaxed);
                    lastCapQpc.store(nowQ, std::memory_order_relaxed);
                }
                size_t n = (size_t)frames * capCh;
                size_t got = rb.write(d, n);
                written += got;
                if (got < n) dropped += n - got;
                float m = 0.0f;
                for (size_t i = 0; i < n; ++i) { float a = fabsf(d[i]); if (a > m) m = a; }
                float cur = peak.load(std::memory_order_relaxed);
                while (m > cur && !peak.compare_exchange_weak(cur, m, std::memory_order_relaxed)) {}
                // 时间戳环（每包发布写入前沿 → ASIO 侧实测桥内延迟）
                {
                    LARGE_INTEGER q;
                    QueryPerformanceCounter(&q);
                    ringW.fetch_add(got, std::memory_order_relaxed);
                    uint64_t si = stampWrite.load(std::memory_order_relaxed);
                    stampBuf[si % kStampCap] = { (uint64_t)q.QuadPart,
                                                 ringW.load(std::memory_order_relaxed) };
                    stampWrite.store(si + 1, std::memory_order_release);
                }
                // 速率锁采样：每 25 个采集块记录 (QPC, 累计投递采样)
                inTotS += n;
                if ((++capCb) % 25 == 0) {
                    LARGE_INTEGER q;
                    QueryPerformanceCounter(&q);
                    rateLock.pushIn((uint64_t)q.QuadPart, inTotS);
                }
            };
        bool capOk = false;
        // Bridge 采集：优先沿用上次目标；失效/首次启动时自动发现正在播放的进程
        {
            DWORD pid = targetPid.load(std::memory_order_relaxed);
            if (!pid) pid = FindActiveAudioPid(procPref);
            if (!pid) {
                // 无活跃渲染进程：静默等待（不打开采集——进程回环不触发
                // Windows 麦克风使用标记，也无需任何虚拟设备）
                static int quietStreak = 0;
                if ((quietStreak++ % 15) == 0)
                    printf("[Bridge] 未发现活跃渲染进程，静默等待中…\n");
                // 无会话时也定期保存配置(用户可能已通过控制台改了设置)
                {
                    ULONGLONG nowCfg = GetTickCount64();
                    if (nowCfg - lastCfgSave >= 5000) { saveConfig(); lastCfgSave = nowCfg; }
                }
                Sleep(2000);
                continue;
            }
            capOk = pcap.open(pid, onData, oerr);
            if (capOk) {
                targetPid.store(pid, std::memory_order_relaxed);
                // 整条 DSP 链为立体声硬编码：多声道混音格式(5.1/7.1)下只取前 2 通道
                // (前 L/R)，否则重采样器/ASIO 按 2 通道记账与采集通道数错位，音频错乱。
                capCh = (uint16_t)std::min<uint16_t>(pcap.format().channels, 2);
                // tap→redirect：静音目标所在渲染端点的终点主音量，消除双重声。
                // 数值自检证明回环取点在终点主音量之前、会话静音之后：
                // 终点静音不影响捕获（ASIO 不走 WDM 终点音量，同样不受影响）
                if (noMute) {
                    printf("[静音] --no-mute 诊断模式：跳过端点静音（可能出现双重声）\n");
                } else {
                    endpointMutes = MuteTargetEndpoints(pid);
                    if (!endpointMutes.empty())
                        printf("[静音] 已静音目标进程 %lu 所在 %zu 个端点——消除双重声"
                               "（重建/退出时自动恢复；该端点其他 WDM 声音也会静音）\n",
                               (unsigned long)pid, endpointMutes.size());
                    else
                        printf("[静音] 目标进程 %lu 所在端点：无新增静音"
                               "（端点已是静音态或获取音量失败——桥关闭时不会恢复此端点）\n",
                               (unsigned long)pid);
                }
            }
        }
        if (!capOk) {
            // 首次打开即失败：置 g_stop 再 break，否则 discoveryThread.join() 永久挂死
            if (first) { printf("采集打开失败: %s\n", oerr.c_str()); g_stop.store(true); break; }
            printf("[自适应] 采集重开失败: %s（2 秒后重试）\n", oerr.c_str());
            targetPid.store(0, std::memory_order_relaxed);
            Sleep(2000);
            continue;
        }
        first = false;
        sessionActive.store(true, std::memory_order_release);

        uint32_t capSr = pcap.format().sampleRate;
        uint16_t capBits = pcap.format().bitsPerSample;
        bool capFloat = pcap.format().isFloat;
        capRate.store(capSr, std::memory_order_relaxed);
        printf("[采集] %u Hz / %u 通道 / %u bit (%s)  模式: Bridge 进程回环\n",
               capSr, capCh, capBits, capFloat ? "float32" : "PCM");

        // 预填充：~46ms（够 ASIO 启动初期即可；环形缓冲吸收两时钟漂移）
        const size_t prefill = 2048;
        while (rb.available() < prefill && !g_stop.load() && !needRestart.load()) Sleep(10);

        // 自适应水位目标倍数（v2 预判式，声明于桥作用域；每次重建重置为下限）：
        //   硬触发：新欠载 → 立即 +4（上限 32 倍）
        //   预判：每 10 秒窗口波谷 < 半目标 → +4（未欠载也提前加固）
        //   回落：连续 6 窗口波谷 > 90% 目标 → -4（下限由控制台可调）
        //   启动 10 秒宽限期内不动作（排除启动瞬态）
        wMult.store(floorMult.load(std::memory_order_relaxed), std::memory_order_relaxed);
        lastFloorMult = floorMult.load(std::memory_order_relaxed);
        rs.reset();
        // 在非回调线程预热 sinc 系数表（setup 首次调用算 8192 组 sin/cos），
        // 否则首个 ASIO 回调内计算可能超时爆音。
        rs.setup(256);
        // 预热厚度与宽度（滤波系数 + 延时缓冲分配在回调外完成）
        if (thicknessState.size() < capCh) thicknessState.resize(capCh);
        for (size_t tc = 0; tc < thicknessState.size(); ++tc) thicknessState[tc].setup((float)capSr);
        thicknessRate = (float)capSr;
        rateLock.reset();
        wmAvg = 0.0;

        // ===== 输出后端候选构建 + 逐个尝试初始化 =====
        // 全手动选择模式（无自动回退）：
        //   显式选择(控制台下拉选定 / --driver 指定) → 只试用户指定的那一个，失败即报错重试
        //   未选择(selectedDevice<0 且未传 --driver) → 不自动回退，空闲等待用户在控制台选择
        struct BackendCand {
            bool        asio = false;
            std::string driver;      // asio=true 时的 ASIO 驱动名
            std::wstring id, name;   // asio=false 时的 WASAPI 端点
        };
        std::vector<BackendCand> cands;
        bool explicitPick = false;   // 用户显式指定（不自动换候选）
        bool selEmpty = true;        // 选中键为空（未选择）
        {
            std::lock_guard<std::mutex> lk(g_devMutex);
            selEmpty = g_selectedKey.empty();
            if (!g_selectedKey.empty()) {
                for (const auto& d : g_devices) {
                    if (d.key == g_selectedKey) {
                        BackendCand c; c.asio = d.asio; c.driver = d.asioDriver;
                        c.id = d.id; c.name = d.name;
                        cands.push_back(std::move(c));
                        explicitPick = true;
                        break;
                    }
                }
            }
        }
        if (!explicitPick && !driver.empty()) {
            BackendCand c; c.asio = true; c.driver = driver; c.name = L"(--driver)";
            cands.push_back(std::move(c));
            explicitPick = true;
        }
        // 全手动选择：除用户显式选定的设备(--driver 或控制台下拉)外，不再自动回退。
        // 未选择时 cands 为空，下方进入「未选择输出设备」空闲分支，等待用户在控制台选择。
        if (cands.empty()) {
            if (selEmpty && driver.empty()) {
                // 全手动选择：用户尚未在控制台选定输出设备 → 空闲等待选择
                static int needSelStreak = 0;
                if ((needSelStreak++ % 10) == 0)
                    printf("[设备] 尚未选择输出设备，请在 Web 控制台「输出设备」下拉框手动选择"
                           "（支持 ASIO / WASAPI）后开启桥\n");
            } else {
                printf("输出后端初始化失败: 未找到任何可用的输出设备"
                       "（无 ASIO 驱动且无 WASAPI 渲染端点）\n");
            }
            pcap.close();
            if (g_stop.load()) break;
            Sleep(2000);
            continue;
        }

        // ===== 会话启动状态 + DSP 拉取回调（先注册回调再 init：ASIOStart 即拉回调）=====
        // 会话启动淡入（无咔嗒切换）：每次重建后输出从 0 线性爬升
        const size_t kFadeSamples = 2048;   // ≈23ms @88.2k 采样/秒
        size_t fadePos = 0;
        // 采集包周期（WASAPI 事件驱动引擎默认 ~10ms 周期）：命名常量替代魔法数
        const double kCapturePeriodMs = 10.0;
        auto pullCb = [&](float* dst, size_t frames, size_t ch) -> size_t {
                if (!g_tidPull) g_tidPull = GetCurrentThreadId();   // ASIO 回调线程身份
                g_where = "pull:entry";
                // 恢复重新 prime：暂停→恢复后先持稳输出，等缓冲回填到 4×ASIO 缓冲。
            // 3 秒超时放弃（目标音频断续时缓冲迟迟不满，避免一直持稳误触看门狗）
            if (priming.load(std::memory_order_relaxed)) {
                if (rb.available() < frames * capCh * 4) {
                    if (primeBegin == 0) primeBegin = GetTickCount64();
                    if (GetTickCount64() - primeBegin > 3000) {
                        priming.store(false, std::memory_order_relaxed);
                        primeBegin = 0;
                    } else {
                        memset(dst, 0, frames * ch * sizeof(float));
                        return frames;
                    }
                } else {
                    priming.store(false, std::memory_order_relaxed);
                    primeBegin = 0;
                }
            }
            if (rs.carry.size() < (frames + 16) * 2) rs.setup(frames);
            // 重采样质量档切换（实时生效，切换时复位重采样器状态避免跨档 carry 混用）
            int wt = srcTaps.load(std::memory_order_relaxed);
            if (rs.taps != wt) { rs.taps = wt; rs.reset(); }
            rs.ratio = ratio.load(std::memory_order_relaxed);
            const size_t want = rs.wantIn(frames);   // 帧
            const size_t wantS = want * 2;           // 采样
            // 快排：水位 > 2.5× 目标（重建后引擎预滚突发、设备重枚举等）
            // 直接丢弃陈旧历史帧至 1.5× 目标。启动/重建时丢弃的是预滚陈旧音频；
            // 稳态水位在目标 ±1× 内摆动，不会触发。计入 dropped 以保持漂移算式正确
            {
                size_t setpointS = wMult.load(std::memory_order_relaxed) * frames * capCh;
                size_t hi = setpointS * 5 / 2;
                size_t a0 = rb.available();
                if (a0 > hi) {
                    size_t excess = a0 - setpointS * 3 / 2;
                    rb.discard(excess);
                    dropped.fetch_add(excess, std::memory_order_relaxed);
                    drainEvent.fetch_add(excess, std::memory_order_relaxed);
                }
            }
            const size_t avail = rb.available();
            // 区分「换歌间隙」与「真时钟漂移欠载」：采集 >40ms 无包 = 停摆(间隙)，
            // 此刻环空输出静音是合法行为(曲间本就无声)，不计欠载；
            // 只有采集仍在投递(包间 ~10ms)却供不上，才是有害的真欠载(时钟漂移)。
            {
                LARGE_INTEGER qg;
                QueryPerformanceCounter(&qg);
                uint64_t lpq = lastCapQpc.load(std::memory_order_relaxed);
                bool cq = !lpq || ((uint64_t)qg.QuadPart - lpq) > (uint64_t)(qpcFreq * 0.04);
                if (want > 0 && avail == 0) {
                    memset(dst, 0, frames * ch * sizeof(float));
                    if (!cq) { underruns++; lastUnderrunAt.store(GetTickCount64(), std::memory_order_relaxed); }
                    return frames;
                }
                if (rsIn_.size() < wantS) rsIn_.resize(wantS);
                size_t gotS = rb.read(rsIn_.data(), avail < wantS ? avail : wantS);
                if (gotS < wantS && !cq) { underruns++; lastUnderrunAt.store(GetTickCount64(), std::memory_order_relaxed); }
                rs.process(rsIn_.data(), gotS / 2, dst, frames);
                // FET 染色(可选)+ 厚度与宽度(谐波预延时) + 频谱累积(声道 0)
                {
                    if (tubeState.size() < ch) tubeState.resize(ch);
                    if (thicknessState.size() < ch) thicknessState.resize(ch);
                    // 采样率变化时重算滤波/延时系数(仅在 ASIO 回调线程)
                    float ar = (float)outSampleRate.load(std::memory_order_relaxed);
                    if (ar != thicknessRate) {
                        thicknessRate = ar;
                        for (size_t tc = 0; tc < thicknessState.size(); ++tc)
                            thicknessState[tc].setup(ar);
                    }
                    float w = tubeOn.load(std::memory_order_relaxed)
                              ? tubeWarmth.load(std::memory_order_relaxed) : 0.0f;
                    bool thOn = thicknessOn.load(std::memory_order_relaxed) && w > 0.0f;
                    float thDelay = thicknessDelayMs.load(std::memory_order_relaxed);
                    float thOff = kWidthOffsets[thicknessWidth.load(std::memory_order_relaxed) & 3];
                    // 火箭推进器：湿增益(0~18dB)只作用于延时谐波层，干信号不动
                    float wetGain = 1.0f;
                    if (thOn && boosterOn.load(std::memory_order_relaxed))
                        wetGain = powf(10.0f, boosterDb.load(std::memory_order_relaxed) * 0.05f);
                    for (size_t f = 0; f < frames; ++f)
                        for (size_t c = 0; c < ch; ++c) {
                            size_t i = f * ch + c;
                            float x = dst[i];
                            float y = (w > 0.0f) ? tubeState[c].process(x, w) : x;
                            if (thOn) {
                                // 干信号不变：仅谐波残差经预延时(含宽度差)+高低切后混回
                                float harm = y - x;
                                float tgt = thDelay + (c == 0 ? thOff * 0.5f : -thOff * 0.5f);
                                dst[i] = x + thicknessState[c].process(harm, tgt, ar) * wetGain;
                            } else {
                                dst[i] = y;
                            }
                            if (c == 0) {
                                float r = dst[i] - x;   // 残差 = 实际新增谐波
                                // 频谱累积(始终进行,染色关时残差为 0)
                                fftInBuf[fftPos] = x;
                                fftResBuf[fftPos] = r;
                                if (++fftPos >= kFftN) {
                                    g_where = "pull:fft";
                                    computeSpectrum(fftInBuf.data(), specIn.data(), fftRe.data(), fftIm.data(), kFftN);
                                    computeSpectrum(fftResBuf.data(), specRes.data(), fftRe.data(), fftIm.data(), kFftN);
                                    specSeq.fetch_add(1, std::memory_order_release);
                                    fftPos = 0;
                                }
                            }
                        }
                }
                consumed += gotS;
            }
            // 会话启动淡入（无咔嗒切换）：重建/换目标后前 ~23ms 线性爬升
            if (fadePos < kFadeSamples) {
                for (size_t i = 0; i < frames * ch; ++i) {
                    if (fadePos >= kFadeSamples) break;
                    dst[i] *= (float)(fadePos + 1) / (float)kFadeSamples;
                    ++fadePos;
                }
            }
            // 末端软过载限幅器（电子管软过载特性）：DSP 链末端、淡入之后无条件生效。
            // 把可能超过 ±1.0 的峰值柔化回 0.999 内，杜绝 DAC 硬削波/破音。默认开启，无开关。
            for (size_t i = 0; i < frames * ch; ++i) {
                dst[i] = TubeSoftLimiter::process(dst[i]);
            }
            // 端到端延迟实测：环内驻留 + ASIO 设备延迟 + 采集包周期
            {
                uint64_t sw = stampWrite.load(std::memory_order_acquire);
                if (sw > 1) {
                    uint64_t frontier = ringW.load(std::memory_order_relaxed) - rb.available();
                    for (uint64_t k = sw - 1; sw - k <= 64; --k) {
                        const WriteStamp& st = stampBuf[k % kStampCap];
                        if (st.written <= frontier) {
                            LARGE_INTEGER qn;
                            QueryPerformanceCounter(&qn);
                            double ms = (double)(qn.QuadPart - (LONGLONG)st.qpc) * 1000.0 / qpcFreq;
                            // 重采样器固有延迟（sinc 15 帧≈0.34ms，线性 0）——端到端延迟补全
                            double resamplerLatMs = (double)rs.latencyFrames() * 1000.0 / outSampleRate.load(std::memory_order_relaxed);
                            totalLatencyMs.store((int)(ms + outDeviceLatencyMs.load(std::memory_order_relaxed) + kCapturePeriodMs + resamplerLatMs),
                                                 std::memory_order_relaxed);
                            break;
                        }
                        if (k == 0) break;
                    }
                }
            }
            // 速率锁采样：每 25 个 ASIO 回调记录 (QPC, 累计输出采样)
            outTotS += frames * ch;
            if ((++asioCb) % 25 == 0) {
                LARGE_INTEGER q;
                QueryPerformanceCounter(&q);
                rateLock.pushOut((uint64_t)q.QuadPart, outTotS);
            }
            g_where = "pull:done";   // 已出 DSP，后续若炸则在驱动侧（ASIOOutputReady/转换）
            return frames;
        };

        // ===== 逐个尝试候选：创建 → setPullCallback → init，第一个成功即用 =====
        std::unique_ptr<AudioOutput> out;
        bool outAsio = false;
        {
            std::string lastErr;
            for (const auto& c : cands) {
                if (c.asio) {
                    out = std::make_unique<AsioRender>(c.driver);
                    outAsio = true;
                    printf("[设备] 尝试 ASIO: %s\n", c.driver.c_str());
                } else {
                    out = std::make_unique<WasapiOutput>(c.id);
                    outAsio = false;
                    printf("[设备] 尝试 WASAPI 独占: %ls\n", c.name.c_str());
                }
                out->setDither(ditherOn.load(std::memory_order_relaxed));
                out->setPullCallback(pullCb);
                std::string e2;
                if (out->init((double)capSr, e2, reqBuffer)) break;   // 成功
                printf("     初始化失败: %s\n", e2.c_str());
                out.reset();   // 失败候选释放，试下一个
                lastErr = e2;
            }
            if (!out) {
                err = lastErr.empty() ? "所有候选输出后端均初始化失败" : lastErr;
                printf("输出后端初始化失败: %s\n", err.c_str());
                if (explicitPick)
                    printf("（--driver/控制台指定，2 秒后重试）\n");
                else
                    printf("（自动模式全部候选失败，2 秒后重试）\n");
                pcap.close();
                if (g_stop.load()) break;
                Sleep(2000);
                continue;
            }
        }

        // 控制台快照 + 输出共享信息(DSP 链读取)
        OutputInfo oi = out->info();
        outSampleRate.store(oi.sampleRate, std::memory_order_relaxed);
        outDeviceLatencyMs.store(oi.latencyMs, std::memory_order_relaxed);
        asioRate.store((long)oi.sampleRate, std::memory_order_relaxed);
        asioBuffer.store(oi.bufferSize, std::memory_order_relaxed);
        asioType.store(oi.sampleType, std::memory_order_relaxed);

        printf("== 桥接运行中: Bridge -> 输出后端 @ %g Hz (%s) ==\n",
               oi.sampleRate,
               "分数重采样");
        const size_t neededPerBuf = (size_t)oi.bufferSize * capCh;

        // 主线程（ASIO 的 STA）绝不做阻塞式 COM 调用——实测会饿死 MADIface
        // 的 STA 回调机制导致进程崩溃。
        ULONGLONG lastStats = GetTickCount64();
        uint64_t lastConsumedW = consumed.load();
        ULONGLONG lastConsumedAt = GetTickCount64();
        ULONGLONG stallLogAt = 0;   // 停滞日志节流
        ULONGLONG crashAt = 0;      // 数据回调 SEH 捕获异常的起始时间（0=未发生）
        uint64_t lastUnderW = underruns.load();
        // 自适应 v2 状态：窗口波谷跟踪 + 安全窗口计数 + 启动宽限
        size_t wmMin = rb.available();
        unsigned safeWindows = 0;
        unsigned windowCount = 0;
        bool windowHadGap = false;   // 本窗口内出现过采集停摆(换歌间隙)——预判据此跳过
        ULONGLONG windowStart = GetTickCount64();
        ULONGLONG graceUntil = GetTickCount64() + 10000;
        // 漂移表（懒初始化：跳过启动瞬态，宽限期结束后再起测）
        // 用计数器差值而非瞬时水位——瞬时水位含 ±441 突发振荡，噪声达 ±16ppm
        bool driftInit = false;
        uint64_t driftWStart = 0, driftCStart = 0, driftDStart = 0;
        uint64_t driftRStart = 0;   // 窗口起始时的重建计数
        ULONGLONG driftStartAt = 0;
        while (!g_stop.load() && !needRestart.load()) {
            g_where = "main:loop";   // 节拍标签：覆盖 saveConfig 的 5 秒陈旧 main:cfg
            ULONGLONG now = GetTickCount64();
            if (now - lastStats >= 2000) {
                lastStats = now;
                // 控制台重置请求：全部计数归零 + 历史清空 + 目标回落下限
                if (resetReq.exchange(false)) {
                    written.store(0, std::memory_order_relaxed);
                    consumed.store(0, std::memory_order_relaxed);
                    underruns.store(0, std::memory_order_relaxed);
                    dropped.store(0, std::memory_order_relaxed);
                    peak.store(0.0f, std::memory_order_relaxed);
                    driftPpm.store(0.0, std::memory_order_relaxed);
                    histWrite.store(0, std::memory_order_relaxed);
                    wMult.store(floorMult.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
                    lastUnderW = 0;
                    lastConsumedW = 0;
                    lastConsumedAt = now;
                    driftInit = false;
                    wmMin = rb.available();
                    safeWindows = 0;
                    printf("[重置] 统计参数全部归零，水位目标回落下限 %zu 采样\n",
                           floorMult.load(std::memory_order_relaxed) * neededPerBuf);
                }
                // 水位下限切换 → 立即重置目标倍数（无需等自适应回落窗口）
                {
                    size_t fm = floorMult.load(std::memory_order_relaxed);
                    if (fm != lastFloorMult) {
                        printf("[自适应] 水位下限 %zu× → %zu×，目标立即重置为 %zu 采样\n",
                               lastFloorMult, fm, fm * neededPerBuf);
                        lastFloorMult = fm;
                        wMult.store(fm, std::memory_order_relaxed);
                        wmMin = rb.available();
                        safeWindows = 0;
                    }
                }
                // ASIO 停滞看门狗：消费量 4 秒不增而采集仍在写入。
                // 不立即重建——对停滞中的 RME 驱动调用 DisposeBuffers 实测会崩溃；
                // 改为等待设备/驱动自恢复，仅「设备事件」（即插即用通知）或
                // 超长停滞（120 秒）才重建。瞬态停滞（几秒）可自行恢复，零重建零崩溃
                uint64_t c = consumed.load();
                if (priming.load(std::memory_order_relaxed)) {
                    // prime 持稳期 consumed 不增长属正常，跳过停滞判定
                    lastConsumedW = c;
                    lastConsumedAt = now;
                } else if (c != lastConsumedW) { lastConsumedW = c; lastConsumedAt = now; }
                else if (now - lastConsumedAt > 4000 && written.load() > c) {
                    if (now - lastConsumedAt > 120000) {
                        printf("[自适应] ASIO 停滞 120 秒未恢复，强制重建...\n");
                        needRestart.store(true);
                        break;
                    } else if (now - stallLogAt > 10000) {
                        stallLogAt = now;
                        printf("[自适应] ASIO 回调停滞 %llu 秒，等待设备恢复（不重建，避免驱动崩溃）...\n",
                               (now - lastConsumedAt) / 1000);
                    }
                }
                // 数据回调 SEH 捕获（设备掉线波及回调路径）→ 与停滞同策略：等 120 秒自恢复再重建
                if (out->callbackCrashed()) {
                    if (crashAt == 0) {
                        crashAt = now;
                        printf("[自适应] ASIO 数据回调捕获异常（设备掉线？），等待自恢复...\n");
                    } else if (now - crashAt > 120000) {
                        printf("[自适应] 回调异常 120 秒未恢复，强制重建...\n");
                        needRestart.store(true);
                        break;
                    }
                }
                // ---- 自适应 v2：以窗口最低水位为控制信号 ----
                size_t wm = rb.available();
                if (wm < wmMin) wmMin = wm;
                // 采集停摆检测（换歌间隙）：>40ms 无采集包。窗口内出现过间隙则预判跳过，
                // 避免把「曲间合法静音导致的波谷」误判成「时钟漂移趋势」而 ratchet 上调目标
                {
                    LARGE_INTEGER qg;
                    QueryPerformanceCounter(&qg);
                    uint64_t lpq = lastCapQpc.load(std::memory_order_relaxed);
                    if (!lpq || ((uint64_t)qg.QuadPart - lpq) > (uint64_t)(qpcFreq * 0.04))
                        windowHadGap = true;
                }

                // 快排事件上报（一次丢弃打印一行）
                {
                    uint64_t de = drainEvent.exchange(0, std::memory_order_relaxed);
                    if (de) printf("[快排] 水位超 2.5× 目标，丢弃陈旧采样 %llu（水位立即回落）\n", de);
                }

                // 硬触发：新欠载 → 立即上调（宽限期内不动作）
                uint64_t u = underruns.load();
                if (u != lastUnderW) {
                    lastUnderW = u;
                    if (now >= graceUntil) {
                        size_t m = wMult.load(std::memory_order_relaxed);
                        if (m < 32) {
                            wMult.store(m + 4, std::memory_order_relaxed);
                            printf("[自适应] 新欠载(累计 %llu)，水位目标上调至 %zu 采样\n",
                                   u, (m + 4) * neededPerBuf);
                        }
                    }
                }

                // 每 10 秒窗口评估一次
                if (now - windowStart >= 10000) {
                    size_t m = wMult.load(std::memory_order_relaxed);
                    size_t target = m * neededPerBuf;
                    if (now >= graceUntil) {
                        if (windowHadGap) {
                            // 本窗口出现过换歌间隙：波谷被污染，不据此上调/回落
                            safeWindows = 0;
                        } else if (wmMin < target / 2) {
                            // 波谷低于半目标 → 预判性加固（还没欠载就升）
                            if (m < 32) {
                                wMult.store(m + 4, std::memory_order_relaxed);
                                printf("[自适应] 波谷 %zu < 半目标 %zu，预判上调至 %zu 采样\n",
                                       wmMin, target / 2, (m + 4) * neededPerBuf);
                            }
                            safeWindows = 0;
                        } else if (wmMin > target * 9 / 10) {
                            // 波谷始终高于 90% 目标 → 余量过剩，累计安全窗口
                            safeWindows++;
                            if (safeWindows >= 3 &&
                                m > floorMult.load(std::memory_order_relaxed)) {
                                wMult.store(m - 4, std::memory_order_relaxed);
                                printf("[自适应] 连续 3 窗口波谷充裕，水位目标回落至 %zu 采样\n",
                                       (m - 4) * neededPerBuf);
                                safeWindows = 0;
                            }
                        } else {
                            safeWindows = 0;
                        }
                    }
                    windowCount++;
                    if (windowCount % 6 == 0) {
                        printf("[自适应] 状态: 目标=%zu 采样, 本窗口波谷=%zu, 欠载累计=%llu\n",
                               wMult.load(std::memory_order_relaxed) * neededPerBuf, wmMin, u);
                    }
                    wmMin = wm;
                    windowStart = now;
                    windowHadGap = false;
                }

                // 控制台抖动开关（实时生效：仅切换转换核原子标志，无需重建链路）
                {
                    int dr = ditherReq.exchange(0, std::memory_order_relaxed);
                    if (dr == 1) {
                        out->setDither(true);
                        ditherOn.store(true, std::memory_order_relaxed);
                        printf("[抖动] 控制台切换: TPDF 抖动开启\n");
                    } else if (dr == 2) {
                        out->setDither(false);
                        ditherOn.store(false, std::memory_order_relaxed);
                        printf("[抖动] 控制台切换: TPDF 抖动关闭\n");
                    }
                }
                // 时钟速率锁（前馈基值）：实测输入/输出设备速率比，始终作用于 ratio
                {
                    double ih = 0.0, oh = 0.0;
                    // Bridge 采集交付易受暂停/恢复突发污染：
                    // 60 秒短窗 + 500ms 断档截断 + 15 秒清洁段
                    if (rateLock.update(qpcFreq, &ih, &oh, 60.0, 0.5) &&
                        ih > 1000.0 && oh > 1000.0) {
                        double b = ih / oh;
                        if (b < 0.999) b = 0.999;
                        if (b > 1.001) b = 1.001;
                        ratioBase.store(b, std::memory_order_relaxed);
                        inRate.store(ih, std::memory_order_relaxed);
                        outRate.store(oh, std::memory_order_relaxed);
                    }
                }
                // 平滑水位（约 20 秒时间常数）：水位闭环只在基值上做微调
                if (wmAvg <= 0.0) wmAvg = (double)wm;
                else wmAvg += ((double)wm - wmAvg) * 0.1;
                {
                    int64_t setpoint = (int64_t)(wMult.load(std::memory_order_relaxed) * neededPerBuf);
                    double err = wmAvg - (double)setpoint;
                    double base = ratioBase.load(std::memory_order_relaxed);
                    // 误差分档限幅：稳态微调 0.0003；中等偏差 0.001；
                    // 大偏差（快排阈值以下残留 + 突发回流）0.004 —— 恢复提速
                    // 但不会超过 0.4%（听感不可察，且只出现在恢复瞬态）
                    double ea = fabs(err);
                    double cap = ea > 2048.0 ? 0.004 : (ea > 1024.0 ? 0.001 : 0.0003);
                    double r = base + err * 5e-7;
                    if (r > base + cap) r = base + cap;
                    if (r < base - cap) r = base - cap;
                    ratio.store(r, std::memory_order_relaxed);
                }

                // 历史水位采样（发布到环形缓冲，供 Web 控制台回看）
                wmNow.store(wm, std::memory_order_relaxed);
                {
                    uint64_t hi = histWrite.load(std::memory_order_relaxed);
                    histBuf[hi % kHistCap] = {
                        (uint32_t)(now / 1000),
                        (uint32_t)wm,
                        (uint32_t)(wMult.load(std::memory_order_relaxed) * neededPerBuf)
                    };
                    histWrite.store(hi + 1, std::memory_order_release);
                }
                SYSTEMTIME st;
                GetLocalTime(&st);
                float peakVal = peak.exchange(0.0f);
                targetActive.store(peakVal > 0.005f, std::memory_order_relaxed);
                // 目标自动切换：发现到「明显更响」的进程时重建锁定。
                // 冷却 20 秒防抖；候选需比当前目标响 ≥1.5 倍才切换
                {
                    DWORD dp = discoveredPid.load(std::memory_order_relaxed);
                    float dpeak = discoveredPeak.load(std::memory_order_relaxed);
                    DWORD cur = targetPid.load(std::memory_order_relaxed);
                    if (dp && dp != cur && dpeak > 0.02f && peakVal < dpeak * 0.66f &&
                        now - lastTargetSwitchAt > 20000) {
                        printf("[Bridge] 目标切换: PID %lu(近峰值 %.3f) -> PID %lu(峰值 %.3f)\n",
                               (unsigned long)cur, peakVal, (unsigned long)dp, dpeak);
                        targetPid.store(dp, std::memory_order_relaxed);
                        lastTargetSwitchAt = now;
                        needRestart.store(true);
                    }
                }
                printf("[%02u:%02u:%02u] 水位=%zu 欠载=%llu 丢弃=%llu 峰值=%.3f 写=%llu 读=%llu 目标=%zu\n",
                       st.wHour, st.wMinute, st.wSecond,
                       rb.available(), underruns.load(), dropped.load(), peakVal,
                       written.load(), consumed.load(),
                       wMult.load(std::memory_order_relaxed) * neededPerBuf);

                // 漂移表：每 300 秒报告两时钟相对偏差（长窗口压制 ±441 突发振荡噪声：
                // 60 秒窗口噪声 ±16ppm，300 秒窗口 ±3ppm）
                if (!driftInit && now >= graceUntil) {
                    driftInit = true;
                    driftWStart = written.load();
                    driftCStart = consumed.load();
                    driftDStart = dropped.load();
                    driftRStart = rebuildCount.load(std::memory_order_relaxed);
                    driftStartAt = now;
                }
                if (driftInit && now - driftStartAt >= 300000) {
                    uint64_t dW = written.load() - driftWStart;
                    uint64_t dC = consumed.load() - driftCStart;
                    uint64_t dD = dropped.load() - driftDStart;
                    if (rebuildCount.load(std::memory_order_relaxed) != driftRStart) {
                        printf("[漂移] 窗口内含重建事件，跳过本次测量\n");
                    } else if (dC > 0) {
                        double ppm = (double)((int64_t)dW - (int64_t)dC - (int64_t)dD) * 1e6 / (double)dC;
                        driftPpm.store(ppm, std::memory_order_relaxed);
                        printf("[漂移] 300秒窗口: %+.2f ppm（%s）\n", ppm,
                               ppm > 0.0 ? "采集时钟快于 ASIO 时钟" : "ASIO 时钟快于采集时钟");
                    }
                    driftWStart = written.load();
                    driftCStart = consumed.load();
                    driftDStart = dropped.load();
                    driftRStart = rebuildCount.load(std::memory_order_relaxed);
                    driftStartAt = now;
                }
            }
            // 配置定期保存(每 5 秒,节流避免频繁写盘)
            {
                ULONGLONG nowCfg = GetTickCount64();
                if (nowCfg - lastCfgSave >= 5000) {
                    saveConfig();
                    lastCfgSave = nowCfg;
                }
            }
            Sleep(50);
        }
        sessionActive.store(false, std::memory_order_release);
        rebuildCount.fetch_add(1, std::memory_order_relaxed);
        printf("[自适应] 关闭采集端...\n");
        pcap.close();
        printf("[自适应] 关闭输出端...\n");
        out->shutdown();
        rb.reset();   // 换速率后清空缓冲，避免旧速率采样残留
        if (!g_stop.load())
            printf("[自适应] 链路重建...\n");
    }

    // 退出前恢复所有被静音的目标端点（防止桥崩溃/退出后目标端点哑掉）
    if (!endpointMutes.empty()) {
        RestoreEndpointMutes(endpointMutes);
        printf("[静音] 退出：已恢复全部目标端点音量\n");
    }
    pcap.close();
    if (discoveryThread.joinable()) discoveryThread.join();
    stopWebConsole();
    CoUninitialize();
    if (hSingle) CloseHandle(hSingle);
    printf("已退出。\n");
    return 0;
}
