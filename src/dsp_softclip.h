#pragma once
#include <cmath>

// 电子管软过载限幅器（Tube Soft-Overload Limiter）
//
// 为什么需要它：桥的输出后端是 ASIO / 独占，绕过了系统端点自带的限幅器
// （见 main.cpp 注释「桥绕过端点限幅器」）。而 DSP 链——300B 电子管染色
// (TubeWarmth 平方律)、厚度/宽度谐波混回、火箭推进器湿增益(+18dB)——会把峰值
// 推过 ±1.0 满刻度。超刻度采样被 DAC 硬切 → 破音。此限幅器在 DSP 链末端
// 兜底，把峰值柔化回满刻度内，杜绝硬削波。
//
// 电子管软过载特性（transfer curve）：
//   - |x| <= 软膝阈值 knee(0.7)：线性直通，完全保真；
//   - |x| > knee：进入电子管饱和(soft saturation)，平滑软拐弯。
//     采用 tanh 软膝：y = knee + (ceil-knee)*tanh((|x|-knee)/(ceil-knee))
//     —— C1 连续（拐点处斜率=1，无瞬态爆裂），且渐近线收敛到 ceil；
//   - ceil = 0.999 既是饱和渐近线也是硬上限：|输出| 永远不超过 0.999，绝不超过 1.0。
//
// 默认常驻、无开关：放在 pullCb 的 DSP 块之后、淡入之后、return 之前，
// 对所有输出样本无条件生效，与后端(ASIO/独占)无关。
struct TubeSoftLimiter {
    static inline float process(float x) {
        const float knee = 0.7f;    // 软膝阈值：以下线性直通（保真）
        const float ceil = 0.999f;  // 饱和渐近线 / 硬上限：永不超过
        const float ax = x < 0.0f ? -x : x;
        if (ax <= knee) return x;   // 透明区：原样通过
        const float denom = ceil - knee;                    // 软膝→上限区间宽度
        float y = knee + denom * std::tanh((ax - knee) / denom);  // 电子管软饱和，渐近 ceil
        if (y > ceil) y = ceil;      // 硬保证（tanh 有限值严格 <1，此处为防御性钳位）
        return x < 0.0f ? -y : y;
    }
};
