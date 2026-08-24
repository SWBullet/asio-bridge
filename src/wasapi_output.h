#pragma once
#include "audio_output.h"
#include <windows.h>
#include <audioclient.h>
#include <atomic>
#include <string>
#include <vector>

// ============================================================================
// WASAPI 独占渲染后端：非 ASIO 设备走这条路径，叠加同一份 DSP 链(PullCallback)。
//   独立渲染线程(MTA) + 事件驱动；格式跟随设备混音格式(float32 直接拷贝，
//   int16/24/32 走对称缩放转换)。与 AsioOutput 实现同一 AudioOutput 接口。
// ============================================================================
class WasapiOutput : public AudioOutput {
public:
    explicit WasapiOutput(const std::wstring& deviceId) : deviceId_(deviceId) {}
    ~WasapiOutput() override { shutdown(); }

    bool init(double sampleRate, std::string& err, long bufferFrames = 0) override;
    void setPullCallback(PullCallback cb) override { pull_ = std::move(cb); }
    void shutdown() override;
    OutputInfo info() const override { return info_; }
    bool callbackCrashed() const override { return failed_.load(std::memory_order_acquire); }
    void setDither(bool) override {}   // float32 直通; int 转换暂不带抖动

private:
    static DWORD WINAPI threadProc(LPVOID p);
    void threadLoop();
    void renderLoop();   // 渲染循环(独立函数以容纳 SEH，避免 C2712)
    void cleanupCom();
    void convertAndWrite(BYTE* dst, const float* src, UINT32 frames);

    std::wstring deviceId_;
    PullCallback pull_;
    OutputInfo info_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    HANDLE thread_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE readyEvent_ = nullptr;

    // 渲染线程独占
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    WAVEFORMATEX fmt_ = {};
    bool isFloat_ = false;
    std::vector<float> scratch_;
    std::string initErr_;
};
