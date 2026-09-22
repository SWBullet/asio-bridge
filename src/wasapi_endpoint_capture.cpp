#include "wasapi_endpoint_capture.h"
#include "util.h"
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <windows.h>
#include <cstdio>
#include <cstring>

static const GUID kSubFloat2 = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

bool WasapiEndpointCapture::open(const std::wstring& endpointId, DataCallback cb, std::string& err) {
    close();
    initErr_.clear();
    failed_.store(false);
    active_.store(false);
    deviceId_ = endpointId;
    cb_ = std::move(cb);
    cbValid_.store(true, std::memory_order_release);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);

    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) {
        err = "创建端点回环采集线程失败";
        running_.store(false);
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
        return false;
    }

    DWORD r = WaitForSingleObject(readyEvent_, 8000);
    if (r != WAIT_OBJECT_0) {
        err = "端点回环采集线程初始化超时";
        close();
        return false;
    }
    if (!initErr_.empty()) {
        err = initErr_;
        close();
        return false;
    }
    return true;
}

void WasapiEndpointCapture::close() {
    // 先切断回调（原子门）：此后采集线程即便仍存活，也不会再触碰 cb_
    // ——cb_ 捕获的 per-session 局部变量可能已被主循环析构。
    cbValid_.store(false, std::memory_order_release);
    running_.store(false);
    if (event_) SetEvent(event_);
    if (thread_) {
        DWORD r = WaitForSingleObject(thread_, 15000);
        if (r == WAIT_OBJECT_0) { CloseHandle(thread_); thread_ = nullptr; }
        else {
            printf("[Bridge] 警告: 端点回环采集线程 15 秒未退出\n");
            // 线程仍存活：泄漏句柄而非误关（与进程回环同一纪律）
            thread_ = nullptr;
            event_ = nullptr;
            readyEvent_ = nullptr;
            return;
        }
    }
    if (event_) { CloseHandle(event_); event_ = nullptr; }
    if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
    cb_ = nullptr;
}

DWORD WINAPI WasapiEndpointCapture::threadProc(LPVOID p) {
    ((WasapiEndpointCapture*)p)->threadLoop();
    return 0;
}

void WasapiEndpointCapture::releaseCom() {
    if (client_ && started_) client_->Stop();
    started_ = false;
    if (capture_) { capture_->Release(); capture_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
}

void WasapiEndpointCapture::threadLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        initErr_ = "端点回环线程 CoInitializeEx 失败 " + hresultText(hr);
        SetEvent(readyEvent_);
        return;
    }

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    WAVEFORMATEX* mf = nullptr;
    WAVEFORMATEX fb{};
    bool exact = false;
    DWORD mmTask = 0;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) {
        initErr_ = "创建 MMDeviceEnumerator 失败 " + hresultText(hr);
        goto fail;
    }

    hr = en->GetDevice(deviceId_.c_str(), &dev);
    if (FAILED(hr) || !dev) {
        initErr_ = "找不到采集源端点（已被移除/禁用？）" + hresultText(hr);
        goto fail;
    }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_);
    if (FAILED(hr) || !client_) {
        initErr_ = "激活采集源端点 IAudioClient 失败 " + hresultText(hr);
        goto fail;
    }

    // 1) 端点混音格式（端点回环客户端应能直接取到；取不到则回退固定格式）
    hr = client_->GetMixFormat(&mf);
    if (SUCCEEDED(hr) && mf) {
        parseFormat(mf);
        exact = true;
    } else {
        fb.wFormatTag = WAVE_FORMAT_PCM;
        fb.nChannels = 2;
        fb.nSamplesPerSec = 48000;
        fb.wBitsPerSample = 16;
        fb.nBlockAlign = (WORD)(fb.nChannels * fb.wBitsPerSample / 8);
        fb.nAvgBytesPerSec = fb.nSamplesPerSec * fb.nBlockAlign;
        parseFormat(&fb);
    }

    // 2) 共享模式 + 回环标志（事件驱动，与进程回环同构）
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                 (exact ? 0 : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM),
                             0, 0, exact ? mf : &fb, nullptr);
    if (FAILED(hr) && exact) {
        // 精确格式被拒（罕见）→ 回退 16/2/48000 + AUTOCONVERTPCM
        printf("[Bridge] 采集端点精确格式 Initialize 失败（%s），回退固定格式\n",
               hresultText(hr).c_str());
        if (mf) { CoTaskMemFree(mf); mf = nullptr; }
        fb.wFormatTag = WAVE_FORMAT_PCM;
        fb.nChannels = 2;
        fb.nSamplesPerSec = 48000;
        fb.wBitsPerSample = 16;
        fb.nBlockAlign = (WORD)(fb.nChannels * fb.wBitsPerSample / 8);
        fb.nAvgBytesPerSec = fb.nSamplesPerSec * fb.nBlockAlign;
        parseFormat(&fb);
        exact = false;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                 0, 0, &fb, nullptr);
    }
    if (mf) { CoTaskMemFree(mf); mf = nullptr; }
    if (FAILED(hr)) {
        initErr_ = "端点回环流 Initialize 失败 " + hresultText(hr) +
                   "（该端点可能被独占占用）";
        goto fail;
    }

    hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_);
    if (FAILED(hr) || !capture_) {
        initErr_ = "GetService(IAudioCaptureClient) 失败 " + hresultText(hr);
        goto fail;
    }

    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        initErr_ = "SetEventHandle 失败 " + hresultText(hr);
        goto fail;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        initErr_ = "端点回环流 Start 失败 " + hresultText(hr);
        goto fail;
    }
    started_ = true;

    printf("[Bridge] 采集端点: %u Hz / %u 通道 / %u bit (%s%s)\n",
           fmt_.sampleRate, fmt_.channels, fmt_.bitsPerSample,
           fmt_.isFloat ? "float32" : "PCM",
           exact ? "，端点混音格式" : "，回退格式+AUTOCONVERTPCM");

    AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmTask);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetEvent(readyEvent_);

    while (running_.load() && !failed_.load()) {
        DWORD r = WaitForSingleObject(event_, 200);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) drain();
    }

    cleanupGuarded();
    CoUninitialize();
    return;

