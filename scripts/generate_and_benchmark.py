#!/usr/bin/env python3
"""
generate_and_benchmark.py

Streams a large volume of mock multi-symbol stock orders into the C++
producer/consumer matching engine and reports throughput AND latency
percentiles (not just an average -- see README for why the tail matters).

Simulates realistic order flow across several symbols simultaneously:
  - Each symbol has its own randomly-walking mid-price.
  - Orders are placed with a random offset around the mid-price so a
    meaningful fraction cross and generate trades.
  - A small fraction of orders are cancellations of earlier resting orders.
  - Orders across symbols are interleaved (round-robin-ish with jitter) to
    simulate a realistic multiplexed feed rather than one symbol at a time.

Usage:
    python3 generate_and_benchmark.py --binary ../build/orderbook_matcher --orders 500000
    python3 generate_and_benchmark.py --sweep
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile
import time

DEFAULT_SYMBOLS = ["AAPL", "MSFT", "GOOG", "AMZN", "NVDA"]


def generate_orders(path: str, num_orders: int, symbols=DEFAULT_SYMBOLS,
                     seed: int = 42, cancel_ratio: float = 0.05) -> None:
    rng = random.Random(seed)
    mid_prices = {s: rng.uniform(50, 500) for s in symbols}
    open_orders = {s: [] for s in symbols}

    with open(path, "w") as f:
        for i in range(1, num_orders + 1):
            symbol = rng.choice(symbols)
            oid = i

            if open_orders[symbol] and rng.random() < cancel_ratio:
                cancel_id = open_orders[symbol].pop(rng.randrange(len(open_orders[symbol])))
                f.write(f"CANCEL,{symbol},{cancel_id}\n")
                continue

            mid_prices[symbol] += rng.uniform(-0.02, 0.02)
            mid_prices[symbol] = max(1.0, mid_prices[symbol])

            side = "B" if rng.random() < 0.5 else "S"
            price = round(mid_prices[symbol] + rng.uniform(-0.5, 0.5), 2)
            qty = rng.randint(1, 500)

            f.write(f"NEW,{symbol},{oid},{side},{price},{qty}\n")
            open_orders[symbol].append(oid)
            if len(open_orders[symbol]) > 3000:
                open_orders[symbol].pop(0)


def run_benchmark(binary: str, input_path: str) -> dict:
    if not os.path.isfile(binary):
        print(f"ERROR: engine binary not found at '{binary}'. Build it first (see README).", file=sys.stderr)
        sys.exit(1)

    wall_start = time.perf_counter()
    with open(input_path, "r") as infile:
        result = subprocess.run(
            [binary, "--quiet"],
            stdin=infile,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    wall_elapsed = time.perf_counter() - wall_start

    if result.returncode != 0:
        print("Engine exited with an error:", result.stderr, file=sys.stderr)
        sys.exit(1)

    metrics = {"wall_clock_sec": wall_elapsed}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            try:
                metrics[key] = float(value)
            except ValueError:
                metrics[key] = value
    return metrics


def fmt_ns(ns: float) -> str:
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.3f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.2f} us"
    return f"{ns:.0f} ns"


def print_report(num_orders: int, metrics: dict) -> None:
    print(f"\n=== Benchmark: {num_orders:,} orders across {int(metrics.get('SYMBOLS', 0))} symbols ===")
    print(f"  Orders processed        : {int(metrics.get('ORDERS_PROCESSED', 0)):,}")
    print(f"  Trades generated        : {int(metrics.get('TRADES_GENERATED', 0)):,}")
    print(f"  Engine elapsed time     : {metrics.get('ELAPSED_MS', 0):.3f} ms")
    print(f"  Throughput              : {metrics.get('THROUGHPUT_ORDERS_PER_SEC', 0):,.0f} orders/sec")
    print(f"  --- Queue latency (time waiting in SPSC buffer) ---")
    print(f"      p50  : {fmt_ns(metrics.get('QUEUE_LATENCY_P50_NS', 0))}")
    print(f"      p90  : {fmt_ns(metrics.get('QUEUE_LATENCY_P90_NS', 0))}")
    print(f"      p99  : {fmt_ns(metrics.get('QUEUE_LATENCY_P99_NS', 0))}")
    print(f"      p99.9: {fmt_ns(metrics.get('QUEUE_LATENCY_P999_NS', 0))}")
    print(f"      max  : {fmt_ns(metrics.get('QUEUE_LATENCY_MAX_NS', 0))}")
    print(f"  --- Match latency (time to actually apply the order) ---")
    print(f"      p50  : {fmt_ns(metrics.get('MATCH_LATENCY_P50_NS', 0))}")
    print(f"      p90  : {fmt_ns(metrics.get('MATCH_LATENCY_P90_NS', 0))}")
    print(f"      p99  : {fmt_ns(metrics.get('MATCH_LATENCY_P99_NS', 0))}")
    print(f"      p99.9: {fmt_ns(metrics.get('MATCH_LATENCY_P999_NS', 0))}")
    print(f"      max  : {fmt_ns(metrics.get('MATCH_LATENCY_MAX_NS', 0))}")


def main():
    parser = argparse.ArgumentParser(description="Benchmark the advanced multi-threaded matching engine.")
    parser.add_argument("--binary", default="../build/orderbook_matcher")
    parser.add_argument("--orders", type=int, default=500_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--keep-file", action="store_true")
    parser.add_argument("--sweep", action="store_true",
                         help="Run 10k / 100k / 500k / 1,000,000 / 2,000,000 orders")
    args = parser.parse_args()

    sizes = [10_000, 100_000, 500_000, 1_000_000, 2_000_000] if args.sweep else [args.orders]

    for n in sizes:
        fd, tmp_path = tempfile.mkstemp(suffix=".csv", prefix="orders_")
        os.close(fd)
        try:
            gen_start = time.perf_counter()
            generate_orders(tmp_path, n, seed=args.seed)
            gen_elapsed = time.perf_counter() - gen_start
            print(f"Generated {n:,} orders in {gen_elapsed:.2f}s -> {tmp_path}")

            metrics = run_benchmark(args.binary, tmp_path)
            print_report(n, metrics)
        finally:
            if args.keep_file:
                print(f"  (kept order file at {tmp_path})")
            else:
                os.remove(tmp_path)


if __name__ == "__main__":
    main()
