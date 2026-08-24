#include "wasapi_output.h"
#include "util.h"
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <avrt.h>
#include <cstring>
#include <cmath>

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

    // 尝试用给定格式做独占初始化(含缓冲对齐重试)
    auto tryInitX = [&](const WAVEFORMATEX* f, REFERENCE_TIME dur) -> HRESULT {
        HRESULT h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, f, nullptr);
        if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 af = 0;
            if (SUCCEEDED(client_->GetBufferSize(&af)) && af > 0 && f->nSamplesPerSec > 0)
                dur = (REFERENCE_TIME)(10000000.0 * (double)af / (double)f->nSamplesPerSec);
            h = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, f, nullptr);
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
    CoTaskMemFree(mixFmt);
    if (FAILED(hr)) { initErr_ = "WASAPI 独占 Initialize 失败 hr=" + hresultText(hr) + " (设备被占用或格式不支持?)"; goto fail; }

    UINT32 bufFrames = 0;
    if (FAILED(client_->GetBufferSize(&bufFrames)) || bufFrames == 0) { initErr_ = "GetBufferSize 失败"; goto fail; }
    hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_);
    if (FAILED(hr) || !render_) { initErr_ = "GetService(IAudioRenderClient) 失败"; goto fail; }
    if (FAILED(client_->SetEventHandle(event_))) { initErr_ = "SetEventHandle 失败"; goto fail; }

    // 3) 填充 info
    info_.sampleRate = (double)fmt_.nSamplesPerSec;
    info_.bufferSize = (long)bufFrames;
    info_.sampleType = fmt_.wFormatTag;
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
    const UINT32 bufFrames = (UINT32)info_.bufferSize;
    const uint16_t ch = (uint16_t)fmt_.nChannels;
    __try {
        while (running_.load() && !failed_.load()) {
            DWORD r = WaitForSingleObject(event_, 500);
            if (r != WAIT_OBJECT_0) continue;
            BYTE* data = nullptr;
            HRESULT hr = render_->GetBuffer(bufFrames, &data);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                failed_.store(true);
                break;
            }
            if (FAILED(hr) || !data) continue;
            if (isFloat_) {
                // float32：直接拉取进设备缓冲
                size_t got = pull_ ? pull_((float*)data, bufFrames, ch) : 0;
                if (got < bufFrames) {
                    float* fp = (float*)data;
                    for (size_t i = (size_t)got * ch; i < (size_t)bufFrames * ch; ++i) fp[i] = 0.0f;
                }
            } else {
                size_t got = pull_ ? pull_(scratch_.data(), bufFrames, ch) : 0;
                if (got < bufFrames) {
                    for (size_t i = (size_t)got * ch; i < (size_t)bufFrames * ch; ++i) scratch_[i] = 0.0f;
                }
                convertAndWrite(data, scratch_.data(), bufFrames);
            }
            render_->ReleaseBuffer(bufFrames, 0);
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
