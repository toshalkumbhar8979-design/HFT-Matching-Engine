// High-Frequency Market Data Order Book Matcher -- Advanced Edition
//
// Architecture: a "feed handler" (producer) thread parses incoming order
// text from stdin and pushes commands into a lock-free SPSC ring buffer.
// A separate "matching engine" (consumer) thread pops commands and applies
// them to per-symbol OrderBooks. This mirrors how real trading systems
// separate network/parsing I/O from the matching hot path so that a slow
// read() syscall or a burst of messages never stalls the book itself.
//
// Every order is timestamped (nanoseconds, steady_clock) the moment it's
// enqueued by the producer, and again the moment it's dequeued and again
// once matching completes by the consumer -- giving two latency
// distributions:
//   QUEUE LATENCY  = time sitting in the SPSC buffer before being picked up
//   MATCH LATENCY  = time to actually run the matching logic once picked up
//
// Input format (comma-separated, one per line):
//   NEW,<symbol>,<order_id>,<B|S>,<price>,<qty>
//   CANCEL,<symbol>,<order_id>
//
// Flags:
//   --quiet             suppress per-trade output lines
//   --summary           print top-of-book depth summary per symbol at exit
//   --pool-capacity N   MemoryPool<Order> size (default 4,000,000)
//   --queue-capacity N  SPSC ring buffer size, must be power of 2 (default 65536)

#include "OrderBook.h"
#include "MemoryPool.h"
#include "SpscRingBuffer.h"
#include "LatencyHistogram.h"

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

enum class CmdType : uint8_t { NEW, CANCEL };

struct OrderCommand {
    CmdType type = CmdType::NEW;
    uint32_t symbolId = 0;
    uint64_t orderId = 0;
    Side side = Side::BUY;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t enqueueNs = 0;
};

