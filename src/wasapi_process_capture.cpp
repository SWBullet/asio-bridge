#include "wasapi_process_capture.h"
#include "util.h"
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <tlhelp32.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

// SDK 头文件未定义该常量（微软官方样例自带此回退定义）
#ifndef E_ACTIVATE_NOT_COMPLETED
#define E_ACTIVATE_NOT_COMPLETED _HRESULT_TYPEDEF_(0x80071FC7L)
#endif

static const GUID kSubtypeFloat = {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};

// 前向声明：目标端点混音格式（定义于文件尾部）
static WAVEFORMATEX* GetTargetMixFormat(DWORD pid);

// 激活参数：按 PID 的进程回环（含子进程树）。
// 注意必须是外层包装 AUDIOCLIENT_ACTIVATION_PARAMS（带 ActivationType），
// 裸的 AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS 会得到 E_INVALIDARG
static bool BuildActivationParams(DWORD pid, PROPVARIANT& pv, AUDIOCLIENT_ACTIVATION_PARAMS& params) {
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    return true;
}

bool WasapiProcessCapture::open(DWORD pid, DataCallback cb, std::string& err) {
    close();
    initErr_.clear();
    failed_.store(false);
    active_.store(false);
    pid_ = pid;
    cb_ = std::move(cb);
    cbValid_.store(true, std::memory_order_release);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_.store(true);

    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) { err = "创建进程回环采集线程失败"; running_.store(false); return false; }

    DWORD r = WaitForSingleObject(readyEvent_, 8000);
    if (r != WAIT_OBJECT_0) {
        err = "进程回环采集线程初始化超时";
        // 统一生命周期门：close() 会先置 cbValid_=false（阻止线程再触碰 cb_）、
        // 等线程退出、线程若挂死则泄漏句柄而非误关（与 close() 的纪律一致）。
        close();
        return false;
    } else if (!initErr_.empty()) {
        err = initErr_;
        close();
        return false;
    }
    return true;
}

