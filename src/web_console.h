#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// 历史水位采样点（2 秒一采样）
struct HistPoint {
    uint32_t t;        // 秒（GetTickCount64/1000）
    uint32_t wm;       // 水位
    uint32_t target;   // 目标
};

// 300B 传递曲线 XY 轨迹点（输入 x → 输出 y，供控制台观察窗）
struct TubeXYPoint {
    float x;
    float y;
};

// 桥的共享状态指针集：HTTP 控制台线程只读写这些原子量，绝不接触 COM
struct BridgeStatsPtrs {
    std::atomic<uint64_t>* written;
    std::atomic<uint64_t>* consumed;
    std::atomic<uint64_t>* underruns;
    std::atomic<uint64_t>* lastUnderrunAt;   // 最近一次真欠载时刻(ms)，滑动窗口 LED 用
    std::atomic<uint64_t>* dropped;
    std::atomic<float>* peak;
    std::atomic<size_t>* wMult;       // 当前水位目标倍数
    std::atomic<size_t>* floorMult;   // 水位下限倍数（控制台可调）
    std::atomic<double>* driftPpm;    // 最近一次漂移读数
    std::atomic<bool>* needRestart;   // 控制台触发链路重建
    std::atomic<bool>* ditherReq;     // 抖动开关（重建后生效）
    std::atomic<bool>* ditherOn;      // 实际生效状态（控制台显示用）
    std::atomic<bool>* resetReq;      // 重置统计请求（主循环处理）
    std::atomic<bool>* gStop;
    std::atomic<long>* asioRate;
    std::atomic<long>* asioBuffer;
    std::atomic<long>* asioType;
    std::atomic<uint32_t>* capRate;
    std::atomic<uint64_t>* wmNow;        // 实时水位（统计循环直写）
    std::atomic<int>* latencyMs;         // 桥内延迟实测（毫秒，采集→ASIO 驻留时间）
    std::vector<HistPoint>* histBuf;       // 历史环形缓冲（容量 kHistCap）
    std::atomic<uint64_t>* histWrite;      // 已写入条数（发布语义）
    std::atomic<double>* ratioBase;        // 时钟速率锁基值（inHz/outHz）
    std::atomic<double>* inRate;           // 实测输入设备速率 Hz
    std::atomic<double>* outRate;          // 实测输出设备速率 Hz
    std::atomic<bool>* passthrough;        // 直通模式（重采样停用）
    std::atomic<int>* passthroughReq;      // 控制台切换请求：0=无 1=开 2=关
    std::atomic<int>* srcTaps;             // 重采样质量档：0=线性 32=sinc
    std::atomic<bool>* tubeOn;             // 300B 电子管染色开关
    std::atomic<float>* tubeWarmth;        // 染色量 0~1
    std::vector<TubeXYPoint>* tubeXY;      // 300B 传递曲线 XY 轨迹环形缓冲
    std::atomic<uint64_t>* tubeXYWrite;    // XY 轨迹写入计数
    std::atomic<unsigned long>* targetPid; // Bridge 采集目标进程 PID
    std::atomic<bool>* targetActive;       // 目标进程活跃状态
};

constexpr size_t kHistCap = 3600;   // 2 秒一采样 × 3600 = 2 小时

// 启动内嵌 HTTP 控制台（独立线程，纯 socket 无 COM）。失败不致命。
bool startWebConsole(const BridgeStatsPtrs& p, unsigned short port = 3999);
void stopWebConsole();
