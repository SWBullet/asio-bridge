#pragma once
#include <atomic>
#include <string>
#include <windows.h>

// ============================================================================
// 音量映射（「全系统过桥」的交互侧）
//
// 动机：端点回环拓扑下，声音链路是
//     应用 → 采集源端点(系统默认设备) [回环取点] → 桥 DSP → 输出端点 → 扬声器
// 而任务栏音量滑块与静音键控制的是「系统默认设备」= 采集源端点，真正出声的
// 却是输出端点。不做映射的话用户调音量完全无效（这正是普通 loopback 方案的
// 通病，也是「系统音量不可用」的根因）。
//
// 做法：独立 MTA 线程每 120ms 轮询采集源端点的 IAudioEndpointVolume
// (主音量标量 + 静音位)，任一变化即写入输出端点。于是：
//   · 任务栏滑块  → 实时改变真正输出设备的音量
//   · 静音键      → 同步静音输出端点
//
// 线程纪律：桥主线程是 ASIO 的 STA，阻塞式 COM 调用会饿死驱动回调（实测会让
// MADIface 崩溃），故全部 COM 都在本模块自己的 MTA 线程上完成。
//
// 退出语义：不恢复输出端点原音量 —— 用户通过映射设定的音量就是他要的音量，
// 桥关闭后停留在该值属预期行为。
// ============================================================================
class VolumeMapper {
public:
    VolumeMapper() = default;
    // 析构兜底 close()：映射对象在会话作用域内，会话中途退出若漏调会留下孤儿线程
    ~VolumeMapper() { close(); }
    VolumeMapper(const VolumeMapper&) = delete;
    VolumeMapper& operator=(const VolumeMapper&) = delete;

    // srcId: 采集源端点 ID；dstId: 桥输出端点 ID
    bool open(const std::wstring& srcId, const std::wstring& dstId, std::string& err);
    void close();
    bool active() const { return active_.load(std::memory_order_acquire); }
    // 最近一次同步的音量标量（-1 = 尚未同步），控制台显示用
    float lastVolume() const { return lastVol_.load(std::memory_order_relaxed); }
    int   syncCount() const { return syncCount_.load(std::memory_order_relaxed); }
    // 0=正常；非 0 = 上次失败的 HRESULT（端点失效等）
    int   lastError() const { return lastErr_.load(std::memory_order_relaxed); }

private:
    static DWORD WINAPI threadProc(LPVOID p);
    void loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};
    std::atomic<float> lastVol_{-1.0f};
    std::atomic<int> syncCount_{0};
    std::atomic<int> lastErr_{0};
    HANDLE thread_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    std::wstring srcId_, dstId_;
    std::string initErr_;
};
