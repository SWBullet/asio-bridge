#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// 时钟速率锁（前馈测量）：
//   从投递/消费回调的 (QPC, 累计采样数) 时间戳环，直接测出
//   输入设备真实速率 inHz 与输出设备真实速率 outHz，
//   ratioBase = inHz / outHz 即为重采样比率的基值。
//
// 设计要点：
//   · QPC 精度：窗口端点取「成对时间戳」，不存在计数分块量化误差
//     （±441 采样/300 秒 ≈ ±33ppm 的量化噪声被彻底消除），
//     300 秒窗口实测精度约 ±2ppm；
//   · 双环结构：速率锁给出基值（前馈、快速），水位闭环只做微调
//     （反馈、±300ppm 内），稳态水位几乎零偏移；
//   · 纯逻辑、无 COM、无锁（单生产者双环 + 顺序计数器），
//     供实时控制与 --pll-test 联合仿真共用。
struct RateLock {
    static constexpr size_t kCap = 2048;   // 每侧 2048 条 ≈ 8 分钟 @4~7 条/秒
    std::vector<std::atomic<uint64_t>> inQpc, inCnt, outQpc, outCnt;
    std::atomic<uint64_t> inPush{ 0 }, outPush{ 0 };

    RateLock() : inQpc(kCap), inCnt(kCap), outQpc(kCap), outCnt(kCap) {}

    // 每次链路重建后调用：清空本会话的采样（比率基值跨会话保留）
    void reset() { inPush.store(0, std::memory_order_relaxed); outPush.store(0, std::memory_order_relaxed); }

    void pushIn(uint64_t qpc, uint64_t samples) {
        uint64_t i = inPush.load(std::memory_order_relaxed);
        inCnt[i % kCap].store(samples, std::memory_order_relaxed);
        inQpc[i % kCap].store(qpc, std::memory_order_release);
        inPush.store(i + 1, std::memory_order_relaxed);
    }
    void pushOut(uint64_t qpc, uint64_t samples) {
        uint64_t i = outPush.load(std::memory_order_relaxed);
        outCnt[i % kCap].store(samples, std::memory_order_relaxed);
        outQpc[i % kCap].store(qpc, std::memory_order_release);
        outPush.store(i + 1, std::memory_order_relaxed);
    }

    // 单侧速率：对最近窗口内的 (时间, 累计采样) 点做最小二乘回归取斜率。
    // 端点取「成对时间戳」，不存在计数分块量化误差；OLS 进一步把窗口边界
    // 落在两次推送之间的量化彻底消除。
    // 窗口长度与断档拒绝可参数化：连续交付设备用 300 秒长窗
    // （±3ppm）；交付易受暂停/恢复突发污染的源（进程回环）用 60 秒短窗
    // 快速淘汰脏数据（±16ppm 但秒级收敛，远好于被污染的 ±700ppm 假读数）。
    static bool rateOf(const std::vector<std::atomic<uint64_t>>& qpc,
                       const std::vector<std::atomic<uint64_t>>& cnt,
                       uint64_t push, double qpcFreq, double* hz,
                       double winSec = 300.0, double gapSec = 0.5) {
        if (push < 16) return false;
        const size_t cap = qpc.size();
        const uint64_t last = push - 1;
        const uint64_t q0 = qpc[last % cap].load(std::memory_order_acquire);
        const uint64_t c0 = cnt[last % cap].load(std::memory_order_relaxed);
        const uint64_t avail = push < cap ? push : cap;
        const uint64_t first = last + 1 - avail;
        const uint64_t win = (uint64_t)(winSec * qpcFreq);
        // 交付断档检测（进程回环关键）：目标应用暂停/恢复时回环包出现断档，
        // 恢复瞬间引擎可能突发补送，窗口斜率被污染（实测曾出现 -710ppm 的
        // 假读数，而晶振漂移不可能超过 ±100ppm）。遇到断档直接截断窗口：
        // 回归只使用断档之后的清洁段，且要求清洁段 ≥ cleanNeed 秒。
        const uint64_t gapLimit = (uint64_t)(gapSec * qpcFreq);
        const double cleanNeed = 15.0;   // 清洁段最少 15 秒（频繁切歌也能出数）
        const double spanNeed = winSec <= 60.0 ? cleanNeed : 60.0;
        uint64_t prevQ = q0;
        uint64_t newestGapQ = 0;   // 断档后第一点的时间（清洁段起点）
        double sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
        size_t n = 0;
        double span = 0.0;   // 实际回归跨度（秒）
        for (uint64_t k = last;; --k) {
            uint64_t q = qpc[k % cap].load(std::memory_order_acquire);
            uint64_t c = cnt[k % cap].load(std::memory_order_relaxed);
            if (q0 - q > win) break;
            if (prevQ - q > gapLimit) {
                // 断档：截断——此点及更早的点属于断档前，不参与回归
                newestGapQ = prevQ;
                break;
            }
            prevQ = q;
            double x = -(double)(q0 - q) / qpcFreq;   // ≤ 0，相对最新点的秒数
            double y = (double)c;
            sx += x; sy += y; sxy += x * y; sxx += x * x;
            ++n;
            span = -x;
            if (k == first) break;
        }
        if (newestGapQ != 0 && q0 - newestGapQ < cleanNeed * qpcFreq) return false;
        if (n < 32 || span < spanNeed) return false;
        double denom = (double)n * sxx - sx * sx;
        if (denom <= 0.0) return false;
        double slope = ((double)n * sxy - sx * sy) / denom;   // 采样/秒
        if (slope < 1000.0 || slope > 200000.0) return false;
        *hz = slope;
        return true;
    }

    bool update(double qpcFreq, double* inHz, double* outHz,
                double winSec = 300.0, double gapSec = 0.5) const {
        bool a = rateOf(inQpc, inCnt, inPush.load(std::memory_order_relaxed),
                        qpcFreq, inHz, winSec, gapSec);
        bool b = rateOf(outQpc, outCnt, outPush.load(std::memory_order_relaxed),
                        qpcFreq, outHz, winSec, gapSec);
        return a && b;
    }
};
