#include "asio_render.h"
#include "asiodrivers.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <xmmintrin.h>   // _mm_getcsr/_mm_setcsr：x64 FTZ/DAZ 防 denormal 卡顿

// 由 asiodrivers.cpp / asio.cpp 提供的全局
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

static AsioRender* g_self = nullptr;

// 崩溃面包屑：每个 SEH 保护区进入前更新，供日志/异常过滤器定位「在哪一步炸的」
static volatile const char* g_crashContext = "idle";

// 纯 POD 的驱动加载+初始化（SEH 可安全包裹：设备掉线会清空驱动函数指针表，
// 调用会 AV）。返回 true 表示 loadAsioDriver 成功（驱动已加载），*ae 存 ASIOInit 结果
static bool asioLoadInitSafe(char* name, ASIODriverInfo* info, ASIOError* ae) {
    __try {
        if (!loadAsioDriver(name)) return false;
        *ae = ASIOInit(info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static const char* typeName(long t) {
    switch (t) {
        case ASIOSTInt16LSB:      return "Int16LSB";
        case ASIOSTInt24LSB:      return "Int24LSB";
        case ASIOSTInt32LSB:      return "Int32LSB";
        case ASIOSTFloat32LSB:    return "Float32LSB";
        case ASIOSTFloat64LSB:    return "Float64LSB";
        case ASIOSTInt32LSB16:    return "Int32LSB16";
        case ASIOSTInt32LSB18:    return "Int32LSB18";
        case ASIOSTInt32LSB20:    return "Int32LSB20";
        case ASIOSTInt32LSB24:    return "Int32LSB24";
        case ASIOSTInt16MSB:      return "Int16MSB";
        case ASIOSTInt24MSB:      return "Int24MSB";
        case ASIOSTInt32MSB:      return "Int32MSB";
        case ASIOSTFloat32MSB:    return "Float32MSB";
        case ASIOSTFloat64MSB:    return "Float64MSB";
        case ASIOSTInt32MSB16:    return "Int32MSB16";
        case ASIOSTInt32MSB18:    return "Int32MSB18";
        case ASIOSTInt32MSB20:    return "Int32MSB20";
        case ASIOSTInt32MSB24:    return "Int32MSB24";
        default:                  return "Unknown";
    }
}

long AsioRender::typeBytes(long type) {
    switch (type) {
        case ASIOSTInt16LSB:   case ASIOSTInt16MSB:                    return 2;
        case ASIOSTInt24LSB:   case ASIOSTInt24MSB:                    return 3;
        case ASIOSTFloat64LSB: case ASIOSTFloat64MSB:                  return 8;
        default:                                                        return 4;
    }
}

static inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// 统一 float→有符号整数转换核：double 域 TPDF 抖动 + 2^(bits-1) 对称缩放 + 整数域夹取。
// 返回已夹取到 [-2^(bits-1), 2^(bits-1)-1] 的 int32。
// 关键：抖动与缩放必须在 double 域进行——float32 在 |x|∈[0.5,1) 的间距是 2^-24，
// 而 32-bit 整数 1 LSB 是 2^-31，若在 float32 域加抖动会被浮点网格完全吞掉（恒 no-op）。
// 缩放必须用 2^(bits-1) 而非 2^(bits-1)-1：前者让 -1.0 映射到精确满刻度负值，
// 且正满刻度在夹取后落到 +max（避免 2147483647.0f 在 float32 里被舍入成 2^31 导致 lrint 越界翻转）。
static inline int32_t quantizeSigned(float x, int bits, bool dither, uint32_t& rng) {
    const double scale = (bits >= 32) ? 2147483648.0
                                      : (double)(int64_t(1) << (bits - 1));
    double v = (double)x * scale;
    if (dither) {
        // TPDF：两个 [0,1) 均匀随机之和减 1 → 三角分布 [-1,+1]，即 ±1 LSB
        const double u1 = (double)(xorshift32(rng) >> 8) * (1.0 / 16777216.0);
        const double u2 = (double)(xorshift32(rng) >> 8) * (1.0 / 16777216.0);
        v += u1 + u2 - 1.0;
    }
    const double lo = -scale;
    const double hi = scale - 1.0;
    if (v < lo) v = lo;
    else if (v > hi) v = hi;
    return (int32_t)llrint(v);
}

bool AsioRender::initInner(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames) {
    g_self = this;
    char name[64] = {0};
    strncpy_s(name, sizeof(name), driverName.c_str(), _TRUNCATE);

    // 驱动信息/回调表均存入成员（实例生命周期）——RME 驱动长期持有这两个指针，
    // 栈上局部变量会在 init 返回后被复用清零，导致驱动的回调分发读到空指针
    driverInfo_ = ASIODriverInfo{};
    strncpy_s(driverInfo_.name, sizeof(driverInfo_.name), name, _TRUNCATE);
    driverInfo_.sysRef = nullptr;
    ASIOError ae = ASE_NotPresent;

    // 设备掉线可能清空驱动函数指针表，loadAsioDriver/ASIOInit 会 AV —— SEH 兜底
    if (asioLoadInitSafe(name, &driverInfo_, &ae)) {
        loaded_ = true;
    }

    if (!loaded_) {
        err = "加载 ASIO 驱动失败: " + driverName + "（请确认注册表 HKLM\\SOFTWARE\\ASIO 下存在该驱动）";
        // 诊断：手动复现 SDK 加载链，定位失败环节
        HKEY hk = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, ("SOFTWARE\\ASIO\\" + driverName).c_str(), 0,
                          KEY_READ, &hk) == ERROR_SUCCESS) {
            char buf[256] = {0};
            DWORD type = 0, size = sizeof(buf);
            LONG r1 = RegQueryValueExA(hk, "clsid", nullptr, &type, (LPBYTE)buf, &size);
            printf("[诊断] RegQueryValueEx('clsid') = %ld, 值=[%s]\n", r1, buf);
            wchar_t wbuf[256] = {0};
            MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf, 256);
            CLSID clsid = {0};
            HRESULT r2 = CLSIDFromString(wbuf, &clsid);
            printf("[诊断] CLSIDFromString = 0x%08lX\n", (unsigned long)r2);
            void* p = nullptr;
            HRESULT r3 = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &p);
            printf("[诊断] CoCreateInstance = 0x%08lX, ptr=%p\n", (unsigned long)r3, p);
            if (p) ((IUnknown*)p)->Release();
            RegCloseKey(hk);
        } else {
            printf("[诊断] 无法打开注册表键\n");
        }
        return false;
    }

    if (ae != ASE_OK) {
        err = std::string("ASIOInit 失败: ") + driverInfo_.errorMessage;
        shutdown();
        return false;
    }
    printf("[ASIO] 驱动: %s 版本: %ld\n", driverInfo_.name, driverInfo_.driverVersion);

    long inCh = 0, outCh = 0;
    if (ASIOGetChannels(&inCh, &outCh) != ASE_OK) { err = "ASIOGetChannels 失败"; shutdown(); return false; }
    if (outCh <= 0) { err = "驱动没有输出通道"; shutdown(); return false; }
    channels_ = (size_t)std::min<long>(outCh, 2);   // 用前两个通道 (1+2)

    ae = ASIOSetSampleRate(ASIOSampleRate(sampleRate));
    ASIOSampleRate actual = 0.0;
    ASIOGetSampleRate(&actual);
    if (ae != ASE_OK)
        printf("[ASIO] 请求 %g Hz 失败，驱动实际 %g Hz\n", sampleRate, (double)actual);
    sampleRate_ = (double)actual;

    long minSz = 0, maxSz = 0, pref = 0, gran = 0;
    ASIOGetBufferSize(&minSz, &maxSz, &pref, &gran);
    bufferSize_ = (bufferFrames > 0) ? bufferFrames : pref;
    if (bufferSize_ < minSz) bufferSize_ = minSz;
    if (bufferSize_ > maxSz) bufferSize_ = maxSz;
    if (gran > 1) bufferSize_ = ((bufferSize_ + gran - 1) / gran) * gran;
    printf("[ASIO] 缓冲 min=%ld max=%ld pref=%ld gran=%ld 选用=%ld\n",
           minSz, maxSz, pref, gran, bufferSize_);

    ASIOChannelInfo ci = {};
    ci.channel = 0;
    ci.isInput = ASIOFalse;
    if (ASIOGetChannelInfo(&ci) != ASE_OK) { err = "ASIOGetChannelInfo 失败"; shutdown(); return false; }
    sampleType_ = ci.type;

    bufInfos_.resize(channels_);
    for (size_t i = 0; i < channels_; ++i) {
        bufInfos_[i].isInput = ASIOFalse;
        bufInfos_[i].channelNum = (long)i;
        bufInfos_[i].buffers[0] = nullptr;
        bufInfos_[i].buffers[1] = nullptr;
    }

    // ⚠ 回调表写入成员（实例生命周期）：RME 等驱动只存指针不拷贝 ASIOCallbacks，
    // 栈上局部 struct 在 init 返回后即悬空——曾致完整模式运行 5~50 秒后
    // 驱动调用已清零的 bufferSwitch 槽位（rip=0 空指针崩溃，crash.log 高频签名）
    asioCallbacks_ = ASIOCallbacks{};
    asioCallbacks_.bufferSwitch = &AsioRender::bufferSwitchCB;
    asioCallbacks_.sampleRateDidChange = &AsioRender::sampleRateDidChangeCB;
    asioCallbacks_.asioMessage = &AsioRender::asioMessageCB;
    asioCallbacks_.bufferSwitchTimeInfo = &AsioRender::bufferSwitchTimeInfoCB;

    ae = ASIOCreateBuffers(bufInfos_.data(), (long)channels_, bufferSize_, &asioCallbacks_);
    if (ae != ASE_OK) { err = "ASIOCreateBuffers 失败（错误码 " + std::to_string(ae) + "）"; shutdown(); return false; }

    ae = ASIOStart();
    if (ae != ASE_OK) { err = "ASIOStart 失败（错误码 " + std::to_string(ae) + "，驱动可能被其他程序占用）"; shutdown(); return false; }
    started_ = true;
    // 设备延迟（ASIOGetLatencies）：用于端到端延迟表
    ASIOGetLatencies(&inputLatency_, &outputLatency_);
    printf("[ASIO] 延迟 输入=%ld 帧 输出=%ld 帧 @ %g Hz（%.2f ms）\n",
           inputLatency_, outputLatency_, sampleRate_, (double)outputLatency_ * 1000.0 / sampleRate_);

    scratch_.resize((size_t)bufferSize_ * channels_);
    hist_.assign(channels_, std::vector<float>((size_t)bufferSize_, 0.0f));
    histPos_ = 0;
    prevUnderrun_ = false;
    rngState_ = (uint32_t)GetTickCount() ^ 0x9E3779B9u;
    printf("[ASIO] %g Hz / %zu 通道 / 缓冲 %ld 帧 / 类型 %s\n",
           sampleRate_, channels_, bufferSize_, typeName(sampleType_));
    return true;
}

