#include "volume_map.h"
#include "util.h"
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <cstdio>

bool VolumeMapper::open(const std::wstring& srcId, const std::wstring& dstId, std::string& err) {
    close();
    initErr_.clear();
    srcId_ = srcId;
    dstId_ = dstId;
    lastVol_.store(-1.0f, std::memory_order_relaxed);
    syncCount_.store(0, std::memory_order_relaxed);
    lastErr_.store(0, std::memory_order_relaxed);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent_) { err = "创建音量映射事件失败"; return false; }
    running_.store(true);

    thread_ = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!thread_) {
        err = "创建音量映射线程失败";
        running_.store(false);
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
        return false;
    }

    DWORD r = WaitForSingleObject(readyEvent_, 5000);
    if (r != WAIT_OBJECT_0) {
        err = "音量映射线程初始化超时";
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

void VolumeMapper::close() {
    running_.store(false);
    if (thread_) {
        DWORD r = WaitForSingleObject(thread_, 5000);
        if (r == WAIT_OBJECT_0) { CloseHandle(thread_); thread_ = nullptr; }
        else {
            printf("[音量映射] 警告: 映射线程 5 秒未退出\n");
            thread_ = nullptr;
            readyEvent_ = nullptr;   // 泄漏而非误关
            return;
        }
    }
    if (readyEvent_) { CloseHandle(readyEvent_); readyEvent_ = nullptr; }
    active_.store(false, std::memory_order_release);
}

DWORD WINAPI VolumeMapper::threadProc(LPVOID p) {
    ((VolumeMapper*)p)->loop();
    return 0;
}

void VolumeMapper::loop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        initErr_ = "音量映射线程 CoInitializeEx 失败 " + hresultText(hr);
        SetEvent(readyEvent_);
        return;
    }

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* sdev = nullptr;
    IMMDevice* ddev = nullptr;
    IAudioEndpointVolume* srcEv = nullptr;
    IAudioEndpointVolume* dstEv = nullptr;
    float lastV = -1.0f;
    BOOL lastM = FALSE;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) {
        initErr_ = "音量映射创建 MMDeviceEnumerator 失败 " + hresultText(hr);
        goto fail;
    }
    hr = en->GetDevice(srcId_.c_str(), &sdev);
    if (FAILED(hr) || !sdev) {
        initErr_ = "音量映射找不到采集源端点 " + hresultText(hr);
        goto fail;
    }
    hr = en->GetDevice(dstId_.c_str(), &ddev);
    if (FAILED(hr) || !ddev) {
        initErr_ = "音量映射找不到输出端点 " + hresultText(hr);
        goto fail;
    }
    hr = sdev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&srcEv);
    if (FAILED(hr) || !srcEv) {
        initErr_ = "音量映射激活采集源 IAudioEndpointVolume 失败 " + hresultText(hr);
        goto fail;
    }
    hr = ddev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&dstEv);
    if (FAILED(hr) || !dstEv) {
        initErr_ = "音量映射激活输出 IAudioEndpointVolume 失败 " + hresultText(hr);
        goto fail;
    }

    // 初始同步：把输出端点直接对齐到采集源端点当前音量/静音态
    {
        float v = 1.0f;
        BOOL m = FALSE;
        if (SUCCEEDED(srcEv->GetMasterVolumeLevelScalar(&v)) &&
            SUCCEEDED(srcEv->GetMute(&m))) {
            dstEv->SetMasterVolumeLevelScalar(v, nullptr);
            dstEv->SetMute(m, nullptr);
            lastV = v;
            lastM = m;
            lastVol_.store(v, std::memory_order_relaxed);
        }
    }
    active_.store(true, std::memory_order_release);
    printf("[音量映射] 已启用：系统音量（默认设备=采集源端点）→ 桥输出端点，"
           "滑块与静音键实时生效\n");
    SetEvent(readyEvent_);

    while (running_.load()) {
        Sleep(120);
        float v = 0.0f;
        BOOL m = FALSE;
        HRESULT h1 = srcEv->GetMasterVolumeLevelScalar(&v);
        HRESULT h2 = srcEv->GetMute(&m);
        if (FAILED(h1) || FAILED(h2)) {
            lastErr_.store((int)(FAILED(h1) ? h1 : h2), std::memory_order_relaxed);
            break;   // 采集源端点失效（拔出/禁用）→ 退出，等主循环重建
        }
        bool volChanged = (lastV < 0.0f) || (v > lastV + 0.002f) || (v < lastV - 0.002f);
        bool muteChanged = (m != lastM);
        if (volChanged || muteChanged) {
            HRESULT h3 = dstEv->SetMasterVolumeLevelScalar(v, nullptr);
            HRESULT h4 = dstEv->SetMute(m, nullptr);
            if (FAILED(h3) || FAILED(h4)) {
                lastErr_.store((int)(FAILED(h3) ? h3 : h4), std::memory_order_relaxed);
                break;
            }
            lastV = v;
            lastM = m;
            lastVol_.store(v, std::memory_order_relaxed);
            syncCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    active_.store(false, std::memory_order_release);
    if (dstEv) { dstEv->Release(); dstEv = nullptr; }
    if (srcEv) { srcEv->Release(); srcEv = nullptr; }
    if (ddev) { ddev->Release(); ddev = nullptr; }
    if (sdev) { sdev->Release(); sdev = nullptr; }
    if (en) { en->Release(); en = nullptr; }
    CoUninitialize();
    return;

fail:
    active_.store(false, std::memory_order_release);
    if (dstEv) dstEv->Release();
    if (srcEv) srcEv->Release();
    if (ddev) ddev->Release();
    if (sdev) sdev->Release();
    if (en) en->Release();
    SetEvent(readyEvent_);
    CoUninitialize();
}
