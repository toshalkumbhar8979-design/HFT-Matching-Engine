#pragma once
#include <cstdint>

enum class Side : uint8_t { BUY, SELL };

// An order slot managed by MemoryPool<Order>. Instead of separate heap
// allocations linked by shared_ptr (the previous version's approach), each
// order lives in contiguous pool storage and participates in its price
// level's doubly-linked list via raw pool INDICES (not pointers) -- this
// keeps the struct trivially relocatable/copyable within the pool's vector
// and avoids any dynamic allocation for list bookkeeping.
struct Order {
    static constexpr uint32_t NIL = 0xFFFFFFFFu;

    uint64_t id = 0;
    Side     side = Side::BUY;
    int64_t  tick = 0;        // price expressed as an integer tick (price / TICK_SIZE)
    uint64_t quantity = 0;    // remaining unfilled quantity
    uint32_t symbolId = 0;
    uint64_t enqueueNs = 0;   // timestamp when the order entered the SPSC queue

    // Intrusive doubly-linked list within its PriceLevel (FIFO time priority).
    uint32_t prevIdx = NIL;
    uint32_t nextIdx = NIL;

    Order() = default;
    Order(uint64_t id_, Side side_, int64_t tick_, uint64_t qty_, uint32_t symbolId_, uint64_t enqueueNs_)
        : id(id_), side(side_), tick(tick_), quantity(qty_), symbolId(symbolId_), enqueueNs(enqueueNs_) {}
};

struct Trade {
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    int64_t  tick;
    uint64_t quantity;
    uint32_t symbolId;
    uint64_t matchNs; // timestamp the match was completed
};