// init 的 SEH 包装：设备掉线会清空驱动函数指针表(ASIOGetChannels/SetSampleRate/
// GetSampleRate/CreateBuffers/Start 等都是全局函数指针),调用即 AV(0xc0000005 空指针)。
// 主体在 initInner(POD 无 SEH),本包装函数只有 __try + 函数调用,无非 POD 局部对象,不会 C2712。
bool AsioRender::init(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames) {
    g_crashContext = "init";
    __try {
        return initInner(driverName, sampleRate, err, bufferFrames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_crashContext = "init:SEH";
        printf("[ASIO] 初始化期间驱动函数指针失效(设备掉线)，已安全跳过\n");
        err = "ASIO 初始化期间设备掉线（驱动函数指针失效）";
        return false;
    }
}

void AsioRender::shutdown() {
    // 先切断回调路由：ASIOStop/DisposeBuffers 期间若仍有在途回调命中
    // render()，会访问正被释放的缓冲导致 use-after-free 崩溃
    g_self = nullptr;
    // 设备掉线会清空驱动的函数指针表，调用 ASIOStop/DisposeBuffers 会在
    // 本进程内访问违例（0xc0000005）——SEH 兜底，跳过已死驱动的清理
    __try {
        if (started_) { printf("[ASIO] Stop...\n"); ASIOStop(); started_ = false; }
        if (!bufInfos_.empty()) { printf("[ASIO] DisposeBuffers...\n"); ASIODisposeBuffers(); bufInfos_.clear(); }
        if (loaded_) { printf("[ASIO] Exit...\n"); ASIOExit(); loaded_ = false; }   // 内部 removeCurrentDriver
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("[ASIO] shutdown 期间驱动异常（设备掉线），已安全跳过清理\n");
        started_ = false;
        loaded_ = false;
        bufInfos_.clear();
    }
}

void AsioRender::bufferSwitchCB(long doubleBufferIndex, ASIOBool directProcess) {
    (void)directProcess;
    if (g_self) g_self->render(doubleBufferIndex);
}

ASIOTime* AsioRender::bufferSwitchTimeInfoCB(ASIOTime* time, long doubleBufferIndex, ASIOBool directProcess) {
    (void)time; (void)directProcess;
    if (g_self) g_self->render(doubleBufferIndex);
    return nullptr;
}

void AsioRender::sampleRateDidChangeCB(ASIOSampleRate rate) {
    printf("[ASIO] 驱动通知采样率变更: %g Hz\n", rate);
}

long AsioRender::asioMessageCB(long selector, long value, void* message, double* opt) {
    (void)value; (void)message; (void)opt;
    switch (selector) {
        case kAsioSelectorSupported:  return 1L;
        case kAsioEngineVersion:      return 2L;
        case kAsioResetRequest:       printf("[ASIO] 驱动请求复位\n"); return 1L;
        case kAsioBufferSizeChange:   printf("[ASIO] 驱动请求缓冲大小变更\n"); return 1L;
        case kAsioResyncRequest:      printf("[ASIO] 驱动请求重同步\n"); return 1L;
        case kAsioLatenciesChanged:   return 1L;
        case kAsioSupportsTimeInfo:   return 0L;
        case kAsioSupportsTimeCode:   return 0L;
        default: return 0L;
    }
}

void AsioRender::render(long idx) {
    // x64 默认 FTZ/DAZ 关闭：sinc 尾部小系数在静音段会生成 denormal，单次运算可耗上百周期。
    // 在驱动回调线程打开 FTZ(0x8000)+DAZ(0x0040)，消除 denormal 卡顿（per-thread MXCSR，安全）。
    _mm_setcsr(_mm_getcsr() | 0x8040);

    // 数据回调路径此前完全裸奔：RME 掉线时若 AV 发生在这里（而非 init/stop），进程直接死。
    // 用 SEH 兜底整个回调体——本函数无局部 C++ 对象（全 POD），不会触发 C2712。
    g_crashContext = "render";
    __try {
    const size_t frames = (size_t)bufferSize_;
    size_t got = pull_ ? pull_(scratch_.data(), frames, channels_) : 0;
    const size_t histLen = frames;
    // 抖动开关一次性读取（回调内 6 个分支共用；atomic→bool 隐式 load 也行，显式更清晰）
    const bool dith = dither_.load(std::memory_order_relaxed);
    if (got < frames) {
        // 波形镜像填充：把最近的真实波形反向回放填满缺口。
        // 缺口起点 = 环形历史末采样（(histPos_+histLen-1)%histLen 恒为新到样本），
        // 数值连续无跳变；缺口内线性淡出到静音。
        // 旧写法 histLen-1-f 在「部分投递」时取到陈旧样本（上次回调残留），产生毛刺。
        const size_t gap = frames - got;
        for (size_t c = 0; c < channels_; ++c) {
            for (size_t f = 0; f < gap; ++f) {
                const size_t hi = (histPos_ + histLen - 1 - f) % histLen;
                const float g = 1.0f - (float)f / (float)gap;   // 1 → 0
                scratch_[(got + f) * channels_ + c] = hist_[c][hi] * g;
            }
        }
        prevUnderrun_ = true;
    } else if (prevUnderrun_) {
        // 恢复包：开头交叉淡化（静音 → 真实音频），掩盖接缝
        const size_t n = fadeInFrames_ < frames ? fadeInFrames_ : frames;
        for (size_t f = 0; f < n; ++f) {
            const float g = (float)f / (float)n;
            for (size_t c = 0; c < channels_; ++c)
                scratch_[f * channels_ + c] *= g;
        }
        prevUnderrun_ = false;
    }
    // 更新波形历史（滚动环形写入：仅记录真实到达的样本，histPos_ 恒指向
    // 「最新样本的下一格」——无论部分投递还是满包，时间轴都连续）
    for (size_t f = 0; f < got; ++f) {
        for (size_t c = 0; c < channels_; ++c)
            hist_[c][histPos_] = scratch_[f * channels_ + c];
        histPos_ = (histPos_ + 1) % histLen;
    }

    const long bytes = typeBytes(sampleType_);
    for (size_t c = 0; c < channels_; ++c) {
        void* dst = bufInfos_[c].buffers[idx];
        const float* src = scratch_.data() + c;
        switch (sampleType_) {
            case ASIOSTFloat32LSB: {
                float* d = (float*)dst;
                for (size_t f = 0; f < frames; ++f) d[f] = src[f * channels_];
                break;
            }
            case ASIOSTFloat64LSB: {
                double* d = (double*)dst;
                for (size_t f = 0; f < frames; ++f) d[f] = (double)src[f * channels_];
                break;
            }
            case ASIOSTInt32LSB: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = quantizeSigned(src[f * channels_], 32, dith, rngState_);
                break;
            }
            // 32-bit 容器、N-bit 数据 LSB 对齐（有效位在低 N 位，范围 ±2^(N-1)）
            case ASIOSTInt32LSB16: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = quantizeSigned(src[f * channels_], 16, dith, rngState_);
                break;
            }
            case ASIOSTInt32LSB18: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = quantizeSigned(src[f * channels_], 18, dith, rngState_);
                break;
            }
            case ASIOSTInt32LSB20: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = quantizeSigned(src[f * channels_], 20, dith, rngState_);
                break;
            }
            case ASIOSTInt32LSB24: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = quantizeSigned(src[f * channels_], 24, dith, rngState_);
                break;
            }
            case ASIOSTInt24LSB: {
                BYTE* d = (BYTE*)dst;
                for (size_t f = 0; f < frames; ++f) {
                    int32_t v = quantizeSigned(src[f * channels_], 24, dith, rngState_);
                    d[f * 3 + 0] = (BYTE)(v & 0xFF);
                    d[f * 3 + 1] = (BYTE)((v >> 8) & 0xFF);
                    d[f * 3 + 2] = (BYTE)((v >> 16) & 0xFF);
                }
                break;
            }
            case ASIOSTInt16LSB: {
                int16_t* d = (int16_t*)dst;
                for (size_t f = 0; f < frames; ++f)
                    d[f] = (int16_t)quantizeSigned(src[f * channels_], 16, dith, rngState_);
                break;
            }
            default:
                memset(dst, 0, frames * (size_t)bytes);
                break;
        }
    }
    ASIOOutputReady();
    g_crashContext = "render:done";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_crashContext = "render:SEH";
        callbackCrashed_.store(true, std::memory_order_relaxed);
        // 静音本包所有通道缓冲（半包数据会爆音）；不再做任何驱动调用，避免二次 AV
        const long bytes = typeBytes(sampleType_);
        for (size_t c = 0; c < channels_; ++c) {
            void* buf = bufInfos_[c].buffers ? bufInfos_[c].buffers[idx] : nullptr;
            if (buf) memset(buf, 0, (size_t)bufferSize_ * (size_t)bytes);
        }
    }
}

