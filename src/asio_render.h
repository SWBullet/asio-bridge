#pragma once
// asiosys.h 必须先于 asio.h：定义平台宏与 IEEE754_64FLOAT（ASIOSampleRate=double）
#include "asiosys.h"
#include "asio.h"
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// ASIO 渲染端：加载指定驱动，按给定采样率创建输出缓冲并启动；
// 回调中从 PullCallback 拉取 float32 交错采样，转换为驱动的原生类型。
class AsioRender {
public:
    // 返回实际写入的帧数（不足时渲染端用末样本填充）
    using PullCallback = std::function<size_t(float* interleaved, size_t frames, size_t channels)>;

    // bufferFrames > 0 时请求该 ASIO 缓冲帧数（自动对齐驱动 min/max/粒度），0 用驱动默认
    bool init(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames = 0);
    void shutdown();
    void setPullCallback(PullCallback cb) { pull_ = std::move(cb); }
    // TPDF 抖动开关：float→整数转换时注入 ±1LSB 三角抖动（默认关，保持纯净）
    void setDither(bool on) { dither_ = on; }
    bool ditherEnabled() const { return dither_; }

    double sampleRate() const { return sampleRate_; }
    long bufferSize() const { return bufferSize_; }
    long sampleType() const { return sampleType_; }
    size_t channels() const { return channels_; }
    // ASIO 数据回调内发生过 SEH 捕获的 AV（设备掉线波及）→ 主循环应据此触发重建
    bool callbackCrashed() const { return callbackCrashed_.load(std::memory_order_relaxed); }
    // ASIOGetLatencies：驱动上报的输入/输出延迟（帧）
    long inputLatency() const { return inputLatency_; }
    long outputLatency() const { return outputLatency_; }

    static std::string listDrivers();

private:
    static void bufferSwitchCB(long doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfoCB(ASIOTime* time, long doubleBufferIndex, ASIOBool directProcess);
    static void sampleRateDidChangeCB(ASIOSampleRate rate);
    static long asioMessageCB(long selector, long value, void* message, double* opt);

    void render(long idx);
    static long typeBytes(long type);
    double nextRand01();          // [0,1) 均匀随机（xorshift32）

    PullCallback pull_;
    double sampleRate_ = 0;
    long bufferSize_ = 0;
    size_t channels_ = 0;
    long sampleType_ = ASIOSTInt32LSB;
    long inputLatency_ = 0;                          // ASIOGetLatencies 输入延迟（帧）
    long outputLatency_ = 0;                         // ASIOGetLatencies 输出延迟（帧）
    bool started_ = false;
    bool loaded_ = false;
    bool prevUnderrun_ = false;                    // 上一包曾欠载 → 本包开头交叉淡化接缝
    size_t fadeInFrames_ = 32;                     // 恢复包淡入长度
    bool dither_ = false;                          // TPDF 抖动开关
    uint32_t rngState_ = 0x9E3779B9;               // xorshift32 状态
    std::atomic<bool> callbackCrashed_{false};     // 数据回调内 SEH 捕获到 AV 标志
    std::vector<std::vector<float>> hist_;         // 每通道最近波形历史（镜像填充用）
    std::vector<ASIOBufferInfo> bufInfos_;
    std::vector<float> scratch_;
};

// 转换核自检（独立于实例，验证 quantizeSigned 的满刻度对称性与 double 域抖动）
int asioConversionSelfTest();
