#include "asio_render.h"
#include "asiodrivers.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// 由 asiodrivers.cpp / asio.cpp 提供的全局
extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

static AsioRender* g_self = nullptr;

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

bool AsioRender::init(const std::string& driverName, double sampleRate, std::string& err, long bufferFrames) {
    g_self = this;
    char name[64] = {0};
    strncpy_s(name, sizeof(name), driverName.c_str(), _TRUNCATE);

    ASIODriverInfo info = {};
    strncpy_s(info.name, sizeof(info.name), name, _TRUNCATE);
    info.sysRef = nullptr;
    ASIOError ae = ASE_NotPresent;

    // 设备掉线可能清空驱动函数指针表，loadAsioDriver/ASIOInit 会 AV —— SEH 兜底
    if (asioLoadInitSafe(name, &info, &ae)) {
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
        err = std::string("ASIOInit 失败: ") + info.errorMessage;
        shutdown();
        return false;
    }
    printf("[ASIO] 驱动: %s 版本: %ld\n", info.name, info.driverVersion);

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

    ASIOCallbacks cb = {};
    cb.bufferSwitch = &AsioRender::bufferSwitchCB;
    cb.sampleRateDidChange = &AsioRender::sampleRateDidChangeCB;
    cb.asioMessage = &AsioRender::asioMessageCB;
    cb.bufferSwitchTimeInfo = &AsioRender::bufferSwitchTimeInfoCB;

    ae = ASIOCreateBuffers(bufInfos_.data(), (long)channels_, bufferSize_, &cb);
    if (ae != ASE_OK) { err = "ASIOCreateBuffers 失败（错误码 " + std::to_string(ae) + "）"; shutdown(); return false; }

    ae = ASIOStart();
    if (ae != ASE_OK) { err = "ASIOStart 失败（错误码 " + std::to_string(ae) + "，驱动可能被其他程序占用）"; shutdown(); return false; }
    started_ = true;
    // 设备延迟（ASIOGetLatencies）：用于端到端延迟表
    ASIOGetLatencies(&inputLatency_, &outputLatency_);
    printf("[ASIO] 延迟 输入=%ld 帧 输出=%ld 帧 @ %g Hz（%.2f ms）\n",
           inputLatency_, outputLatency_, sampleRate_, (double)outputLatency_ * 1000.0 / sampleRate_);
    started_ = true;

    scratch_.resize((size_t)bufferSize_ * channels_);
    hist_.assign(channels_, std::vector<float>((size_t)bufferSize_, 0.0f));
    prevUnderrun_ = false;
    rngState_ = (uint32_t)GetTickCount() ^ 0x9E3779B9u;
    printf("[ASIO] %g Hz / %zu 通道 / 缓冲 %ld 帧 / 类型 %s\n",
           sampleRate_, channels_, bufferSize_, typeName(sampleType_));
    return true;
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
    const size_t frames = (size_t)bufferSize_;
    size_t got = pull_ ? pull_(scratch_.data(), frames, channels_) : 0;
    const size_t histLen = frames;
    if (got < frames) {
        // 波形镜像填充：把最近的真实波形反向回放填满缺口。
        // 缺口起点 = 历史末采样，数值连续无跳变；缺口内线性淡出到静音
        const size_t gap = frames - got;
        for (size_t c = 0; c < channels_; ++c) {
            for (size_t f = 0; f < gap; ++f) {
                const size_t hi = histLen - 1 - (f % histLen);
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
    // 更新波形历史（仅真实到达的样本）
    if (got > 0) {
        for (size_t c = 0; c < channels_; ++c)
            for (size_t f = 0; f < got; ++f)
                hist_[c][f] = scratch_[f * channels_ + c];
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
            case ASIOSTInt32LSB:
            case ASIOSTInt32LSB16:
            case ASIOSTInt32LSB18:
            case ASIOSTInt32LSB20:
            case ASIOSTInt32LSB24: {
                int32_t* d = (int32_t*)dst;
                for (size_t f = 0; f < frames; ++f) {
                    float x = src[f * channels_];
                    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                    if (dither_) {
                        // TPDF：两个均匀随机之和（三角分布），±1 个 Int32 LSB
                        const double dlsb = (nextRand01() - 0.5) + (nextRand01() - 0.5);
                        x += (float)(dlsb * (1.0 / 2147483648.0));
                    }
                    d[f] = (int32_t)lrintf(x * 2147483647.0f);
                }
                break;
            }
            case ASIOSTInt24LSB: {
                BYTE* d = (BYTE*)dst;
                for (size_t f = 0; f < frames; ++f) {
                    float x = src[f * channels_];
                    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                    if (dither_) {
                        const double dlsb = (nextRand01() - 0.5) + (nextRand01() - 0.5);
                        x += (float)(dlsb * (1.0 / 8388608.0));
                    }
                    int32_t v = (int32_t)lrintf(x * 8388607.0f);
                    d[f * 3 + 0] = (BYTE)(v & 0xFF);
                    d[f * 3 + 1] = (BYTE)((v >> 8) & 0xFF);
                    d[f * 3 + 2] = (BYTE)((v >> 16) & 0xFF);
                }
                break;
            }
            case ASIOSTInt16LSB: {
                int16_t* d = (int16_t*)dst;
                for (size_t f = 0; f < frames; ++f) {
                    float x = src[f * channels_];
                    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                    if (dither_) {
                        const double dlsb = (nextRand01() - 0.5) + (nextRand01() - 0.5);
                        x += (float)(dlsb * (1.0 / 32768.0));
                    }
                    d[f] = (int16_t)lrintf(x * 32767.0f);
                }
                break;
            }
            default:
                memset(dst, 0, frames * (size_t)bytes);
                break;
        }
    }
    ASIOOutputReady();
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

static inline uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

double AsioRender::nextRand01() {
    return (double)(xorshift32(rngState_) >> 8) * (1.0 / 16777216.0);
}
