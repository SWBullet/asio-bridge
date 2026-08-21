#pragma once
#include <cmath>

// 300B 电子管温暖染色(带 DC blocker):三极管 3/2 定律 + 单端 A 类
//
// 每声道一个实例(保留直流估计状态)。
// warmth: 0.0 = 直通(干净); 1.0 ≈ 5% 2次谐波 @ 满幅。
// 内部一阶 DC blocker(约 5Hz @44.1k,约 10Hz @88.2k)消除染色引入的信号相关直流偏移
// ——直流偏移 ≈ 0.125·A²/bias,是信号功率的函数,只能用运行平均(状态)跟踪去除。
struct TubeWarmth {
    float dc = 0.0f;   // 直流估计(运行平均,一阶 DC blocker 状态)

    float process(float x, float warmth) {
        if (warmth <= 0.0f) return x;               // 直通(无染色无直流)
        const float bias = 12.5f / (1.0f + 4.0f * warmth);
        const float v = bias + x;
        float y;
        if (v <= 0.0f) y = -bias / 1.5f;            // 截止(正常单端 A 类不触发)
        else {
            const float b = sqrtf(bias);
            y = (v * sqrtf(v) - bias * b) / (1.5f * b);  // (v^1.5 - B^1.5) / (1.5·B^0.5)
        }
        // 一阶 DC blocker:yac = y - dc; dc += alpha·yac; alpha ≈ 2π·5Hz/44100
        const float alpha = 0.00071f;
        const float yac = y - dc;
        dc += alpha * yac;
        return yac;
    }

    void reset() { dc = 0.0f; }
};
