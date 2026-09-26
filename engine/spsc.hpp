// Bounded single-producer / single-consumer ring. Head and tail on separate cache
// lines, each side caches the other's index to avoid cross-core traffic per op.
#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

template <typename T>
class Spsc {
public:
    explicit Spsc(size_t cap_pow2) : mask_(cap_pow2 - 1), buf_(cap_pow2) {}

    bool push(const T& v) {                                   // producer only
        size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_cache_ > mask_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h - tail_cache_ > mask_) return false;        // full
        }
        buf_[h & mask_] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {                                        // consumer only
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t == head_cache_) return false;               // empty
        }
        out = buf_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    const size_t mask_;
    std::vector<T> buf_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) size_t tail_cache_ = 0;      // producer's view of tail
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) size_t head_cache_ = 0;      // consumer's view of head
};