void WasapiProcessCapture::close() {
    // 先切断回调（原子门）：此后采集线程即便仍存活（异常挂死在 COM 调用中），
    // 也不会再触碰 cb_/onError_——而 cb_ 捕获的 per-session 局部变量可能已被主循环析构。
    cbValid_.store(false, std::memory_order_release);
    running_.store(false);
    if (event_) SetEvent(event_);
    if (thread_) {
        DWORD r = WaitForSingleObject(thread_, 15000);
        if (r == WAIT_OBJECT_0) { CloseHandle(thread_); thread_ = nullptr; }
        else {
            printf("[Bridge] 警告: 采集线程 15 秒未退出\n");
            // 线程仍存活：泄漏 event_/readyEvent_ 句柄（置空但不 CloseHandle）。
            // 若关闭，活线程正在 WaitForSingleObject 的句柄会变 use-after-close。
            // 异常挂死极罕见，代价只是泄漏两把句柄（进程退出时回收）。
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

DWORD WINAPI WasapiProcessCapture::threadProc(LPVOID p) {
    ((WasapiProcessCapture*)p)->threadLoop();
    return 0;
}

void WasapiProcessCapture::releaseCom() {
    if (client_ && started_) client_->Stop();
    if (capture_) { capture_->Release(); capture_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
}

void WasapiProcessCapture::threadLoop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        initErr_ = "进程回环线程 CoInitializeEx 失败 " + hresultText(hr);
        SetEvent(readyEvent_);
        return;
    }

    // 1) 按 PID 异步激活回环客户端（正规 COM 完成回调 + 事件等待）
    {
        // 完成回调 handler。必须实现 IAgileObject：Win11 mmdevapi 的
        // ActivateAudioInterfaceWorker::Initialize 会 QI handler 的 IAgileObject，
        // 若返回 E_NOINTERFACE 会被错误映射为 E_OUTOFMEMORY (0x8007000E)。
        // （WinDbg 定位：MMDevAPI!ActivateAudioInterfaceWorker::Initialize+0x1ed）
        class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
        public:
            ActivationHandler() : ref_(1), event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
            ~ActivationHandler() { if (client_) client_->Release(); if (event_) CloseHandle(event_); }
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
                if (riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
                    riid == __uuidof(IAgileObject) || riid == __uuidof(IUnknown)) {
                    *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
            ULONG STDMETHODCALLTYPE Release() override {
                ULONG r = InterlockedDecrement(&ref_);
                if (r == 0) delete this;
                return r;
            }
            HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
                HRESULT r = E_UNEXPECTED;
                IUnknown* unk = nullptr;
                if (SUCCEEDED(op->GetActivateResult(&r, &unk))) {
                    hr_ = r;
                    if (SUCCEEDED(r) && unk) {
                        unk->QueryInterface(__uuidof(IAudioClient), (void**)&client_);
                        unk->Release();
                    }
                } else {
                    hr_ = r;
                }
                SetEvent(event_);
                return S_OK;
            }
            HRESULT Wait(DWORD ms) { WaitForSingleObject(event_, ms); return hr_; }
            LONG ref_ = 1;
            HANDLE event_;
            HRESULT hr_ = E_UNEXPECTED;
            IAudioClient* client_ = nullptr;
        };

        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid_;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        ActivationHandler* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* op = nullptr;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                         __uuidof(IAudioClient), &pv, handler, &op);
        if (FAILED(hr)) {
            handler->Release();
            if (op) op->Release();
            initErr_ = "ActivateAudioInterfaceAsync(PROCESS_LOOPBACK) 失败 " + hresultText(hr) +
                       "（需 Windows 11 22H2+）";
            goto fail;
        }
        if (op) op->Release();   // 完成后由回调处理结果，操作对象不再需要
        hr = handler->Wait(8000);
        client_ = handler->client_;
        handler->client_ = nullptr;   // 所有权移交
        handler->Release();
        if (FAILED(hr) || !client_) {
            initErr_ = "进程回环激活失败 " + hresultText(hr) +
                       "（目标进程不存在或未建立音频流？）";
            goto fail;
        }
    }

    // 2) 混音格式 + 共享模式事件驱动初始化（LOOPBACK 标志）
    //    进程回环客户端的 GetMixFormat 可能返回 E_NOTIMPL（目标进程无活动流时）。
    //    策略：先试 GetMixFormat（位精确，不转换）；失败则回退官方示例的
    //    16bit/2ch/44100 + AUTOCONVERTPCM。
    {
        WAVEFORMATEX fb{};
        WAVEFORMATEX* mf = nullptr;
        WAVEFORMATEX* useFmt = nullptr;
        bool exact = false;
        fmt_.fixedFallback = false;
        hr = client_->GetMixFormat(&mf);
        if (FAILED(hr) || !mf) {
            // 回环客户端自身 GetMixFormat 在本机恒 E_NOTIMPL → 取目标端点
            // 真实引擎混音格式：免 AUTOCONVERTPCM 转换级（速率更稳、位更纯）
            mf = GetTargetMixFormat(pid_);
        }
        if (mf) {
            parseFormat(mf);
            useFmt = mf;
            exact = true;
        } else {
            fb.wFormatTag = WAVE_FORMAT_PCM;
            fb.nChannels = 2;
            fb.nSamplesPerSec = 44100;
            fb.wBitsPerSample = 16;
            fb.nBlockAlign = fb.nChannels * fb.wBitsPerSample / 8;
            fb.nAvgBytesPerSec = fb.nSamplesPerSec * fb.nBlockAlign;
            parseFormat(&fb);
            fmt_.fixedFallback = true;
            useFmt = &fb;
        }

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK |
                                     (exact ? 0 : AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM),
                                 0, 0, useFmt, nullptr);
        if (FAILED(hr) && exact) {
            // 精确格式被引擎拒绝（罕见）→ 回退 16/2/44100 + AUTOCONVERTPCM
            printf("[Bridge] 精确格式 Initialize 失败（%s），回退固定格式\n",
                   hresultText(hr).c_str());
            if (mf) { CoTaskMemFree(mf); mf = nullptr; }
            fb.wFormatTag = WAVE_FORMAT_PCM;
            fb.nChannels = 2;
            fb.nSamplesPerSec = 44100;
            fb.wBitsPerSample = 16;
            fb.nBlockAlign = fb.nChannels * fb.wBitsPerSample / 8;
            fb.nAvgBytesPerSec = fb.nSamplesPerSec * fb.nBlockAlign;
            parseFormat(&fb);
            fmt_.fixedFallback = true;
            useFmt = &fb;
            exact = false;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK |
                                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                     0, 0, useFmt, nullptr);
        }
        if (mf) CoTaskMemFree(mf);
        if (FAILED(hr)) { initErr_ = "回环流 Initialize 失败 " + hresultText(hr); goto fail; }

        hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**)&capture_);
        if (FAILED(hr)) { initErr_ = "GetService(IAudioCaptureClient) 失败 " + hresultText(hr); goto fail; }

        hr = client_->SetEventHandle(event_);
        if (FAILED(hr)) { initErr_ = "SetEventHandle 失败 " + hresultText(hr); goto fail; }

        hr = client_->Start();
        if (FAILED(hr)) { initErr_ = "回环流 Start 失败 " + hresultText(hr); goto fail; }
        started_ = true;
    }

    printf("[Bridge] 捕获 PID %lu: %u Hz / %u 通道 / %u bit (%s%s)\n",
           (unsigned long)pid_, fmt_.sampleRate, fmt_.channels, fmt_.bitsPerSample,
           fmt_.isFloat ? "float32" : "PCM",
           fmt_.fixedFallback ? "，回退格式+AUTOCONVERTPCM" : "，精确混音格式");

    DWORD mmTask = 0;
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
    cleanupGuarded();
    SetEvent(readyEvent_);
    CoUninitialize();
}

