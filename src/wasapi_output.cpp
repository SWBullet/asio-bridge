#include "wasapi_output.h"
#include "util.h"
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <avrt.h>
#include <cstring>
#include <cmath>
#include <cstdio>

// 格式判断：是否 float32
static bool IsFloatFormat(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* wfe = (const WAVEFORMATEXTENSIBLE*)wf;
        static const GUID kFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
        return IsEqualGUID(wfe->SubFormat, kFloat);
    }
    return false;
}

// float32 → 有符号整数(2^(bits-1) 对称缩放 + 夹取)，与 ASIO 转换核同算法(无抖动简化版)
static inline int32_t F2I(float x, int bits) {
    const double scale = (bits >= 32) ? 2147483648.0 : (double)(int64_t(1) << (bits - 1));
    double v = (double)x * scale;
    const double lo = -scale, hi = scale - 1.0;
    if (v < lo) v = lo; else if (v > hi) v = hi;
    return (int32_t)llrint(v);
}

bool WasapiOutput::init(double sampleRate, std::string& err, long bufferFrames) {
    (void)sampleRate;   // WASAPI 独占由设备混音格式决定采样率(见 info())
    (void)bufferFrames;
    shutdown();
    running_.store(true);
    failed_.store(false);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) { err = "创建 WASAPI 渲染线程失败"; running_.store(false); return false; }
    DWORD r = WaitForSingleObject(readyEvent_, 8000);
    if (r != WAIT_OBJECT_0) { err = "WASAPI 渲染线程初始化超时"; shutdown(); return false; }
    if (!initErr_.empty()) { err = initErr_; shutdown(); return false; }
    return true;
}

void WasapiOutput::cleanupCom() {
    if (client_) { client_->Stop(); client_->Release(); client_ = nullptr; }
    if (render_) { render_->Release(); render_ = nullptr; }
}

// 关闭（含 init 失败路径）：统一走关线程 + 句柄纪律
void WasapiOutput::shutdown() {
    running_.store(false);
    if (event_) SetEvent(event_);
    if (thread_) {
        DWORD r = WaitForSingleObject(thread_, 15000);
        if (r == WAIT_OBJECT_0) { CloseHandle(thread_); thread_ = nullptr; }
        else {
            // 线程挂死：泄漏句柄而非误关(与采集线程同纪律)
            thread_ = nullptr; event_ = nullptr; readyEvent_ = nullptr;
            return;
        }
    }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
}

DWORD WINAPI WasapiOutput::threadProc(LPVOID p) {
    ((WasapiOutput*)p)->threadLoop();
    return 0;
}