static inline uint64_t nowNs(const std::chrono::steady_clock::time_point& start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

int main(int argc, char** argv) {
    bool quiet = false;
    bool summary = false;
    size_t poolCapacity = 4'000'000;
    size_t queueCapacity = 1u << 16; // must be power of 2

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
        else if (std::strcmp(argv[i], "--summary") == 0) summary = true;
        else if (std::strcmp(argv[i], "--pool-capacity") == 0 && i + 1 < argc) poolCapacity = std::stoull(argv[++i]);
        else if (std::strcmp(argv[i], "--queue-capacity") == 0 && i + 1 < argc) queueCapacity = std::stoull(argv[++i]);
    }
    if ((queueCapacity & (queueCapacity - 1)) != 0) {
        std::cerr << "ERROR: --queue-capacity must be a power of two\n";
        return 1;
    }

    MemoryPool<Order> pool(poolCapacity);
    SpscRingBuffer<OrderCommand> queue(queueCapacity);

    std::vector<std::string> symbolNames;
    std::vector<std::unique_ptr<OrderBook>> books;

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> commandsProduced{0};

    auto startTime = std::chrono::steady_clock::now();

    // ---- Producer thread: parse stdin, push into the lock-free queue ----
    std::thread producer([&]() {
        std::ios::sync_with_stdio(false);
        std::unordered_map<std::string, uint32_t> symbolIds;
        std::string line;
        line.reserve(128);
        std::vector<std::string> tok;
        tok.reserve(6);

        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            tok.clear();
            size_t pos = 0;
            while (pos <= line.size()) {
                size_t comma = line.find(',', pos);
                if (comma == std::string::npos) { tok.push_back(line.substr(pos)); break; }
                tok.push_back(line.substr(pos, comma - pos));
                pos = comma + 1;
            }
            if (tok.empty()) continue;

            OrderCommand cmd;
            if (tok[0] == "NEW" && tok.size() == 6) {
                const std::string& sym = tok[1];
                auto it = symbolIds.find(sym);
                uint32_t symId;
                if (it == symbolIds.end()) {
                    symId = static_cast<uint32_t>(symbolNames.size());
                    symbolIds.emplace(sym, symId);
                    symbolNames.push_back(sym); // producer is sole writer, safe pre-EOF handoff
                } else {
                    symId = it->second;
                }
                cmd.type = CmdType::NEW;
                cmd.symbolId = symId;
                cmd.orderId = std::stoull(tok[2]);
                cmd.side = (tok[3][0] == 'B' || tok[3][0] == 'b') ? Side::BUY : Side::SELL;
                cmd.price = std::stod(tok[4]);
                cmd.quantity = std::stoull(tok[5]);
            } else if (tok[0] == "CANCEL" && tok.size() == 3) {
                const std::string& sym = tok[1];
                auto it = symbolIds.find(sym);
                if (it == symbolIds.end()) continue; // unknown symbol, nothing to cancel
                cmd.type = CmdType::CANCEL;
                cmd.symbolId = it->second;
                cmd.orderId = std::stoull(tok[2]);
            } else {
                continue;
            }

            cmd.enqueueNs = nowNs(startTime);
            while (!queue.push(cmd)) {
                std::this_thread::yield(); // backpressure: ring buffer full, spin briefly
            }
            commandsProduced.fetch_add(1, std::memory_order_relaxed);
        }
        producerDone.store(true, std::memory_order_release);
    });

    // ---- Consumer thread (matching engine): pop & apply commands ----
    uint64_t ordersProcessed = 0, tradesGenerated = 0;
    LatencyHistogram queueLatency, matchLatency;
    std::vector<Trade> tradesBuf;
    tradesBuf.reserve(8);

    std::thread consumer([&]() {
        OrderCommand cmd;
        while (true) {
            if (queue.pop(cmd)) {
                uint64_t dequeueTs = nowNs(startTime);
                queueLatency.record(dequeueTs - cmd.enqueueNs);

                if (cmd.symbolId >= books.size()) {
                    books.resize(cmd.symbolId + 1);
                }
                if (!books[cmd.symbolId]) {
                    books[cmd.symbolId] = std::make_unique<OrderBook>(pool, cmd.symbolId);
                }

                if (cmd.type == CmdType::NEW) {
                    tradesBuf.clear();
                    books[cmd.symbolId]->addOrder(cmd.orderId, cmd.side, cmd.price, cmd.quantity,
                                                   dequeueTs, tradesBuf);
                    tradesGenerated += tradesBuf.size();
                    if (!quiet) {
                        for (auto& t : tradesBuf) {
                            std::cout << "TRADE," << symbolNames[t.symbolId] << ','
                                      << t.buyOrderId << ',' << t.sellOrderId << ','
                                      << OrderBook::tickToPrice(t.tick) << ',' << t.quantity
                                      << ',' << t.matchNs << '\n';
                        }
                    }
                } else {
                    books[cmd.symbolId]->cancelOrder(cmd.orderId);
                }
                ordersProcessed++;
                matchLatency.record(nowNs(startTime) - dequeueTs);
            } else if (producerDone.load(std::memory_order_acquire) && queue.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    auto endTime = std::chrono::steady_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    double throughputPerMs = elapsedMs > 0 ? ordersProcessed / elapsedMs : 0.0;

    if (summary) {
        for (size_t i = 0; i < books.size(); ++i) {
            if (!books[i]) continue;
            std::cerr << "=== Symbol: " << symbolNames[i] << " ===\n";
            books[i]->printBookSummary(std::cerr);
        }
    }

    std::cout << "ORDERS_PROCESSED=" << ordersProcessed << '\n';
    std::cout << "TRADES_GENERATED=" << tradesGenerated << '\n';
    std::cout << "SYMBOLS=" << symbolNames.size() << '\n';
    std::cout << "ELAPSED_MS=" << elapsedMs << '\n';
    std::cout << "THROUGHPUT_ORDERS_PER_MS=" << throughputPerMs << '\n';
    std::cout << "THROUGHPUT_ORDERS_PER_SEC=" << throughputPerMs * 1000.0 << '\n';
    std::cout << "QUEUE_LATENCY_P50_NS=" << queueLatency.percentile(0.50) << '\n';
    std::cout << "QUEUE_LATENCY_P90_NS=" << queueLatency.percentile(0.90) << '\n';
    std::cout << "QUEUE_LATENCY_P99_NS=" << queueLatency.percentile(0.99) << '\n';
    std::cout << "QUEUE_LATENCY_P999_NS=" << queueLatency.percentile(0.999) << '\n';
    std::cout << "QUEUE_LATENCY_MAX_NS=" << queueLatency.maxNs() << '\n';
    std::cout << "MATCH_LATENCY_P50_NS=" << matchLatency.percentile(0.50) << '\n';
    std::cout << "MATCH_LATENCY_P90_NS=" << matchLatency.percentile(0.90) << '\n';
    std::cout << "MATCH_LATENCY_P99_NS=" << matchLatency.percentile(0.99) << '\n';
    std::cout << "MATCH_LATENCY_P999_NS=" << matchLatency.percentile(0.999) << '\n';
    std::cout << "MATCH_LATENCY_MAX_NS=" << matchLatency.maxNs() << '\n';
    std::cout << "POOL_LIVE_ORDERS=" << pool.liveCount() << '\n';

    return 0;
}
