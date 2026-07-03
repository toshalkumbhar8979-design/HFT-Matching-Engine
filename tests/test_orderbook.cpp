// Dependency-free assert-based unit tests for the advanced OrderBook.

#include "OrderBook.h"
#include "MemoryPool.h"
#include <cassert>
#include <iostream>
#include <vector>

static void test_simple_full_match() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 100.0, 10, 1, trades);
    assert(trades.empty());

    book.addOrder(2, Side::BUY, 100.0, 10, 2, trades);
    assert(trades.size() == 1);
    assert(trades[0].quantity == 10);
    assert(OrderBook::tickToPrice(trades[0].tick) == 100.0);
    assert(book.restingOrderCount() == 0);
    std::cout << "test_simple_full_match PASSED\n";
}

static void test_partial_match_leaves_remainder() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 100.0, 5, 1, trades);
    trades.clear();
    book.addOrder(2, Side::BUY, 100.0, 8, 2, trades);
    assert(trades.size() == 1);
    assert(trades[0].quantity == 5);
    assert(book.restingOrderCount() == 1);

    int64_t tick; uint64_t qty;
    assert(book.bestBidTick(tick, qty));
    assert(OrderBook::tickToPrice(tick) == 100.0 && qty == 3);
    std::cout << "test_partial_match_leaves_remainder PASSED\n";
}

static void test_price_priority() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 101.0, 10, 1, trades);
    book.addOrder(2, Side::SELL, 100.0, 10, 2, trades);
    trades.clear();
    book.addOrder(3, Side::BUY, 101.0, 10, 3, trades);
    assert(trades.size() == 1);
    assert(trades[0].sellOrderId == 2); // cheaper ask fills first
    std::cout << "test_price_priority PASSED\n";
}

static void test_time_priority() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 100.0, 5, 1, trades);
    book.addOrder(2, Side::SELL, 100.0, 5, 2, trades);
    trades.clear();
    book.addOrder(3, Side::BUY, 100.0, 5, 3, trades);
    assert(trades.size() == 1);
    assert(trades[0].sellOrderId == 1); // arrived first at same price
    std::cout << "test_time_priority PASSED\n";
}

static void test_cancel() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::BUY, 99.0, 10, 1, trades);
    assert(book.restingOrderCount() == 1);
    assert(book.cancelOrder(1));
    assert(book.restingOrderCount() == 0);
    assert(!book.cancelOrder(1)); // second cancel fails gracefully
    std::cout << "test_cancel PASSED\n";
}

static void test_no_cross_no_trade() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 105.0, 10, 1, trades);
    book.addOrder(2, Side::BUY, 100.0, 10, 2, trades);
    assert(trades.empty());
    assert(book.restingOrderCount() == 2);
    std::cout << "test_no_cross_no_trade PASSED\n";
}

static void test_multi_level_sweep() {
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::SELL, 100.0, 5, 1, trades);
    book.addOrder(2, Side::SELL, 101.0, 5, 2, trades);
    book.addOrder(3, Side::SELL, 102.0, 5, 3, trades);
    trades.clear();
    book.addOrder(4, Side::BUY, 102.0, 15, 4, trades);
    assert(trades.size() == 3);
    assert(OrderBook::tickToPrice(trades[0].tick) == 100.0);
    assert(OrderBook::tickToPrice(trades[1].tick) == 101.0);
    assert(OrderBook::tickToPrice(trades[2].tick) == 102.0);
    assert(book.restingOrderCount() == 0);
    std::cout << "test_multi_level_sweep PASSED\n";
}

static void test_ladder_grows_both_directions() {
    // Exercises the deque ladder growing below AND above its initial base
    // tick, which is the part of this design that replaces std::map.
    MemoryPool<Order> pool(1000);
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    book.addOrder(1, Side::BUY, 100.0, 1, 1, trades);
    book.addOrder(2, Side::BUY, 90.0, 1, 2, trades);   // grows ladder downward
    book.addOrder(3, Side::BUY, 110.0, 1, 3, trades);  // grows ladder upward

    int64_t tick; uint64_t qty;
    assert(book.bestBidTick(tick, qty));
    assert(OrderBook::tickToPrice(tick) == 110.0); // highest bid still found correctly
    assert(book.restingOrderCount() == 3);
    std::cout << "test_ladder_grows_both_directions PASSED\n";
}

static void test_pool_reuse_after_fill() {
    // Confirms pool slots are actually recycled (no leak / no unbounded growth).
    MemoryPool<Order> pool(4); // deliberately tiny
    OrderBook book(pool, 0);
    std::vector<Trade> trades;

    for (int i = 0; i < 100; ++i) {
        trades.clear();
        book.addOrder(2 * i + 1, Side::SELL, 100.0, 1, 2 * i + 1, trades);
        book.addOrder(2 * i + 2, Side::BUY, 100.0, 1, 2 * i + 2, trades);
        assert(trades.size() == 1); // fully matches immediately every time
    }
    assert(book.restingOrderCount() == 0);
    assert(pool.liveCount() == 0); // every slot returned to the pool
    std::cout << "test_pool_reuse_after_fill PASSED\n";
}

int main() {
    test_simple_full_match();
    test_partial_match_leaves_remainder();
    test_price_priority();
    test_time_priority();
    test_cancel();
    test_no_cross_no_trade();
    test_multi_level_sweep();
    test_ladder_grows_both_directions();
    test_pool_reuse_after_fill();
    std::cout << "\nALL TESTS PASSED\n";
    return 0;
}
