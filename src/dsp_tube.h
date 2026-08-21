#pragma once
#include <cmath>

// FET 温暖染色(平方律)+ DC blocker
//
// FET 场效应管的转移特性是平方律(饱和区):Id = K·(Vgs - Vth)²。
// 比三极管的 3/2 定律多一倍 2 次谐波(2次/基波 ≈ 0.25·A/bias,3/2 定律为 0.125·A/bias)。
//
// 每声道一个实例(保留直流估计状态)。
// warmth: 0.0 = 直通(干净); 1.0 ≈ 10% 2次谐波 @ 满幅。
// 内部一阶 DC blocker(约 5Hz @44.1k)消除染色引入的信号相关直流偏移。
struct TubeWarmth {
    float dc = 0.0f;   // 直流估计(运行平均,一阶 DC blocker 状态)

    float process(float x, float warmth) {
        if (warmth <= 0.0f) return x;               // 直通(无染色无直流)
        const float bias = 12.5f / (1.0f + 4.0f * warmth);
        const float v = bias + x;
        float y;
        if (v <= 0.0f) y = -bias / 2.0f;            // 截止(正常不触发)
        else y = (v * v - bias * bias) / (2.0f * bias);  // 平方律: (v² - B²)/(2B),小信号增益=1
        // 一阶 DC blocker:yac = y - dc; dc += alpha·yac; alpha ≈ 2π·5Hz/44100
        const float alpha = 0.00071f;
        const float yac = y - dc;
        dc += alpha * yac;
        return yac;
    }

    void reset() { dc = 0.0f; }
};
