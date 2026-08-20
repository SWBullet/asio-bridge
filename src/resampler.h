#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

// 分数重采样器（按帧线性插值，双通道交错）：
//   输入 = 追加的交错采样块（帧×2）；输出 = 每轮固定 needFrames 帧。
//   帧位置游标按 ratio 推进（ratio>1 快排、<1 补足），±数百 ppm 级。
//
// 设计要点（吸取历次失败教训）：
//   · 按「帧」插值——L 只在 L 样本间、R 只在 R 样本间插值。
//     此前按「交错采样流」单游标插值，游标会横跨帧对边界，
//     造成 L/R 串扰与随 k 增长的相位漂移（自检 2 的 FAIL 根因之一）；
//   · wantIn() 让调用方精确投喂本轮所需帧数（ceil 对账），
//     相位记账严格连续、永不越界，取代固定读 need 的旧结构；
//   · 插值索引全部落在内部 carry 内并钳制，杜绝越界杂音；
//   · 饥饿时（输入不足）自动保持末帧，输出始终完整。
struct FractionalResampler {
    std::vector<float> carry;      // 交错样本，帧×2
    size_t carryFrames = 0;
    double pos = 0.0;              // 帧位置游标：整数部分=已消耗帧，分数=插值相位
    double ratio = 1.0;

    void setup(size_t needFrames) { carry.resize((needFrames + 16) * 2); }

    void reset() { carryFrames = 0; pos = 0.0; ratio = 1.0; }

    // 本轮回调需投喂的新输入帧数（在 process 之前调用；
    // 与 process 内部丢弃量精确对账，任何情况下不越界）
    size_t wantIn(size_t needFrames) const {
        double w = pos + (double)needFrames * ratio - (double)carryFrames;
        if (w <= 0.0) return 0;
        return (size_t)ceil(w - 1e-12);
    }

    // 返回产出帧数（恒为 needFrames）
    size_t process(const float* srcIn, size_t srcFrames, float* dst, size_t needFrames) {
        // 1) 丢弃已消耗的旧帧（钳制：饥饿记账越界时保持相位连续性）
        size_t consumed = (size_t)pos;
        if (consumed > carryFrames) consumed = carryFrames;
        if (consumed > 0) {
            memmove(carry.data(), carry.data() + consumed * 2,
                    (carryFrames - consumed) * 2 * sizeof(float));
            carryFrames -= consumed;
            pos -= (double)consumed;
        }
        // 2) 追加新输入（容量钳制）
        size_t cap = carry.size() / 2 - carryFrames;
        if (srcFrames > cap) srcFrames = cap;
        memcpy(carry.data() + carryFrames * 2, srcIn, srcFrames * 2 * sizeof(float));
        carryFrames += srcFrames;
        // 3) 按帧线性插值：L、R 各自独立通道。
        //    数据不足（游标触及 carry 末尾）时：输出保持最新帧、游标冻结——
        //    绝不让游标空转（空转会累积无法偿还的输入赤字，wantIn 随之膨胀，
        //    把环形缓冲锁死在 0，形成持续性欠载）。
        double p = pos;
        const size_t lastIdx = carryFrames ? carryFrames - 1 : 0;
        for (size_t j = 0; j < needFrames; ++j) {
            size_t ip = (size_t)p;
            size_t ip1 = ip + 1;
            float* o = dst + j * 2;
            if (carryFrames && ip1 < carryFrames) {
                // 正常插值：ip、ip1 均在 carry 内
                const double frac = p - (double)ip;
                const double w1 = 1.0 - frac;
                const float* c0 = carry.data() + ip * 2;
                const float* c1 = carry.data() + ip1 * 2;
                o[0] = (float)((double)c0[0] * w1 + (double)c1[0] * frac);
                o[1] = (float)((double)c0[1] * w1 + (double)c1[1] * frac);
                p += ratio;
            } else if (carryFrames && ip < carryFrames) {
                // 整点取数：p 恰好落在已有帧上（无插值需求），游标照常推进
                o[0] = carry[ip * 2];
                o[1] = carry[ip * 2 + 1];
                p += ratio;
            } else {
                // 数据不足：保持最新帧，游标冻结（相位连续性不丢）；carry 空则输出静音
                o[0] = carryFrames ? carry[lastIdx * 2] : 0.0f;
                o[1] = carryFrames ? carry[lastIdx * 2 + 1] : 0.0f;
            }
        }
        pos = p;
        // 4) 安全网：滞留过多（罕见瞬态）时丢弃最旧帧
        if (carryFrames > needFrames + 8) {
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
