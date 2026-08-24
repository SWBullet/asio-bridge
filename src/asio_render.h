#pragma once
// asiosys.h 必须先于 asio.h：定义平台宏与 IEEE754_64FLOAT（ASIOSampleRate=double）
#include "asiosys.h"
#include "asio.h"
#include "audio_output.h"
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// ASIO 渲染端：加载指定驱动，按给定采样率创建输出缓冲并启动；
// 回调中从 PullCallback 拉取 float32 交错采样，转换为驱动的原生类型。
class AsioRender : public AudioOutput {
public:
    AsioRender() = default;
    explicit AsioRender(std::string driverName) : driverName_(std::move(driverName)) {}
    // 返回实际写入的帧数（不足时渲染端用末样本填充）
    using PullCallback = std::function<size_t(float* interleaved, size_t frames, size_t channels)>;

    // bufferFrames > 0 时请求该 ASIO 缓冲帧数（自动对齐驱动 min/max/粒度），0 用驱动默认
    bool init(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames = 0);
    // AudioOutput 接口（driverName 由构造参数给定）
    bool init(double sampleRate, std::string& err, long bufferFrames = 0) override {
        return init(driverName_, sampleRate, err, bufferFrames);
    }
    OutputInfo info() const override;
    void shutdown() override;
    void setPullCallback(PullCallback cb) override { pull_ = std::move(cb); }
    // TPDF 抖动开关：float→整数转换时注入 ±1LSB 三角抖动（默认关，保持纯净）。
    // 回调线程读、主线程写——显式 relaxed 原子（避免依赖隐式转换的模糊写法）
    void setDither(bool on) override { dither_.store(on, std::memory_order_relaxed); }
    bool ditherEnabled() const { return dither_.load(std::memory_order_relaxed); }

    double sampleRate() const { return sampleRate_; }
    long bufferSize() const { return bufferSize_; }
    long sampleType() const { return sampleType_; }
    size_t channels() const { return channels_; }
    // ASIO 数据回调内发生过 SEH 捕获的 AV（设备掉线波及）→ 主循环应据此触发重建
    bool callbackCrashed() const override { return callbackCrashed_.load(std::memory_order_relaxed); }
    // ASIOGetLatencies：驱动上报的输入/输出延迟（帧）
    long inputLatency() const { return inputLatency_; }
    long outputLatency() const { return outputLatency_; }

    static std::string listDrivers();

private:
    static void bufferSwitchCB(long doubleBufferIndex, ASIOBool directProcess);
    // init 的 POD 主体（由 init 用 SEH 包裹调用：设备掉线时驱动函数指针失效会 AV）
    bool initInner(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames);
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
    // ⚠ 宿主回调表/驱动信息必须与实例同生命周期（成员存储），绝不能放栈上：
    // RME v1.017 等驱动对 ASIOCreateBuffers 的 callbacks 参数只存指针不拷贝，
    // initInner() 返回后栈帧被复用清零 → 驱动经悬空指针读到 NULL 的
    // bufferSwitch/bufferSwitchTimeInfo → rip=0 崩溃（实测 5~50 秒随机触发）
    ASIODriverInfo driverInfo_ = {};
    ASIOCallbacks asioCallbacks_ = {};
    bool started_ = false;
    bool loaded_ = false;
    bool prevUnderrun_ = false;                    // 上一包曾欠载 → 本包开头交叉淡化接缝
    size_t fadeInFrames_ = 32;                     // 恢复包淡入长度
    std::atomic<bool> dither_{false};              // TPDF 抖动开关（回调线程读，主线程写）
    uint32_t rngState_ = 0x9E3779B9;               // xorshift32 状态
    std::atomic<bool> callbackCrashed_{false};     // 数据回调内 SEH 捕获到 AV 标志
    std::vector<std::vector<float>> hist_;         // 每通道波形历史环形缓冲（镜像填充用）
    size_t histPos_ = 0;                           // hist_ 滚动写位置（最新样本的下一格）
    std::vector<ASIOBufferInfo> bufInfos_;
    std::vector<float> scratch_;
    std::string driverName_;                    // AudioOutput 接口用的驱动名(构造时给定)
};

// 转换核自检（独立于实例，验证 quantizeSigned 的满刻度对称性与 double 域抖动）
int asioConversionSelfTest();
