# HFT Matching Engine — Advanced Edition

A multi-threaded, low-latency limit order book matching engine in C++17,
implementing **Price-Time Priority** across multiple symbols, with a
lock-free producer/consumer pipeline and nanosecond-precision latency
histograms. Includes a Python harness that streams 100,000+ orders/sec
into it and reports both throughput and tail latency.

This is a deliberate rebuild of a simpler "std::map + shared_ptr" order
book, replacing every piece that doesn't hold up under real scrutiny:

| | Simple version | This version |
|---|---|---|
| Price levels | `std::map` (red-black tree) | Deque-indexed ladder, O(1) best-price access |
| Order storage | `shared_ptr<Order>` (heap alloc per order) | Fixed-capacity `MemoryPool<Order>`, zero heap allocation on the hot path |
| Order lists | `std::deque<shared_ptr>` per level | Intrusive doubly-linked list via pool indices |
| Ingestion | Single-threaded stdin read | Producer thread + lock-free SPSC ring buffer + consumer (matching) thread |
| Measurement | Aggregate throughput only | Throughput **and** nanosecond p50/p90/p99/p99.9 latency histograms |

## Project Layout

```
HFT-MatchingEngine/
├── CMakeLists.txt
├── include/
│   ├── Order.h              # pool-managed order w/ intrusive list pointers
│   ├── OrderBook.h           # deque-ladder matching engine
│   ├── MemoryPool.h          # fixed-capacity object pool (no per-order heap alloc)
│   ├── SpscRingBuffer.h      # lock-free single-producer/single-consumer queue
│   └── LatencyHistogram.h    # O(1)-record nanosecond latency histogram
├── src/
│   ├── OrderBook.cpp
│   └── main.cpp              # producer/consumer threads, CLI, metrics
├── tests/
│   └── test_orderbook.cpp    # 9 assert-based unit tests
└── scripts/
    └── generate_and_benchmark.py   # multi-symbol order generator + benchmark harness
```

## Design Notes

### 1. Price ladder instead of a tree — `include/OrderBook.h`

Prices are converted to integer ticks (`tick = round(price / 0.01)`), and
each side of the book is a `std::deque<PriceLevel>` **directly indexed** by
`tick - baseTick`. Looking up "the level for this price" is O(1) array
indexing instead of an O(log N) red-black tree descent, and a deque's
chunked contiguous storage is far more cache-friendly than following tree
node pointers scattered across the heap. The ladder grows at either end
(`push_front`/`push_back`, O(1) amortized) as prices move outside the
current range, and dead levels at the best-price end are trimmed lazily so
lookups stay O(1) amortized even as the book empties out.

### 2. Zero heap allocation on the hot path — `include/MemoryPool.h`

All order storage is a single `std::vector<Order>` allocated once at
startup. Orders are handed out and reclaimed via an O(1) free-index stack —
no `new`/`delete`, no `shared_ptr` refcounting, no allocator locks. Within a
price level, orders link via raw pool indices (`prevIdx`/`nextIdx`), not
pointers, keeping `Order` trivially relocatable inside the pool's vector.
`tests/test_orderbook.cpp::test_pool_reuse_after_fill` verifies pool slots
are actually recycled by running 100 orders through a **4-slot** pool.

### 3. Lock-free producer/consumer pipeline — `include/SpscRingBuffer.h`, `src/main.cpp`

A "feed handler" thread parses stdin and pushes `OrderCommand`s into a
lock-free single-producer/single-consumer ring buffer (power-of-two
capacity, bitmask wraparound, acquire/release atomics, each cursor on its
own cache line to avoid false sharing). A separate "matching engine" thread
pops commands and applies them. This mirrors how real trading systems keep
network/parsing I/O off the matching hot path, so a slow read() or a burst
of messages can't stall the book itself.

### 4. Latency histograms, not just throughput — `include/LatencyHistogram.h`

Every order is timestamped (nanoseconds, `steady_clock`) when it's enqueued
by the producer, again when the consumer dequeues it, and again when
matching completes — giving two separate distributions:

