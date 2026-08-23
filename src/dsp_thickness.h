#pragma once
#include <cmath>
#include <vector>

// ============================================================================
// 300B 音色「厚度与宽度」：预延时只作用于 300B 染色产生的谐波残差（不碰干信号）。
//   谐波 harm = tube(x) - x → 预延时(2~100ms) + 左右声道固定差(宽度档) → 高低切
//   （低切 160Hz -36dB/oct + 高切 8kHz -12dB/oct）→ 混回干信号 x。
//   并联拓扑：干信号零延迟、bit-perfect 不受影响。
// ============================================================================

// 二阶 biquad（RBJ 音频均衡器公式，直接 II 型）
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;  // 已按 a0 归一化
    float z1 = 0.0f, z2 = 0.0f;
    void reset() { z1 = z2 = 0.0f; }
    void setHighpass(float fc, float fs, float Q) {
        float w0 = 6.283185307179586f * fc / fs;
        float cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.0f * Q);
        float a0 = 1.0f + alpha;
        b0 = (1.0f + cw) * 0.5f / a0;
        b1 = -(1.0f + cw) / a0;
        b2 = (1.0f + cw) * 0.5f / a0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void setLowpass(float fc, float fs, float Q) {
        float w0 = 6.283185307179586f * fc / fs;
        float cw = std::cos(w0), sw = std::sin(w0);
        float alpha = sw / (2.0f * Q);
        float a0 = 1.0f + alpha;
        b0 = (1.0f - cw) * 0.5f / a0;
        b1 = (1.0f - cw) / a0;
        b2 = (1.0f - cw) * 0.5f / a0;
        a1 = -2.0f * cw / a0;
        a2 = (1.0f - alpha) / a0;
    }
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// 分数延时线（环形缓冲 + 线性插值 + 一阶平滑防爆音）
struct DelayLine {
    std::vector<float> buf;
    size_t write = 0;
    float delaySamples = 0.0f;   // 平滑后的当前延时(采样数)

    void setup(float maxDelayMs, float fs) {
        size_t n = (size_t)(maxDelayMs * fs / 1000.0f) + 4;
        if (n < 8) n = 8;
        buf.assign(n, 0.0f);
        write = 0;
        delaySamples = 0.0f;
    }
    void reset() {
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = 0.0f;
        write = 0;
        delaySamples = 0.0f;
    }
    float process(float x, float targetDelayMs, float fs) {
        size_t n = buf.size();
        buf[write] = x;
        write = (write + 1) % n;
        float target = targetDelayMs * fs / 1000.0f;
        if (target < 1.0f) target = 1.0f;
        if (target > (float)(n - 4)) target = (float)(n - 4);
        // 一阶平滑：拖动滑块/切档时读指针缓动，避免音高滑移与咔嗒
        delaySamples += (target - delaySamples) * 0.01f;
        float rp = (float)write - delaySamples;
        if (rp < 0.0f) rp += (float)n;
        size_t i0 = (size_t)rp;
        size_t i1 = (i0 + 1) % n;
        float frac = rp - (float)i0;
        return buf[i0] * (1.0f - frac) + buf[i1] * frac;
    }
};

// 每声道一个「厚度与宽度」处理器：预延时 → 6 阶高通(160Hz) → 2 阶低通(8kHz)
struct ThicknessWidth {
    DelayLine delay;
    Biquad hpf[3];   // 3 个二阶级联 = 6 阶 Butterworth(-36dB/oct)
    Biquad lpf;      // 2 阶 Butterworth(-12dB/oct)

    void setup(float fs) {
        delay.setup(140.0f, fs);   // 100ms 延时 + 40ms 宽度差余量
        // 6 阶 Butterworth 三节 Q 值
        hpf[0].setHighpass(160.0f, fs, 0.51763809f);
        hpf[1].setHighpass(160.0f, fs, 0.70710678f);
        hpf[2].setHighpass(160.0f, fs, 1.93185165f);
        lpf.setLowpass(8000.0f, fs, 0.70710678f);
    }
    void reset() {
        delay.reset();
        for (auto& h : hpf) h.reset();
        lpf.reset();
    }
    // 处理谐波：预延时 → 高低切，返回处理后的谐波
    float process(float x, float targetDelayMs, float fs) {
        float y = delay.process(x, targetDelayMs, fs);
        y = hpf[0].process(y);
        y = hpf[1].process(y);
        y = hpf[2].process(y);
        y = lpf.process(y);
        return y;
    }
};

// 宽度档位对应的左右声道延时差(ms)：关闭/俱乐部/音乐厅/太和殿
static const float kWidthOffsets[4] = { 0.0f, 5.0f, 12.0f, 25.0f };