void WasapiProcessCapture::drainInner() {
    UINT32 packets = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packets);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        failed_.store(true);
        if (cbValid_.load(std::memory_order_acquire) && onError_) onError_();
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
            if (cbValid_.load(std::memory_order_acquire) && onError_) onError_();
            return;
        }
        if (SUCCEEDED(hr) && frames > 0) {
            conv_.resize((size_t)frames * fmt_.channels);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                memset(conv_.data(), 0, sizeof(float) * conv_.size());
            else
                convert(data, frames, conv_.data());
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                active_.store(true, std::memory_order_release);
            if (cbValid_.load(std::memory_order_acquire)) cb_(conv_.data(), frames);
        }
        if (SUCCEEDED(hr))
            capture_->ReleaseBuffer(frames);
        hr = capture_->GetNextPacketSize(&packets);
        if (FAILED(hr)) packets = 0;
    }
}

void WasapiProcessCapture::drain() {
    __try {
        drainInner();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failed_.store(true);
        if (cbValid_.load(std::memory_order_acquire) && onError_) onError_();
    }
}

void WasapiProcessCapture::cleanupGuarded() {
    __try {
        releaseCom();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WasapiProcessCapture::parseFormat(const WAVEFORMATEX* wf) {
    fmt_.sampleRate = wf->nSamplesPerSec;
    fmt_.channels = wf->nChannels;
    fmt_.bitsPerSample = wf->wBitsPerSample;
    fmt_.isFloat = (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    frameSizeBytes_ = wf->nBlockAlign;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE* wfe = (const WAVEFORMATEXTENSIBLE*)wf;
        fmt_.isFloat = IsEqualGUID(wfe->SubFormat, kSubtypeFloat);
    }
}

void WasapiProcessCapture::convert(const BYTE* src, UINT32 frames, float* dst) {
    const size_t n = (size_t)frames * fmt_.channels;
    if (fmt_.isFloat) {
        memcpy(dst, src, n * sizeof(float));
    } else if (fmt_.bitsPerSample == 32) {
        const int32_t* s = (const int32_t*)src;
        for (size_t i = 0; i < n; ++i) dst[i] = (float)s[i] * (1.0f / 2147483648.0f);
    } else if (fmt_.bitsPerSample == 24) {
        for (size_t i = 0; i < n; ++i) {
            const BYTE* p = src + i * 3;
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            v <<= 8; v >>= 8;
            dst[i] = (float)v * (1.0f / 8388608.0f);
        }
    } else if (fmt_.bitsPerSample == 16) {
        const int16_t* s = (const int16_t*)src;
        for (size_t i = 0; i < n; ++i) dst[i] = (float)s[i] * (1.0f / 32768.0f);
    } else {
        memset(dst, 0, n * sizeof(float));
    }
}

// ISimpleAudioMeter（未文档化接口，SDK 无声明；IID 与 vtable 取自公开资料）
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
ISimpleAudioMeter : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
};

// ---------------------------------------------------------------------------
// COM 枚举委托：桥主线程是 ASIO 的 STA（CoInitializeEx(APARTMENTTHREADED)），
// 直接在其上做 MTA COM 枚举会得到 RPC_E_CHANGED_MODE（静默返回 0 的隐藏 bug，
// 这正是进程回环「自动发现目标」一直失效的原因）。此包装在非 MTA 线程上开
// MTA 工作线程执行并等待结果——仅在会话建立/切换期调用，此刻 ASIO 回调
// 尚未运行或已停止，无 STA 饿死风险。
// ---------------------------------------------------------------------------
template <typename Fn>
static auto RunComOnMta(Fn fn) -> decltype(fn()) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return decltype(fn()){};
    if (hr == RPC_E_CHANGED_MODE) {
        decltype(fn()) result{};
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (done) {
            std::thread t([&] {
                HRESULT h2 = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (SUCCEEDED(h2)) {
                    result = fn();
                    CoUninitialize();
                }
                SetEvent(done);
            });
            WaitForSingleObject(done, 15000);
            t.join();
            CloseHandle(done);
        }
        return result;
    }
    decltype(fn()) result = fn();
    CoUninitialize();
    return result;
}

