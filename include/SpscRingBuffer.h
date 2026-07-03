#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// SpscRingBuffer<T>
//
// A lock-free, wait-free single-producer/single-consumer bounded queue.
// This is the standard pattern used to hand work off between a "feed
// handler" thread (parsing/receiving incoming order messages) and a
// "matching engine" thread (applying them to the book) without either
// thread ever blocking on a mutex.
//
// Correctness relies on:
//   - Capacity is a power of two, so index wraparound is a cheap bitmask
//     instead of a modulo/division.
//   - head_ is only ever written by the consumer, tail_ only by the
//     producer -- each thread privately caches the other's cursor to avoid
//     re-reading a shared atomic (and the cache-line bouncing that causes)
//     on every single push/pop.
//   - acquire/release memory ordering on the shared atomics is exactly
//     what's needed (and no more) to publish a written slot to the other
//     thread, which is what makes this safe without a lock.
// ---------------------------------------------------------------------------
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacityPow2)
        : mask_(capacityPow2 - 1), buffer_(capacityPow2) {
        // capacity must be a power of two for the bitmask trick to work.
    }

    // Producer side. Returns false if the queue is full.
    bool push(const T& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t nextTail = (tail + 1) & mask_;
        if (nextTail == cachedHead_) {
            cachedHead_ = head_.load(std::memory_order_acquire);
            if (nextTail == cachedHead_) {
                return false; // full
            }
        }
        buffer_[tail] = item;
        tail_.store(nextTail, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool pop(T& out) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == cachedTail_) {
            cachedTail_ = tail_.load(std::memory_order_acquire);
            if (head == cachedTail_) {
                return false; // empty
            }
        }
        out = buffer_[head];
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    const size_t mask_;
    std::vector<T> buffer_;

    alignas(64) std::atomic<size_t> head_{0}; // consumer-owned, own cache line
    alignas(64) std::atomic<size_t> tail_{0}; // producer-owned, own cache line

    // Each side privately caches the other's cursor so the hot path usually
    // doesn't need to touch the other thread's atomic (and its cache line)
    // at all -- only when it looks like the buffer might be full/empty.
    size_t cachedHead_ = 0;
    size_t cachedTail_ = 0;
};
