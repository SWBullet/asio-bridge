#pragma once
#include <audioclient.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// 端点回环采集（「全系统过桥」的采集侧）
//
// 与进程回环(WasapiProcessCapture)的区别只在「采谁」：
//   进程回环 = 按 PID 采某个进程（含进程树）混音后的流，与它输出到哪个端点无关；
//   端点回环 = 采某个渲染端点混音后的全部声音，不限进程。
//
// 用途：把系统默认输出指向一个「哑端点」(无物理扬声器的端点，如未接线的板载
// 输出或虚拟声卡)，桥对这个端点做回环采集 → 全系统音频经桥渲染 → 输出到真实
// 设备。用户不需要逐个应用改输出设备。
//
// 取点位置与进程回环一致：会话音量之后、端点主音量之前(同一套 WASAPI 引擎
// 混音链)。推论：静音该端点主音量不影响捕获 —— 但本拓扑下源与输出已不同
// 端点，无需静音源端点，故采集端不做任何静音，端点主音量交给音量映射使用
// (任务栏滑块/静音键照常可用)。
//
// 自激保护：采集源端点不得等于桥的输出端点，否则桥会采到自己的输出形成无限
// 回环。该约束由调用方(main)在会话建立前校验。
//
// 输出固定 2 通道 float32：源端点混音格式多于 2 通道时只取前 2 通道
// (L/R)，避免与 DSP 链的立体声记账错位。
// ============================================================================
struct EndpointCaptureFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;       // 源端点混音格式的通道数
    uint16_t bitsPerSample = 0;
    bool isFloat = false;
};

class WasapiEndpointCapture {
public:
    // 回调固定交付 2 通道交错 float32
    using DataCallback = std::function<void(const float* interleavedStereo, uint32_t frames)>;

    WasapiEndpointCapture() = default;
    // 析构兜底 close()：对象常在会话作用域内构造，会话中途 continue/break 若漏调
    // close()，采集线程会继续持有 cb_（其捕获的会话局部变量已析构）= 悬垂引用 UAF。
    ~WasapiEndpointCapture() { close(); }
    WasapiEndpointCapture(const WasapiEndpointCapture&) = delete;
    WasapiEndpointCapture& operator=(const WasapiEndpointCapture&) = delete;

    // endpointId: IMMDevice::GetId 返回的端点 ID（形如 {0.0.0.0.00000000}.{GUID}）
    bool open(const std::wstring& endpointId, DataCallback cb, std::string& err);
    void close();
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    const EndpointCaptureFormat& format() const { return fmt_; }
    // 最近是否收到过非静音数据（控制台显示采集源活跃度）
    bool recentlyActive() const { return active_.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI threadProc(LPVOID p);
    void threadLoop();
    void drainInner();
    void drain();              // SEH 兜底
    void cleanupGuarded();
    void releaseCom();
    void parseFormat(const WAVEFORMATEX* wf);
    void convertStereo(const BYTE* src, UINT32 frames);

    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> cbValid_{true};   // close() 先置 false：阻止采集线程再触碰 cb_（悬垂引用 UAF）
    HANDLE thread_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    std::wstring deviceId_;
    DataCallback cb_;
    std::string initErr_;

    // 以下成员由采集线程独占
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    bool started_ = false;
    EndpointCaptureFormat fmt_;
    std::vector<float> conv_;    // 源格式全通道转换缓冲
    std::vector<float> out2_;    // 抽取后的 2 通道交付缓冲
    size_t frameSizeBytes_ = 0;
};