std::string AsioRender::listDrivers() {
    AsioDrivers d;
    char storage[32][64];
    char* names[32];
    for (int i = 0; i < 32; ++i) names[i] = storage[i];
    long n = d.getDriverNames(names, 32);
    std::string s;
    for (long i = 0; i < n; ++i)
        s += "  " + std::string(names[i]) + "\n";
    return s.empty() ? "  (无)\n" : s;
}

double AsioRender::nextRand01() {
    return (double)(xorshift32(rngState_) >> 8) * (1.0 / 16777216.0);
}

// 转换核自检：验证 float→int 满刻度不翻转（P0-1）、2^(b-1) 对称缩放（P0-2）、
// 以及抖动在 double 域确实有效（旧 float32 域对 int32 恒 no-op）。
int asioConversionSelfTest() {
    int fails = 0;
    uint32_t rng = 0x9E3779B9u;

    auto expect = [&](const char* name, int32_t got, int32_t want) {
        if (got != want) {
            printf("[转换自检] %s: 期望 %d(0x%08X) 实得 %d(0x%08X) => FAIL\n",
                   name, want, (unsigned)want, got, (unsigned)got);
            ++fails;
        }
    };

    // P0-1：满刻度正负不翻转（旧代码 +1.0 → INT32_MIN 满幅负脉冲爆音）
    expect("int32 +1.0", quantizeSigned(1.0f, 32, false, rng), INT32_MAX);
    expect("int32 -1.0", quantizeSigned(-1.0f, 32, false, rng), INT32_MIN);
    expect("int32  0.0", quantizeSigned(0.0f, 32, false, rng), 0);
    expect("int32 +0.5", quantizeSigned(0.5f, 32, false, rng), 1073741824);
    expect("int32 -0.5", quantizeSigned(-0.5f, 32, false, rng), -1073741824);
    // 超幅夹取
    expect("int32 +1.5 夹取", quantizeSigned(1.5f, 32, false, rng), INT32_MAX);
    expect("int32 -1.5 夹取", quantizeSigned(-1.5f, 32, false, rng), INT32_MIN);

    // P0-2：各 N-bit LSB 对齐类型用 2^(N-1) 对称缩放
    expect("int24 +1.0", quantizeSigned(1.0f, 24, false, rng), 8388607);
    expect("int24 -1.0", quantizeSigned(-1.0f, 24, false, rng), -8388608);
    expect("int16 +1.0", quantizeSigned(1.0f, 16, false, rng), 32767);
    expect("int16 -1.0", quantizeSigned(-1.0f, 16, false, rng), -32768);
    expect("lsb18 +1.0", quantizeSigned(1.0f, 18, false, rng), 131071);
    expect("lsb18 -1.0", quantizeSigned(-1.0f, 18, false, rng), -131072);
    expect("lsb20 +1.0", quantizeSigned(1.0f, 20, false, rng), 524287);
    expect("lsb20 -1.0", quantizeSigned(-1.0f, 20, false, rng), -524288);

    // 抖动有效性：int16 的 0.5 LSB 处（x=2^-16 在 float32 精确可表示）。
    // 无抖动 llrint(0.5)=0（round-half-even），有抖动应能偏离到 ±1。
    {
        uint32_t r1 = 0x12345678u, r2 = 0x12345678u;
        const float half = 1.0f / 65536.0f;   // = 2^-16
        int32_t a = quantizeSigned(half, 16, false, r1);
        int changed = 0;
        for (int i = 0; i < 256; ++i) {
            int32_t b = quantizeSigned(half, 16, true, r2);
            if (b != a) ++changed;
        }
        printf("[转换自检] 抖动有效性(int16 0.5LSB): 无抖动=%d, 有抖动 256 次中 %d 次偏离 %s\n",
               a, changed, changed > 0 ? "PASS" : "FAIL(抖动仍被吞)");
        if (changed == 0) ++fails;
    }

    printf(fails == 0 ? "== 转换核自检通过 ==\n" : "== 转换核自检存在 %d 项失败 ==\n", fails);
    return fails;
}