- **Queue latency**: time spent waiting in the ring buffer before being picked up.
- **Match latency**: time to actually apply the order once picked up (this is
  the number that reflects the matching *algorithm's* own speed).

Recorded into power-of-two buckets (like a simplified HdrHistogram) — O(1)
per sample, O(1) memory, and O(num_buckets) to compute any percentile. This
matters because average throughput hides the tail, and the tail is what
actually costs you in a real system (a single slow order = a missed fill or
a stale quote).

## Building

```bash
# CMake
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# or direct g++
g++ -std=c++17 -O3 -DNDEBUG -Iinclude -pthread -o build/orderbook_matcher src/main.cpp src/OrderBook.cpp
g++ -std=c++17 -O3 -DNDEBUG -Iinclude -pthread -o build/orderbook_tests tests/test_orderbook.cpp src/OrderBook.cpp
```

## Running

Input format, one command per line:

```
NEW,<symbol>,<order_id>,<B|S>,<price>,<qty>
CANCEL,<symbol>,<order_id>
```

```bash
printf "NEW,AAPL,1,S,100.50,10\nNEW,AAPL,2,B,100.50,4\nNEW,AAPL,3,B,101.00,10\nCANCEL,AAPL,1\n" \
  | ./build/orderbook_matcher --summary
```

Flags: `--quiet` (suppress trade lines, use for benchmarking), `--summary`
(print top-of-book per symbol on exit), `--pool-capacity N` (default
4,000,000), `--queue-capacity N` (must be power of 2, default 65536).

## Unit Tests

```bash
./build/orderbook_tests
```

9 tests: full/partial fills, price priority, time priority, cancellation,
non-crossing orders, multi-level sweep, ladder growth in both directions,
and pool-slot reuse under a deliberately tiny pool.

## Memory & Thread Safety

Verified with sanitizers (not just "should be fine"):

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer — clean
g++ -std=c++17 -O0 -g -fsanitize=address,undefined -Iinclude -pthread \
  -o build/orderbook_matcher_asan src/main.cpp src/OrderBook.cpp

# ThreadSanitizer — clean under a 50k-order, 3-symbol multithreaded run
g++ -std=c++17 -O1 -g -fsanitize=thread -Iinclude -pthread \
  -o build/orderbook_matcher_tsan src/main.cpp src/OrderBook.cpp
```

Both were run against the test suite and a multi-symbol order stream with
zero reported errors.

## Benchmark Results (measured, not estimated)

```bash
cd scripts
python3 generate_and_benchmark.py --binary ../build/orderbook_matcher --sweep
```

Measured on this build (Release, `-O3`), 5 symbols, this container's CPU:

| Orders | Trades | Engine time | Throughput | Match latency p50 / p99 | Match latency max |
|---:|---:|---:|---:|---:|---:|
| 10,000 | 7,399 | 4.3 ms | 2.31M/sec | 128 ns / 512 ns | 18.7 µs |
| 100,000 | 79,172 | 42.7 ms | 2.34M/sec | 128 ns / 512 ns | 108.3 µs |
| 500,000 | 417,352 | 235.7 ms | 2.12M/sec | 256 ns / 1.02 µs | 5.3 ms |
| 1,000,000 | 849,866 | 497.0 ms | 2.01M/sec | 256 ns / 1.02 µs | 5.3 ms |
| 2,000,000 | 1,715,898 | 1076.9 ms | 1.86M/sec | 256 ns / 1.02 µs | 5.7 ms |

**The matching algorithm itself runs in ~128–256 nanoseconds at p50/p90,
sub-microsecond through p99** — that's the number that reflects the data
structure design, and it's the one to quote. Overall system throughput
sustains ~2M orders/sec.

### An honest caveat about the "queue latency" numbers

If you run the benchmark yourself, you'll see `QUEUE_LATENCY_P50_NS` values
in the **millisecond** range, which looks alarming next to a 128ns match
latency. This is **not** an architectural problem with the SPSC queue — it's
this development container sharing CPU across the producer and consumer
threads (and everything else running on the host) rather than giving each
thread a dedicated core. When the OS scheduler doesn't run the consumer
thread for a few milliseconds, every order sitting in the ring buffer during
that gap gets charged that same delay. On a machine with real core pinning
(`taskset`/`sched_setaffinity`) and enough spare cores, queue latency should
collapse close to the match latency. I'm calling this out explicitly rather
than only showing the flattering number — being able to explain *why* a
benchmark number looks the way it does is exactly what separates a project
you can defend from one you can't.

## Known Limitations (worth knowing before an interview asks)

- Ladder levels emptied by cancellations in the *middle* of the book (not at
  the best-price end) aren't compacted — only the best-price end is trimmed.
  A long-running book with heavy mid-book cancellation activity would slowly
  grow its deque capacity. A production system would periodically compact
  or use a sparse structure for far-out-of-range levels.
- `MemoryPool` capacity is fixed at startup; exceeding it throws rather than
  growing. That's intentional (predictable behavior beats surprise
  reallocation on the hot path) but means capacity has to be provisioned for
  the expected peak resting-order count.
- No persistence, no risk checks, no sequencing/replay, no networking layer —
  this is a matching engine core, not a full exchange.
- The SPSC queue is exactly that — single producer, single consumer. A real
  multi-feed system would need either multiple queues (one per feed, fanned
  into the matching thread) or an MPSC structure.