// ---------------------------------------------------------------------------
// 会话发现：枚举全部活动渲染端点的音频会话，用 ISimpleAudioMeter 峰值
// 找「正在播放」的进程。preferredSubstr 非空时优先匹配（进程名子串）。
// 必须在 MTA 线程上调用（RunComOnMta 保证）。
// ---------------------------------------------------------------------------
static DWORD FindActiveAudioPidInner(const wchar_t* preferredSubstr, float* outPeak) {
    DWORD bestPid = 0;
    float bestPeak = 0.001f;   // 峰值阈值：低于视为静音
    DWORD prefPid = 0;
    float prefPeak = 0.0f;
    // 排除桥自身：进程回环会让引擎把桥注册为「回声会话」，其峰值等于被采
    // 内容——若不禁用，下一次发现会锁死桥自己，再也找不到真正的播放软件
    const DWORD selfPid = GetCurrentProcessId();

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        return 0;
    }
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                        nullptr, (void**)&mgr)) && mgr) {
                IAudioSessionEnumerator* se = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                    int n = 0;
                    se->GetCount(&n);
                    for (int j = 0; j < n; ++j) {
                        IAudioSessionControl* sc = nullptr;
                        if (FAILED(se->GetSession(j, &sc))) continue;
                        IAudioSessionControl2* sc2 = nullptr;
                        DWORD pid = 0;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                            sc2->GetProcessId(&pid);
                            sc2->Release();
                        }
                        ISimpleAudioMeter* meter = nullptr;
                        float peak = 0.0f;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioMeter), (void**)&meter)) && meter) {
                            meter->GetPeakValue(&peak);
                            meter->Release();
                        }
                        if (pid && pid != selfPid && peak > 0.0f) {
                            if (preferredSubstr && wcslen(preferredSubstr) > 0) {
                                wchar_t path[MAX_PATH] = {0};
                                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                                if (hp) {
                                    DWORD len = MAX_PATH;
                                    if (QueryFullProcessImageNameW(hp, 0, path, &len) &&
                                        wcsstr(path, preferredSubstr)) {
                                        if (peak > prefPeak) { prefPeak = peak; prefPid = pid; }
                                    }
                                    CloseHandle(hp);
                                }
                            }
                            if (peak > bestPeak) { bestPeak = peak; bestPid = pid; }
                        }
                        sc->Release();
                    }
                    se->Release();
                }
                mgr->Release();
            }
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    if (outPeak) *outPeak = prefPid ? prefPeak : bestPeak;
    return prefPid ? prefPid : bestPid;
}

// ---------------------------------------------------------------------------
// 目标端点混音格式：进程回环客户端自身的 GetMixFormat 在本机（26200.9168）
// 恒返回 E_NOTIMPL，改为在目标进程最响渲染会话所在的端点（IMMDevice）上
// 取共享混音格式——与引擎实际输出一致，可免 AUTOCONVERTPCM 转换级：
// 位更纯、且少了引擎转换器的速率适应（回环模式速率锁劣化的主因之一）。
// 返回 CoTaskMem 分配的格式，调用方负责 CoTaskMemFree。
// 必须在 MTA 线程调用（采集线程内部使用，已满足）。
// ---------------------------------------------------------------------------
static WAVEFORMATEX* GetTargetMixFormat(DWORD pid) {
    WAVEFORMATEX* result = nullptr;
    if (!pid) return nullptr;

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) return nullptr;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        IMMDevice* bestDev = nullptr;
        float bestPeak = 0.001f;
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                        nullptr, (void**)&mgr)) && mgr) {
                IAudioSessionEnumerator* se = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                    int n = 0;
                    se->GetCount(&n);
                    for (int j = 0; j < n; ++j) {
                        IAudioSessionControl* sc = nullptr;
                        if (FAILED(se->GetSession(j, &sc))) continue;
                        IAudioSessionControl2* sc2 = nullptr;
                        DWORD spid = 0;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2),
                                                         (void**)&sc2)) && sc2) {
                            sc2->GetProcessId(&spid);
                            sc2->Release();
                        }
                        if (spid == pid) {
                            ISimpleAudioMeter* meter = nullptr;
                            float peak = 0.0f;
                            if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioMeter),
                                                             (void**)&meter)) && meter) {
                                meter->GetPeakValue(&peak);
                                meter->Release();
                            }
                            if (peak > bestPeak) {
                                bestPeak = peak;
                                if (bestDev) bestDev->Release();
                                bestDev = dev;
                                dev->AddRef();
                            }
                        }
                        sc->Release();
                    }
                    se->Release();
                }
                mgr->Release();
            }
            dev->Release();
        }
        if (bestDev) {
            IAudioClient* rc = nullptr;
            if (SUCCEEDED(bestDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                            (void**)&rc)) && rc) {
                WAVEFORMATEX* mf = nullptr;
                if (SUCCEEDED(rc->GetMixFormat(&mf)) && mf) result = mf;
                rc->Release();
            }
            bestDev->Release();
        }
        coll->Release();
    }
    en->Release();
    return result;
}