void WasapiOutput::threadLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { initErr_ = "WASAPI 渲染线程 CoInitializeEx 失败"; SetEvent(readyEvent_); return; }

    // 1) 激活目标设备
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en)) && en)
        hr = en->GetDevice(deviceId_.c_str(), &dev);
    else hr = E_FAIL;
    if (FAILED(hr) || !dev) {
        initErr_ = "获取输出设备失败(端点 ID 可能已变化)";
        if (en) en->Release();
        failed_.store(true); SetEvent(readyEvent_); CoUninitialize(); return;
    }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
    dev->Release();
    if (en) en->Release();
    if (FAILED(hr) || !client_) { initErr_ = "Activate(IAudioClient) 失败"; failed_.store(true); SetEvent(readyEvent_); CoUninitialize(); return; }

    // 2) 混音格式 + 独占初始化(事件驱动)；失败则探测候选格式
    WAVEFORMATEX* mixFmt = nullptr;
    if (FAILED(client_->GetMixFormat(&mixFmt)) || !mixFmt) { initErr_ = "GetMixFormat 失败"; goto fail; }
    fmt_ = *mixFmt;
    isFloat_ = IsFloatFormat(mixFmt);
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client_->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME bufDur = (defPeriod > 0) ? defPeriod * 2 : 200000;   // 2×默认周期 ≈ 20ms

    // 尝试用给定格式做独占初始化（含缓冲尺寸/对齐错误重试）。
    // 关键：独占模式下缓冲时长不是设备周期整数倍时，不同驱动回报的 HRESULT 不同——
    //   AUDCLNT_E_BUFFER_SIZE_ERROR        = 0x88890016  （本机 Realtek / Bose 实测即此码）
    //   AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED  = 0x88890019
    // 旧代码只判 NOT_ALIGNED，导致 0x016 从不重试、独占模式在所有设备上恒失败
    // （症状：桥反复「输出后端初始化失败」、采样全丢、无声）。两者都要重算时长重试。
    auto tryInitX = [&](const WAVEFORMATEX* f, REFERENCE_TIME dur) -> HRESULT {
        HRESULT h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, f, nullptr);
        if (h != AUDCLNT_E_BUFFER_SIZE_ERROR && h != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
            return h;
        // 重试候选：对齐帧数 → 设备最小周期 → 设备默认周期
        REFERENCE_TIME alt[3];
        int n = 0;
        UINT32 af = 0;
        if (SUCCEEDED(client_->GetBufferSize(&af)) && af > 0 && f->nSamplesPerSec > 0) {
            REFERENCE_TIME aligned = (REFERENCE_TIME)(10000000.0 * (double)af / (double)f->nSamplesPerSec);
            if (aligned > 0) alt[n++] = aligned;
        }
        if (minPeriod > 0) alt[n++] = minPeriod;
        if (defPeriod > 0) alt[n++] = defPeriod;
        for (int i = 0; i < n && FAILED(h); ++i) {
            if (alt[i] == dur) continue;
            h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, alt[i], 0, f, nullptr);
        }
        return h;
    };

    hr = tryInitX(mixFmt, bufDur);
    if (FAILED(hr)) {
        // 候选格式：float32/int16/int32 × 48k/44.1k/32k
        WAVEFORMATEX cand;
        memset(&cand, 0, sizeof(cand));
        cand.nChannels = 2;
        const DWORD rates[] = { 48000, 44100, 32000 };
        for (int tag = 0; tag < 3 && FAILED(hr); ++tag) {
            if (tag == 0) { cand.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; cand.wBitsPerSample = 32; }
            else if (tag == 1) { cand.wFormatTag = WAVE_FORMAT_PCM; cand.wBitsPerSample = 16; }
            else { cand.wFormatTag = WAVE_FORMAT_PCM; cand.wBitsPerSample = 32; }
            cand.nBlockAlign = cand.nChannels * cand.wBitsPerSample / 8;
            for (DWORD rate : rates) {
                cand.nSamplesPerSec = rate;
                cand.nAvgBytesPerSec = rate * cand.nBlockAlign;
                WAVEFORMATEX* closest = nullptr;
                if (SUCCEEDED(client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &cand, &closest))) {
                    if (closest) CoTaskMemFree(closest);
                    hr = tryInitX(&cand, bufDur);
                    if (SUCCEEDED(hr)) { fmt_ = cand; isFloat_ = (cand.wFormatTag == WAVE_FORMAT_IEEE_FLOAT); break; }
                } else if (closest) {
                    CoTaskMemFree(closest);
                }
            }
        }
    }
    if (FAILED(hr)) {
        // 独占不可用 → 降级共享模式。
        // 实测：Win11(10.0.26200) 上 Realtek / Bose 端点独占恒返回
        // AUDCLNT_E_BUFFER_SIZE_ERROR(0x88890016)，按对齐帧数/设备周期重试仍无效。
        // 共享模式延迟略高(约 20ms，独占约 5-10ms)，但几乎每个渲染端点都可用，
        // 远好于「永久初始化失败 → 采样全丢 → 没声音」。
        // 必须传完整的 mixFmt 指针：GetMixFormat 常返回 WAVEFORMATEXTENSIBLE，
        // 而 fmt_ 只是其 WAVEFORMATEX 截断拷贝（cbSize 仍声明 22 字节扩展数据，
        // 但 SubFormat 等已被丢弃）→ 传 &fmt_ 会被判 UNSUPPORTED_FORMAT(0x88890008)。
        const HRESULT hrEx = hr;   // 保存独占失败码（下面 hr 会被共享模式结果覆盖）
        HRESULT hs = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        0, 0, mixFmt, nullptr);
        if (SUCCEEDED(hs)) {
            shared_ = true;
            hr = hs;
            printf("[设备] 独占模式不可用(hr=0x%08lX)，已降级为 WASAPI 共享模式\n",
                   (unsigned long)hrEx);
        } else {
            const char* hint = (hr == AUDCLNT_E_BUFFER_SIZE_ERROR ||
                                hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) ? " (独占缓冲时长不符设备要求)"
                             : (hr == AUDCLNT_E_DEVICE_IN_USE)            ? " (设备已被其他程序独占占用)"
                             : (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)       ? " (该格式独占模式不支持)"
                                                                          : " (设备被占用或格式不支持?)";
            initErr_ = "WASAPI 独占 Initialize 失败 hr=" + hresultText(hr) + hint
                     + "；共享模式亦失败 hr=" + hresultText(hs);
            CoTaskMemFree(mixFmt);
            goto fail;
        }
    }
    CoTaskMemFree(mixFmt);

    UINT32 bufFrames = 0;
    if (FAILED(client_->GetBufferSize(&bufFrames)) || bufFrames == 0) { initErr_ = "GetBufferSize 失败"; goto fail; }
    deviceBufFrames_ = bufFrames;
    // 共享模式：块大小取「引擎周期帧数」，而非端点缓冲总量。
    // 实测(Bose Revolve+ USB)：端点缓冲总量 1056 帧，但引擎每 10ms 事件只释放
    // 480 帧（一个周期）→ 若按缓冲总量请求，2 秒内 200 次事件仅 67 次成功、
    // 133 次返回 AUDCLNT_E_BUFFER_TOO_LARGE(0x88890006)，实际写入仅 ~35k 帧/秒，
    // 远低于设备 48k 需求 → 设备缓冲周期性耗尽（断续/咔哒），同时环水位只增不减、
    // 周期性触发快排丢弃（第二重跳变）。改用周期帧数后：100 Hz × 480 = 48000 帧/秒，
    // 与设备消费严格匹配，水位回到目标附近、快排不再触发。
    UINT32 blockFrames = bufFrames;
    REFERENCE_TIME dp2 = 0, mp2 = 0;
    if (shared_ && SUCCEEDED(client_->GetDevicePeriod(&dp2, &mp2)) && dp2 > 0) {
        UINT32 pf = (UINT32)((double)dp2 * (double)fmt_.nSamplesPerSec / 10000000.0);
        if (pf > 0 && pf <= bufFrames) blockFrames = pf;
    }
    hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
    if (FAILED(hr) || !render_) { initErr_ = "GetService(IAudioRenderClient) 失败"; goto fail; }
    if (FAILED(client_->SetEventHandle(event_))) { initErr_ = "SetEventHandle 失败"; goto fail; }

    // 3) 填充 info
    info_.sampleRate = (double)fmt_.nSamplesPerSec;
    info_.bufferSize = (long)blockFrames;
    info_.sampleType = fmt_.wFormatTag;
    printf("[设备] WASAPI %s：端点缓冲 %u 帧，每次写入 %u 帧（引擎周期 %u 帧）\n",
           shared_ ? "共享" : "独占", (unsigned)bufFrames, (unsigned)blockFrames,
           (dp2 > 0) ? (unsigned)((double)dp2 * (double)fmt_.nSamplesPerSec / 10000000.0) : 0);
    {
        long long lat = 0;
        if (SUCCEEDED(client_->GetStreamLatency(&lat)))
            info_.latencyMs = (double)lat / 10000.0;   // 100ns → ms
    }
    uint16_t ch = fmt_.nChannels;
    if (!isFloat_) scratch_.resize((size_t)bufFrames * ch);

    if (FAILED(client_->Start())) { initErr_ = "WASAPI Start 失败"; goto fail; }
    SetEvent(readyEvent_);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 4) 渲染循环(事件驱动, 独立函数容纳 SEH)
    renderLoop();
    cleanupCom();
    CoUninitialize();
    return;