fail:
    if (mf) CoTaskMemFree(mf);
    if (dev) dev->Release();
    if (en) en->Release();
    cleanupGuarded();
    SetEvent(readyEvent_);
    CoUninitialize();
}

void WasapiEndpointCapture::drainInner() {
    UINT32 packets = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packets);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        failed_.store(true);
        return;
    }
    if (FAILED(hr)) packets = 0;
    while (packets > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
            failed_.store(true);
            return;
        }
        if (SUCCEEDED(hr) && frames > 0) {
            out2_.resize((size_t)frames * 2);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                memset(out2_.data(), 0, sizeof(float) * out2_.size());
            else {
                convertStereo(data, frames);
                active_.store(true, std::memory_order_release);
            }
            if (cbValid_.load(std::memory_order_acquire)) cb_(out2_.data(), frames);
        }
        if (SUCCEEDED(hr)) capture_->ReleaseBuffer(frames);
        hr = capture_->GetNextPacketSize(&packets);
        if (FAILED(hr)) packets = 0;
    }
}

void WasapiEndpointCapture::drain() {
    __try {
        drainInner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failed_.store(true);
    }
}

void WasapiEndpointCapture::cleanupGuarded() {
    __try {
        releaseCom();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WasapiEndpointCapture::parseFormat(const WAVEFORMATEX* wf) {
    fmt_.sampleRate = wf->nSamplesPerSec;
    fmt_.channels = wf->nChannels;
    fmt_.bitsPerSample = wf->wBitsPerSample;
    fmt_.isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    frameSizeBytes_ = wf->nBlockAlign;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* wfe = (const WAVEFORMATEXTENSIBLE*)wf;
        fmt_.isFloat = IsEqualGUID(wfe->SubFormat, kSubFloat2);
    }
}

// 源格式 → float32 全通道，再抽取前 2 通道写入 out2_（DSP 链为立体声硬编码）
void WasapiEndpointCapture::convertStereo(const BYTE* src, UINT32 frames) {
    const uint16_t ch = fmt_.channels ? fmt_.channels : 2;
    const size_t n = (size_t)frames * ch;
    conv_.resize(n);
    if (fmt_.isFloat) {
        memcpy(conv_.data(), src, n * sizeof(float));
    } else if (fmt_.bitsPerSample == 32) {
        const int32_t* s = (const int32_t*)src;
        for (size_t i = 0; i < n; ++i) conv_[i] = (float)s[i] * (1.0f / 2147483648.0f);
    } else if (fmt_.bitsPerSample == 24) {
        for (size_t i = 0; i < n; ++i) {
            const BYTE* p = src + i * 3;
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            v <<= 8; v >>= 8;
            conv_[i] = (float)v * (1.0f / 8388608.0f);
        }
    } else if (fmt_.bitsPerSample == 16) {
        const int16_t* s = (const int16_t*)src;
        for (size_t i = 0; i < n; ++i) conv_[i] = (float)s[i] * (1.0f / 32768.0f);
    } else {
        memset(conv_.data(), 0, n * sizeof(float));
    }

    out2_.resize((size_t)frames * 2);
    if (ch >= 2) {
        for (size_t f = 0; f < frames; ++f) {
            out2_[f * 2]     = conv_[f * ch];
            out2_[f * 2 + 1] = conv_[f * ch + 1];
        }
    } else if (ch == 1) {
        for (size_t f = 0; f < frames; ++f) {
            out2_[f * 2] = out2_[f * 2 + 1] = conv_[f];
        }
    } else {
        memset(out2_.data(), 0, sizeof(float) * out2_.size());
    }
}
