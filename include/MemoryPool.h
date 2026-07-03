#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

// ---------------------------------------------------------------------------
// MemoryPool<T>
//
// A fixed-capacity object pool. All storage is allocated ONCE up front
// (a single contiguous std::vector<T>), and objects are handed out / taken
// back via an O(1) free-index stack. This is the standard technique real
// low-latency systems use to avoid calling malloc/new on the hot path --
// heap allocation has unpredictable latency (the allocator may take a lock,
// walk free lists, or trigger a syscall) which is unacceptable when you're
// trying to hold tail latency to nanoseconds/low-microseconds.
//
// Contiguous storage also means "iterate all live orders" style operations
// have good cache locality, unlike a set of individually heap-allocated
// nodes (e.g. shared_ptr<Order> scattered across the heap) which is what a
// naive implementation -- and the previous version of this project -- uses.
// ---------------------------------------------------------------------------
template <typename T>
class MemoryPool {
public:
    static constexpr uint32_t NIL = 0xFFFFFFFFu;

    explicit MemoryPool(size_t capacity) : storage_(capacity), inUse_(capacity, false) {
        freeList_.reserve(capacity);
        // Push in reverse so index 0 is handed out first (nicer for debugging).
        for (size_t i = capacity; i-- > 0; ) {
            freeList_.push_back(static_cast<uint32_t>(i));
        }
    }

    // Acquires a slot, constructs T in place with the given args, returns its index.
    template <typename... Args>
    uint32_t acquire(Args&&... args) {
        if (freeList_.empty()) {
            throw std::runtime_error("MemoryPool exhausted - increase capacity");
        }
        uint32_t idx = freeList_.back();
        freeList_.pop_back();
        storage_[idx] = T(std::forward<Args>(args)...);
        inUse_[idx] = true;
        return idx;
    }

    // Returns a slot to the pool. O(1), no destructor call needed for POD-ish T.
    void release(uint32_t idx) {
        inUse_[idx] = false;
        freeList_.push_back(idx);
    }

    T& get(uint32_t idx) { return storage_[idx]; }
    const T& get(uint32_t idx) const { return storage_[idx]; }

    bool isInUse(uint32_t idx) const { return inUse_[idx]; }
    size_t capacity() const { return storage_.size(); }
    size_t freeCount() const { return freeList_.size(); }
    size_t liveCount() const { return storage_.size() - freeList_.size(); }

private:
    std::vector<T> storage_;
    std::vector<uint8_t> inUse_;
    std::vector<uint32_t> freeList_;
};
