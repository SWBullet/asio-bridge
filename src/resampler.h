#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

// 分数重采样器（双通道交错 float32）：
//   输入 = 追加的交错采样块（帧×2）；输出 = 每轮固定 needFrames 帧。
//   帧位置游标按 ratio 推进（ratio>1 快排、<1 补足），±数百 ppm 级。
//
// 质量档（taps）：
//   0 = 线性（2 点，低延迟，默认）；
//   32 = 32-tap 汉宁窗 sinc（高精度，分数延迟 FIR，时钟漂移级重采样下
//        THD 比线性低约两个数量级）。
//
// 设计要点（吸取历次失败教训）：
//   · 按「帧」插值——L 只在 L 样本间、R 只在 R 样本间插值；
//   · wantIn() 让调用方精确投喂本轮所需帧数（含 sinc 前瞻窗），相位连续不越界；
//   · sinc 档保留半窗（16 帧）回看历史，插值索引全部落在 carry 内并钳制；
//   · 饥饿时（输入不足）保持末帧、游标冻结——绝不让游标空转。
struct FractionalResampler {
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr int kHalf = 16;            // sinc 半窗（lookback=16，lookahead=15）
    static constexpr int kTaps = 32;

    std::vector<float> carry;      // 交错样本，帧×2
    size_t carryFrames = 0;
    double pos = 0.0;              // 帧位置游标：整数部分=已消耗帧，分数=插值相位
    double ratio = 1.0;
    int taps = 0;                  // 0=线性，32=sinc

    // 预计算 sinc 系数表（256 档 frac × 32 tap，归一化）——热循环零超越函数
    static const double (*sincTable())[32] {
        static double tbl[256][32];
        static bool done = false;
        if (!done) {
            for (int i = 0; i < 256; ++i) {
                const double frac = (double)i / 256.0;
                double s = 0.0;
                for (int k = -16; k < 16; ++k) {
                    const double t = (double)k - frac;
                    const double sv = (t == 0.0) ? 1.0 : sin(kPi * t) / (kPi * t);
                    const double wv = 0.5 * (1.0 + cos(kPi * t / 16.0));
                    tbl[i][k + 16] = sv * wv;
                    s += tbl[i][k + 16];
                }
                if (s != 0.0)
                    for (int k = 0; k < 32; ++k) tbl[i][k] /= s;
            }
            done = true;
        }
        return tbl;
    }

    void setup(size_t needFrames) { carry.resize((needFrames + kTaps + 8) * 2); }

    void reset() { carryFrames = 0; pos = 0.0; ratio = 1.0; }

    // 本轮回调需投喂的新输入帧数（在 process 之前调用；
    // 与 process 内部丢弃量精确对账，任何情况下不越界）
    size_t wantIn(size_t needFrames) const {
        const double lookahead = (taps > 0) ? (double)(kHalf - 1) : 0.0;
        double w = pos + (double)needFrames * ratio + lookahead - (double)carryFrames;
        if (w <= 0.0) return 0;
        return (size_t)ceil(w - 1e-12);
    }

    // 返回产出帧数（恒为 needFrames）
    size_t process(const float* srcIn, size_t srcFrames, float* dst, size_t needFrames) {
        // 1) 丢弃已消耗的旧帧（sinc 档保留半窗回看历史；钳制饥饿记账越界）
        size_t consumed = (size_t)pos;
        if (consumed > carryFrames) consumed = carryFrames;
        size_t keep = (taps > 0) ? kHalf : 0;
        if (keep > consumed) keep = consumed;
        if (consumed > keep) {
            memmove(carry.data(), carry.data() + (consumed - keep) * 2,
                    (carryFrames - (consumed - keep)) * 2 * sizeof(float));
            carryFrames -= (consumed - keep);
            pos -= (double)(consumed - keep);
        }
        // 2) 追加新输入（容量钳制）
        size_t cap = carry.size() / 2 - carryFrames;
        if (srcFrames > cap) srcFrames = cap;
        memcpy(carry.data() + carryFrames * 2, srcIn, srcFrames * 2 * sizeof(float));
        carryFrames += srcFrames;
        // 3) 插值
        double p = pos;
        const size_t lastIdx = carryFrames ? carryFrames - 1 : 0;
        for (size_t j = 0; j < needFrames; ++j) {
            size_t ip = (size_t)p;
            float* o = dst + j * 2;
            if (taps > 0) {
                // sinc：前瞻需满足 ip + kHalf-1 < carryFrames
                if (ip + (size_t)(kHalf - 1) < carryFrames) {
                    if (ip < (size_t)kHalf) {
                        // 启动过渡：回看不足（前 16 帧），最近邻取数并推进游标
                        o[0] = carry[ip * 2];
                        o[1] = carry[ip * 2 + 1];
                        p += ratio;
                    } else {
                        const double frac = p - (double)ip;
                        size_t fi = (size_t)(frac * 256.0);
                        if (fi >= 256) fi = 255;
                        const double* hh = sincTable()[fi];
                        double yl = 0.0, yr = 0.0;
                        for (int k = -kHalf; k < kHalf; ++k) {
                            const float* c = carry.data() + (ip + k) * 2;
                            const double w = hh[k + kHalf];
                            yl += (double)c[0] * w;
                            yr += (double)c[1] * w;
                        }
                        o[0] = (float)yl;
                        o[1] = (float)yr;
                        p += ratio;
                    }
                } else {
                    // 数据不足：保持最新帧，游标冻结（相位连续性不丢）
                    o[0] = carryFrames ? carry[lastIdx * 2] : 0.0f;
                    o[1] = carryFrames ? carry[lastIdx * 2 + 1] : 0.0f;
                }
            } else {
                // 线性：2 点插值
                size_t ip1 = ip + 1;
                if (carryFrames && ip1 < carryFrames) {
                    const double frac = p - (double)ip;
                    const double w1 = 1.0 - frac;
                    const float* c0 = carry.data() + ip * 2;
                    const float* c1 = carry.data() + ip1 * 2;
                    o[0] = (float)((double)c0[0] * w1 + (double)c1[0] * frac);
                    o[1] = (float)((double)c0[1] * w1 + (double)c1[1] * frac);
                    p += ratio;
                } else if (carryFrames && ip < carryFrames) {
                    o[0] = carry[ip * 2];
                    o[1] = carry[ip * 2 + 1];
                    p += ratio;
                } else {
                    o[0] = carryFrames ? carry[lastIdx * 2] : 0.0f;
                    o[1] = carryFrames ? carry[lastIdx * 2 + 1] : 0.0f;
                }
            }
        }
        pos = p;
        // 4) 安全网：滞留过多（罕见瞬态）时丢弃最旧帧
        if (carryFrames > needFrames + kTaps) {
            size_t drop = carryFrames - needFrames;
            memmove(carry.data(), carry.data() + drop * 2,
                    (carryFrames - drop) * 2 * sizeof(float));
            carryFrames -= drop;
            pos -= (double)drop;
            if (pos < 0.0) pos = 0.0;
        }
        return needFrames;
    }
};
