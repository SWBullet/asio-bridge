#pragma once
#include <string>
#include <vector>

// 输出设备条目：控制台设备列表的一项
struct DeviceEntry {
    std::wstring name;        // 端点友好名
    std::wstring id;          // 端点 ID (IMMDevice::GetId)
    bool         asio = false;// true=走 ASIO 输出, false=走 WASAPI 独占
    std::string  asioDriver;  // asio=true 时的 ASIO 驱动名
};

// 扫描所有活动渲染端点 + ASIO 驱动，做名字匹配。
// 内部在 MTA 线程执行(避开桥主线程 STA 的 RPC_E_CHANGED_MODE)。
// 失败时返回空列表并填 err。
std::vector<DeviceEntry> ScanOutputDevices(std::string& err);

// 纯注册表枚举 ASIO 驱动名(不加载驱动, 无 COM 依赖)
std::vector<std::string> ListAsioDriverNames();
