#pragma once
#include <audioclient.h>
#include <endpointvolume.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 进程回环采集（Windows 11 22H2+ 原生能力）：
//   按 PID 捕获目标进程「混音后」的渲染流（AUDCLNT_STREAMFLAGS_LOOPBACK +
//   PROCESS_LOOPBACK 激活）——旁路监听，与目标进程输出到哪个设备无关。
//   输出统一转为 float32（引擎混音格式），经回调交给环形缓冲。
//   所有 COM 调用都在内部采集线程（MTA）完成，与设备采集模块同构。
struct ProcessCaptureFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    bool isFloat = false;
    bool fixedFallback = false;   // GetMixFormat 不可用，使用 16/2/44100+AUTOCONVERTPCM 回退
};

class WasapiProcessCapture {
public:
    using DataCallback = std::function<void(const float* interleaved, uint32_t frames)>;
    using ErrorCallback = std::function<void()>;

    // pid: 目标进程 PID；启动失败时返回 false 并填 err
    bool open(DWORD pid, DataCallback cb, std::string& err);
    void close();
    void setErrorCallback(ErrorCallback cb) { onError_ = std::move(cb); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    const ProcessCaptureFormat& format() const { return fmt_; }
    // 最近是否收到过非静音数据（控制台显示目标进程活跃度）
    bool recentlyActive() const { return active_.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI threadProc(LPVOID p);
    void threadLoop();
    void drainInner();      // 无 SEH 主体
    void drain();           // SEH 兜底
    void cleanupGuarded();
    void releaseCom();
    void parseFormat(const WAVEFORMATEX* wf);
    void convert(const BYTE* src, UINT32 frames, float* dst);

    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> active_{false};
    HANDLE thread_ = nullptr;
    HANDLE event_ = nullptr;        // 数据到达事件
    HANDLE readyEvent_ = nullptr;   // 初始化完成信号
    DWORD pid_ = 0;
    DataCallback cb_;
    ErrorCallback onError_;
    std::string initErr_;

    // 以下成员由采集线程独占
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    bool started_ = false;
    ProcessCaptureFormat fmt_;
    std::vector<float> conv_;
    size_t frameSizeBytes_ = 0;
    ULONGLONG lastActiveAt_ = 0;
};

// 从全部渲染会话中发现「正在播放」的进程：按 ISimpleAudioMeter 峰值
// 选最大者（全静音时返回 0）。preferredSubstr 非空时优先匹配进程名。
// 自动排除桥自身进程（进程回环会让引擎把桥注册为回声会话，峰值等于
// 被采内容——不排除会在下一次发现时锁死自己）。
// outPeak 非空时输出所选会话的峰值。
// 自动处理 STA 线程问题：当前线程非 MTA 时委托 MTA 工作线程执行。
DWORD FindActiveAudioPid(const wchar_t* preferredSubstr, float* outPeak = nullptr);

// ---------------------------------------------------------------------------
// 目标端点静音（tap→redirect 关键组件）：进程回环是旁路监听，目标应用原声
// 仍会到达扬声器造成双重声。数值自检已证明：回环取点在「会话静音之后」、
// 在「终点主音量之前」——因此会话级静音会哑掉捕获（不可用），而静音目标
// 进程所在渲染端点的终点主音量（IAudioEndpointVolume）既消除双重声又完全
// 不影响捕获；ASIO 输出不走 WDM 终点音量，同样不受影响。
// 注意：终点静音会同时静音该端点上的其他 WDM 声音（系统提示音等）。
// ---------------------------------------------------------------------------
struct EndpointMuteEntry {
    IAudioEndpointVolume* ev = nullptr;
    BOOL prevMute = FALSE;
};
std::vector<EndpointMuteEntry> MuteTargetEndpoints(DWORD pid);
void RestoreEndpointMutes(std::vector<EndpointMuteEntry>& entries);
