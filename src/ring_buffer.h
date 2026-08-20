#pragma once
#include <atomic>
#include <cstddef>

// 单生产者/单消费者环形缓冲（交错 float32 采样）
class RingBuffer {
public:
    RingBuffer() = default;
    ~RingBuffer() { delete[] buf_; }

    bool init(size_t minSamples) {
        size_t s = 1;
        while (s < minSamples) s <<= 1;
        delete[] buf_;
        buf_ = new float[s];
        if (!buf_) return false;
        size_ = s;
        mask_ = s - 1;
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
        return true;
    }

    size_t capacity() const { return size_; }

    // 生产者：写不下的部分被丢弃（溢出保护）
    size_t write(const float* src, size_t n) {
        size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t r = readPos_.load(std::memory_order_acquire);
        size_t freeCnt = size_ - (w - r);
        if (n > freeCnt) n = freeCnt;
        for (size_t i = 0; i < n; ++i)
            buf_[(w + i) & mask_] = src[i];
        writePos_.store(w + n, std::memory_order_release);
        return n;
    }

    size_t read(float* dst, size_t n) {
        size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t w = writePos_.load(std::memory_order_acquire);
        size_t avail = w - r;
        if (n > avail) n = avail;
        for (size_t i = 0; i < n; ++i)
            dst[i] = buf_[(r + i) & mask_];
        readPos_.store(r + n, std::memory_order_release);
        return n;
    }

    size_t available() const {
        return writePos_.load(std::memory_order_acquire) -
               readPos_.load(std::memory_order_acquire);
    }

    // 仅消费者调用：丢弃 n 个采样（水位过高时的漂移校正）
    void discard(size_t n) {
        size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t w = writePos_.load(std::memory_order_acquire);
        size_t avail = w - r;
        if (n > avail) n = avail;
        readPos_.store(r + n, std::memory_order_release);
    }

    void reset() {
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

private:
    float* buf_ = nullptr;
    size_t size_ = 0;
    size_t mask_ = 0;
    // 写入/读取下标分占独立 cache line（alignas(64)），避免生产者/消费者
    // 在相邻同 cache line 上互相伪共享导致每次写入都无效化对方缓存行
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
};