fail:
    cleanupCom();
    failed_.store(true);
    SetEvent(readyEvent_);
    CoUninitialize();
}

// 渲染循环：事件驱动拉取 → 写设备缓冲。本函数只用 POD 局部 + 成员访问，
// 以便容纳 __try/__except(设备掉线 AV 兜底)。
void WasapiOutput::renderLoop() {
    // 共享模式：每次事件只写「当前可用空间」（实测恰为一个引擎周期 480 帧）。
    // deviceBufFrames_ 是端点缓冲总量，仅用于算可用空间；写入量固定用
    // info_.bufferSize（= 引擎周期帧数），以保持 pull 块大小恒定 —— 桥的水位
    // 目标 setpoint 按 frames 计算，块大小抖动会带偏水位目标。
    const UINT32 blockFrames = (UINT32)info_.bufferSize;
    const UINT32 totalFrames = deviceBufFrames_ ? deviceBufFrames_ : blockFrames;
    const uint16_t ch = (uint16_t)fmt_.nChannels;
    // 诊断：正常应恒为 0，仅异常（空间不足 / TOO_LARGE）时打印
    uint64_t dgT0 = GetTickCount64();
    uint64_t dgEv = 0, dgOk = 0, dgTooLarge = 0, dgFail = 0, dgShort = 0;
    __try {
        while (running_.load() && !failed_.load()) {
            DWORD r = WaitForSingleObject(event_, 500);
            if (r != WAIT_OBJECT_0) continue;
            ++dgEv;
            UINT32 n = blockFrames;
            if (shared_) {
                UINT32 pad = 0;
                if (FAILED(client_->GetCurrentPadding(&pad))) continue;
                UINT32 avail = (pad < totalFrames) ? (totalFrames - pad) : 0;
                if (avail < n) {                 // 可用空间不足一个周期
                    ++dgShort;
                    if (avail == 0) continue;    // 完全没空间：本次不写，环内有水位兜底
                    n = avail;
                }
            }
            BYTE* data = nullptr;
            HRESULT hr = render_->GetBuffer(n, &data);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                failed_.store(true);
                break;
            }
            if (FAILED(hr) || !data) {
                if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) ++dgTooLarge; else ++dgFail;
            } else {
                ++dgOk;
            }
            {
                uint64_t dgNow = GetTickCount64();
                if (dgNow - dgT0 >= 2000) {
                    if (dgTooLarge || dgFail || dgShort)
                        printf("[WASAPI] 异常统计(2s): 事件=%llu 成功=%llu TOO_LARGE=%llu 失败=%llu 空间不足=%llu"
                               " (块=%u 缓冲=%u)\n",
                               (unsigned long long)dgEv, (unsigned long long)dgOk,
                               (unsigned long long)dgTooLarge, (unsigned long long)dgFail,
                               (unsigned long long)dgShort,
                               (unsigned)blockFrames, (unsigned)totalFrames);
                    dgT0 = dgNow; dgEv = dgOk = dgTooLarge = dgFail = dgShort = 0;
                }
            }
            if (FAILED(hr) || !data) continue;
            if (isFloat_) {
                // float32：直接拉取进设备缓冲
                size_t got = pull_ ? pull_((float*)data, n, ch) : 0;
                if (got < n) {
                    float* fp = (float*)data;
                    for (size_t i = (size_t)got * ch; i < (size_t)n * ch; ++i) fp[i] = 0.0f;
                }
            } else {
                size_t got = pull_ ? pull_(scratch_.data(), n, ch) : 0;
                if (got < n) {
                    for (size_t i = (size_t)got * ch; i < (size_t)n * ch; ++i) scratch_[i] = 0.0f;
                }
                convertAndWrite(data, scratch_.data(), n);
            }
            render_->ReleaseBuffer(n, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failed_.store(true);
    }
}

void WasapiOutput::convertAndWrite(BYTE* dst, const float* src, UINT32 frames) {
    const uint16_t ch = fmt_.nChannels;
    const size_t n = (size_t)frames * ch;
    switch (fmt_.wBitsPerSample) {
        case 32: { int32_t* d = (int32_t*)dst; for (size_t i = 0; i < n; ++i) d[i] = F2I(src[i], 32); break; }
        case 24: {
            BYTE* d = dst;
            for (size_t i = 0; i < n; ++i) {
                int32_t v = F2I(src[i], 24);
                d[i*3+0] = (BYTE)(v & 0xFF);
                d[i*3+1] = (BYTE)((v >> 8) & 0xFF);
                d[i*3+2] = (BYTE)((v >> 16) & 0xFF);
            }
            break;
        }
        case 16: { int16_t* d = (int16_t*)dst; for (size_t i = 0; i < n; ++i) d[i] = (int16_t)F2I(src[i], 16); break; }
        default: memset(dst, 0, n * 2); break;
    }
}
