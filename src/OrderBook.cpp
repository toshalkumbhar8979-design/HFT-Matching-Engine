#include "OrderBook.h"
#include <algorithm>
#include <iomanip>

void OrderBook::appendToLevel(PriceLevel& level, uint32_t orderIdx) {
    Order& o = pool_.get(orderIdx);
    o.prevIdx = level.tailIdx;
    o.nextIdx = Order::NIL;
    if (level.tailIdx != Order::NIL) {
        pool_.get(level.tailIdx).nextIdx = orderIdx;
    } else {
        level.headIdx = orderIdx;
    }
    level.tailIdx = orderIdx;
    level.totalQty += o.quantity;
    level.orderCount++;
}

void OrderBook::removeFromLevel(PriceLevel& level, uint32_t orderIdx) {
    Order& o = pool_.get(orderIdx);
    if (o.prevIdx != Order::NIL) pool_.get(o.prevIdx).nextIdx = o.nextIdx;
    else level.headIdx = o.nextIdx;

    if (o.nextIdx != Order::NIL) pool_.get(o.nextIdx).prevIdx = o.prevIdx;
    else level.tailIdx = o.prevIdx;

    level.totalQty -= o.quantity;
    level.orderCount--;
}

// Trims dead levels off the front of the ask ladder (lowest tick = best ask)
// so the front of the deque always points at the real best price. Amortized
// O(1): each level is trimmed at most once over its lifetime.
size_t OrderBook::bestAskLevelIndex() {
    while (!askLadder_.levels.empty() && !askLadder_.levels.front().active()) {
        askLadder_.levels.pop_front();
        askLadder_.baseTick++;
    }
    return askLadder_.levels.empty() ? SIZE_MAX : 0;
}

// Symmetric trim off the BACK of the bid ladder (highest tick = best bid).
size_t OrderBook::bestBidLevelIndex() {
    while (!bidLadder_.levels.empty() && !bidLadder_.levels.back().active()) {
        bidLadder_.levels.pop_back();
    }
    return bidLadder_.levels.empty() ? SIZE_MAX : bidLadder_.levels.size() - 1;
}

void OrderBook::addOrder(uint64_t id, Side side, double price, uint64_t quantity,
                          uint64_t nowNs, std::vector<Trade>& tradesOut) {
    if (quantity == 0) return;
    int64_t tick = priceToTick(price);

    if (side == Side::BUY) {
        while (quantity > 0) {
            size_t idx = bestAskLevelIndex();
            if (idx == SIZE_MAX) break;
            int64_t askTick = askLadder_.baseTick; // idx is always 0 post-trim
            if (askTick > tick) break; // best ask too expensive, no more crossing

            PriceLevel& level = askLadder_.levels[idx];
            while (quantity > 0 && level.headIdx != Order::NIL) {
                uint32_t restIdx = level.headIdx;
                Order& resting = pool_.get(restIdx);
                uint64_t fillQty = std::min(quantity, resting.quantity);

                Trade t;
                t.buyOrderId = id;
                t.sellOrderId = resting.id;
                t.tick = askTick;
                t.quantity = fillQty;
                t.symbolId = symbolId_;
                t.matchNs = nowNs;
                tradesOut.push_back(t);

                quantity -= fillQty;
                resting.quantity -= fillQty;
                level.totalQty -= fillQty;

                if (resting.quantity == 0) {
                    removeFromLevel(level, restIdx);
                    orderLocation_.erase(resting.id);
                    pool_.release(restIdx);
                }
            }
        }
        if (quantity > 0) {
            size_t idx = bidLadder_.indexFor(tick);
            uint32_t orderIdx = pool_.acquire(id, side, tick, quantity, symbolId_, nowNs);
            appendToLevel(bidLadder_.levels[idx], orderIdx);
            orderLocation_[id] = Location{side, tick, orderIdx};
        }
    } else {
        while (quantity > 0) {
            size_t idx = bestBidLevelIndex();
            if (idx == SIZE_MAX) break;
            int64_t bidTick = bidLadder_.baseTick + static_cast<int64_t>(idx);
            if (bidTick < tick) break;

            PriceLevel& level = bidLadder_.levels[idx];
            while (quantity > 0 && level.headIdx != Order::NIL) {
                uint32_t restIdx = level.headIdx;
                Order& resting = pool_.get(restIdx);
                uint64_t fillQty = std::min(quantity, resting.quantity);

                Trade t;
                t.buyOrderId = resting.id;
                t.sellOrderId = id;
                t.tick = bidTick;
                t.quantity = fillQty;
                t.symbolId = symbolId_;
                t.matchNs = nowNs;
                tradesOut.push_back(t);

                quantity -= fillQty;
                resting.quantity -= fillQty;
                level.totalQty -= fillQty;

                if (resting.quantity == 0) {
                    removeFromLevel(level, restIdx);
                    orderLocation_.erase(resting.id);
                    pool_.release(restIdx);
                }
            }
        }
        if (quantity > 0) {
            size_t idx = askLadder_.indexFor(tick);
            uint32_t orderIdx = pool_.acquire(id, side, tick, quantity, symbolId_, nowNs);
            appendToLevel(askLadder_.levels[idx], orderIdx);
            orderLocation_[id] = Location{side, tick, orderIdx};
        }
    }
}

