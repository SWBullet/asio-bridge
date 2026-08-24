#pragma once
#include <cstddef>
#include <functional>
#include <string>

// ============================================================================
// 输出后端抽象：把「产生音频」从「驱动输出」解耦。
//   ASIO 后端(现 AsioRender) 与 WASAPI 独占后端 都实现本接口，
//   共享同一份 DSP 链(PullCallback)。
// ============================================================================

// 输出后端信息
struct OutputInfo {
    double sampleRate = 0.0;   // 实际输出采样率
    long   bufferSize = 0;     // 每块帧数
    double latencyMs  = 0.0;   // 设备输出延迟(毫秒)
    int    sampleType = 0;     // 后端格式(ASIO 类型 / WASAPI wFormatTag)
};

// 拉取回调：后端以固定帧数调用，DSP 链填充 float32 交错采样，返回实际帧数
using PullCallback = std::function<size_t(float* dst, size_t frames, size_t channels)>;

class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    // sampleRate=请求采样率(ASIO 遵循; WASAPI 独占由设备决定, 实际值见 info());
    // bufferFrames>0 时请求该缓冲帧数(仅 ASIO 生效)
    virtual bool init(double sampleRate, std::string& err, long bufferFrames = 0) = 0;
    virtual void setPullCallback(PullCallback cb) = 0;
    virtual void shutdown() = 0;
    virtual OutputInfo info() const = 0;
    // 数据回调内是否发生过异常(设备掉线)——供主循环看门狗
    virtual bool callbackCrashed() const { return false; }
    // TPDF 抖动开关(仅整数转换路径生效; float32 直通为 no-op)
    virtual void setDither(bool) {}
};
