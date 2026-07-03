#pragma once
#include "Order.h"
#include "MemoryPool.h"

#include <deque>
#include <vector>
#include <unordered_map>
#include <ostream>

// ---------------------------------------------------------------------------
// OrderBook (advanced version)
//
// Improvements over a std::map<price, deque<shared_ptr<Order>>> design:
//
//  1. PRICE LADDER INSTEAD OF A TREE
//     Prices are converted to integer ticks (tick = round(price / TICK_SIZE))
//     and each side of the book is a std::deque<PriceLevel> that is directly
//     INDEXED by (tick - baseTick_). Looking up "the level for this price"
//     is therefore O(1) array indexing, not an O(log N) tree descent, and
//     the deque's chunked contiguous storage is far more cache-friendly than
//     following red-black tree node pointers scattered across the heap.
//     The ladder grows at either end (push_front/push_back, O(1) amortized)
//     as orders arrive outside the current tick range.
//
//  2. INTRUSIVE LINKED LISTS OVER POOL INDICES
//     Each PriceLevel keeps a head/tail INDEX into a shared MemoryPool<Order>
//     rather than a container of shared_ptr. FIFO time-priority is
//     maintained by appending to the tail and matching from the head --
//     O(1) either way -- with zero heap allocation per order (see
//     MemoryPool.h) and zero atomic refcount traffic (see: no shared_ptr).
//
//  3. BEST-PRICE ACCESS
//     The best bid/ask is tracked incrementally (bestBidIdx_/bestAskIdx_)
//     rather than recomputed by scanning, so the hottest possible operation
//     -- "what do I match against right now" -- stays O(1).
// ---------------------------------------------------------------------------
class OrderBook {
public:
    static constexpr double TICK_SIZE = 0.01;

    explicit OrderBook(MemoryPool<Order>& pool, uint32_t symbolId)
        : pool_(pool), symbolId_(symbolId) {}

    static int64_t priceToTick(double price) {
        return static_cast<int64_t>(price / TICK_SIZE + (price >= 0 ? 0.5 : -0.5));
    }
    static double tickToPrice(int64_t tick) {
        return static_cast<double>(tick) * TICK_SIZE;
    }

    // Adds a new order, matching immediately against the resting book.
    // Appends generated trades to `tradesOut`. `nowNs` is used to timestamp trades.
    void addOrder(uint64_t id, Side side, double price, uint64_t quantity,
                  uint64_t nowNs, std::vector<Trade>& tradesOut);

    bool cancelOrder(uint64_t id);

    size_t restingOrderCount() const { return orderLocation_.size(); }

    bool bestBidTick(int64_t& tickOut, uint64_t& qtyOut) const;
    bool bestAskTick(int64_t& tickOut, uint64_t& qtyOut) const;

    void printBookSummary(std::ostream& os, size_t depth = 5) const;

private:
    struct PriceLevel {
        uint32_t headIdx = Order::NIL;
        uint32_t tailIdx = Order::NIL;
        uint64_t totalQty = 0;
        uint32_t orderCount = 0;
        bool active() const { return orderCount > 0; }
    };

    struct Ladder {
        std::deque<PriceLevel> levels;
        int64_t baseTick = 0;
        bool initialized = false;

        // Ensures a level for `tick` exists (growing the deque as needed)
        // and returns its index within `levels`.
        size_t indexFor(int64_t tick) {
            if (!initialized) {
                baseTick = tick;
                levels.emplace_back();
                initialized = true;
                return 0;
            }
            if (tick < baseTick) {
                size_t grow = static_cast<size_t>(baseTick - tick);
                for (size_t i = 0; i < grow; ++i) levels.emplace_front();
                baseTick = tick;
                return 0;
            }
            size_t idx = static_cast<size_t>(tick - baseTick);
            while (idx >= levels.size()) levels.emplace_back();
            return idx;
        }
    };

    struct Location {
        Side side;
        int64_t tick;
        uint32_t poolIdx; // direct index into the shared MemoryPool -> O(1) cancel
    };

    void appendToLevel(PriceLevel& level, uint32_t orderIdx);
    void removeFromLevel(PriceLevel& level, uint32_t orderIdx);

    // Finds the best (lowest for asks / highest for bids) active level's
    // deque index, or SIZE_MAX if none. Advances a cached cursor so repeated
    // calls during a matching sweep are O(1) amortized rather than O(N).
    size_t bestAskLevelIndex();
    size_t bestBidLevelIndex();

    MemoryPool<Order>& pool_;
    uint32_t symbolId_;

    Ladder bidLadder_; // index i => tick (bidLadder_.baseTick + i), higher index = higher tick
    Ladder askLadder_;

    std::unordered_map<uint64_t, Location> orderLocation_;
};