DWORD FindActiveAudioPid(const wchar_t* preferredSubstr, float* outPeak) {
    return RunComOnMta([preferredSubstr, outPeak] {
        return FindActiveAudioPidInner(preferredSubstr, outPeak);
    });
}

// ---------------------------------------------------------------------------
// 目标端点静音（tap→redirect）：进程回环是旁路监听，目标应用原声仍会到达
// 扬声器造成双重声。数值自检证明回环取点在会话静音之后、终点主音量之前：
// 因此静音目标进程（含进程树，与 INCLUDE_TARGET_PROCESS_TREE 对齐）所在
// 渲染端点的终点主音量，既消除双重声又完全不影响捕获；ASIO 不走 WDM
// 终点音量，桥输出不受影响。
// ---------------------------------------------------------------------------
static std::vector<EndpointMuteEntry> MuteTargetEndpointsInner(DWORD pid) {
    std::vector<EndpointMuteEntry> out;
    if (!pid) return out;

    // 进程树成员判定：收集父链，判断会话 PID 是否为目标或其任意后代
    std::unordered_map<DWORD, DWORD> parents;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do { parents[pe.th32ProcessID] = pe.th32ParentProcessID; }
                while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }
    auto inTree = [&](DWORD p) {
        if (p == pid) return true;
        DWORD cur = p;
        for (int hop = 0; hop < 64; ++hop) {
            auto it = parents.find(cur);
            if (it == parents.end() || it->second == 0) return false;
            cur = it->second;
            if (cur == pid) return true;
        }
        return false;
    };

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) return out;
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev))) continue;
            bool hasTarget = false;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                        nullptr, (void**)&mgr)) && mgr) {
                IAudioSessionEnumerator* se = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                    int n = 0;
                    se->GetCount(&n);
                    for (int j = 0; j < n && !hasTarget; ++j) {
                        IAudioSessionControl* sc = nullptr;
                        if (FAILED(se->GetSession(j, &sc))) continue;
                        IAudioSessionControl2* sc2 = nullptr;
                        DWORD spid = 0;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2),
                                                         (void**)&sc2)) && sc2) {
                            sc2->GetProcessId(&spid);
                            sc2->Release();
                        }
                        if (spid && inTree(spid)) hasTarget = true;
                        sc->Release();
                    }
                    se->Release();
                }
                mgr->Release();
            }
            if (hasTarget) {
                IAudioEndpointVolume* ev = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                            nullptr, (void**)&ev)) && ev) {
                    BOOL prev = FALSE;
                    ev->GetMute(&prev);
                    if (prev) {
                        ev->Release();   // 本来已静音，无需记录
                    } else if (SUCCEEDED(ev->SetMute(TRUE, nullptr))) {
                        out.push_back({ ev, FALSE });
                    } else {
                        ev->Release();
                    }
                }
            }
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out;
}

void RestoreEndpointMutes(std::vector<EndpointMuteEntry>& entries) {
    if (entries.empty()) return;
    RunComOnMta([&] {
        for (auto& e : entries) {
            if (e.ev) {
                e.ev->SetMute(e.prevMute, nullptr);
                e.ev->Release();
                e.ev = nullptr;
            }
        }
        return true;
    });
    // 兜底：RunComOnMta 若因 CoInitializeEx/CreateEventW 失败未执行 lambda，
    // 此处 Release 所有残留 ev，避免整批 IAudioEndpointVolume* 泄漏。
    for (auto& e : entries) {
        if (e.ev) { e.ev->Release(); e.ev = nullptr; }
    }
    entries.clear();
}

std::vector<EndpointMuteEntry> MuteTargetEndpoints(DWORD pid) {
    return RunComOnMta([pid] { return MuteTargetEndpointsInner(pid); });
}