bool OrderBook::cancelOrder(uint64_t id) {
    auto it = orderLocation_.find(id);
    if (it == orderLocation_.end()) return false;

    const Location loc = it->second;
    Ladder& ladder = (loc.side == Side::BUY) ? bidLadder_ : askLadder_;
    size_t idx = static_cast<size_t>(loc.tick - ladder.baseTick);
    if (idx >= ladder.levels.size()) { orderLocation_.erase(it); return false; }

    PriceLevel& level = ladder.levels[idx];
    removeFromLevel(level, loc.poolIdx);
    pool_.release(loc.poolIdx);
    orderLocation_.erase(it);
    return true;
}

bool OrderBook::bestBidTick(int64_t& tickOut, uint64_t& qtyOut) const {
    for (auto it = bidLadder_.levels.rbegin(); it != bidLadder_.levels.rend(); ++it) {
        if (it->active()) {
            size_t idxFromEnd = static_cast<size_t>(it - bidLadder_.levels.rbegin());
            tickOut = bidLadder_.baseTick + static_cast<int64_t>(bidLadder_.levels.size() - 1 - idxFromEnd);
            qtyOut = it->totalQty;
            return true;
        }
    }
    return false;
}

bool OrderBook::bestAskTick(int64_t& tickOut, uint64_t& qtyOut) const {
    size_t i = 0;
    for (auto it = askLadder_.levels.begin(); it != askLadder_.levels.end(); ++it, ++i) {
        if (it->active()) {
            tickOut = askLadder_.baseTick + static_cast<int64_t>(i);
            qtyOut = it->totalQty;
            return true;
        }
    }
    return false;
}

void OrderBook::printBookSummary(std::ostream& os, size_t depth) const {
    os << std::fixed << std::setprecision(2);
    os << "----- ASKS (lowest first) -----\n";
    size_t shown = 0, i = 0;
    for (auto it = askLadder_.levels.begin(); it != askLadder_.levels.end() && shown < depth; ++it, ++i) {
        if (!it->active()) continue;
        os << "  ASK  " << tickToPrice(askLadder_.baseTick + static_cast<int64_t>(i))
           << "  x" << it->totalQty << "  (" << it->orderCount << " orders)\n";
        shown++;
    }
    os << "----- BIDS (highest first) -----\n";
    shown = 0;
    for (auto it = bidLadder_.levels.rbegin(); it != bidLadder_.levels.rend() && shown < depth; ++it) {
        if (!it->active()) continue;
        size_t idxFromEnd = static_cast<size_t>(it - bidLadder_.levels.rbegin());
        int64_t tick = bidLadder_.baseTick + static_cast<int64_t>(bidLadder_.levels.size() - 1 - idxFromEnd);
        os << "  BID  " << tickToPrice(tick) << "  x" << it->totalQty
           << "  (" << it->orderCount << " orders)\n";
        shown++;
    }
}
