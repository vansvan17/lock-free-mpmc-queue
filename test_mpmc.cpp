// test_mpmc.cpp — Functional and stress tests for the MPMC queue.
//
// Compile:
//   g++ -O2 -std=c++17 -pthread -Wall -Wextra test_mpmc.cpp -o test_mpmc
// Run:
//   ./test_mpmc

#include "mpmc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using lockfree::MpmcQueue;

static int failures = 0;

#define EXPECT(cond) do {                                                 \
    if (!(cond)) {                                                        \
        std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static void test_single_threaded() {
    std::fprintf(stderr, "[ RUN ] single_threaded\n");
    MpmcQueue<int> q;
    EXPECT(!q.dequeue().has_value());

    for (int i = 0; i < 100; ++i) q.enqueue(i);
    for (int i = 0; i < 100; ++i) {
        auto v = q.dequeue();
        EXPECT(v.has_value());
        EXPECT(*v == i);   // FIFO
    }
    EXPECT(!q.dequeue().has_value());
}

static void test_interleaved() {
    std::fprintf(stderr, "[ RUN ] interleaved_enqueue_dequeue\n");
    MpmcQueue<int> q;
    q.enqueue(1); q.enqueue(2);
    EXPECT(*q.dequeue() == 1);
    q.enqueue(3);
    EXPECT(*q.dequeue() == 2);
    EXPECT(*q.dequeue() == 3);
    EXPECT(!q.dequeue().has_value());
}

// The critical test. Multiple producers and consumers race; we verify that
// the SUM of dequeued values matches the SUM of enqueued values, and that
// no value is missing or duplicated.
static void test_mpmc_stress() {
    std::fprintf(stderr, "[ RUN ] mpmc_stress (4 prod x 4 cons x 100k items)\n");

    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr int kItemsPerProducer = 100'000;
    constexpr int kTotal = kProducers * kItemsPerProducer;

    MpmcQueue<int> q;
    std::atomic<long long> sum{0};
    std::atomic<int> dequeued{0};
    std::atomic<bool> producers_done{false};

    std::vector<std::thread> threads;

    // Producers enqueue values [p*N, (p+1)*N). Across all producers, the
    // multiset of values is exactly [0, kTotal).
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&, p]() {
            int start = p * kItemsPerProducer;
            for (int i = 0; i < kItemsPerProducer; ++i) {
                q.enqueue(start + i);
            }
        });
    }

    // Consumers dequeue and accumulate.
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back([&]() {
            for (;;) {
                auto v = q.dequeue();
                if (v) {
                    sum.fetch_add(*v, std::memory_order_relaxed);
                    dequeued.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load(std::memory_order_acquire) &&
                           q.empty_hint()) {
                    // Defensive: re-check once more after the producers_done
                    // signal in case we raced with the last enqueue.
                    auto v2 = q.dequeue();
                    if (v2) {
                        sum.fetch_add(*v2, std::memory_order_relaxed);
                        dequeued.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        return;
                    }
                }
                // Otherwise: spin. (In production: yield or back off.)
            }
        });
    }

    // Wait for producers, then signal consumers.
    for (int i = 0; i < kProducers; ++i) threads[i].join();
    producers_done.store(true, std::memory_order_release);

    for (int i = kProducers; i < kProducers + kConsumers; ++i) threads[i].join();

    // Sum of [0, kTotal) is kTotal*(kTotal-1)/2.
    long long expected = static_cast<long long>(kTotal) * (kTotal - 1) / 2;
    long long actual   = sum.load();
    int got = dequeued.load();

    std::fprintf(stderr, "  dequeued: %d (expected %d)\n", got, kTotal);
    std::fprintf(stderr, "  sum: %lld (expected %lld)\n", actual, expected);

    EXPECT(got == kTotal);
    EXPECT(actual == expected);
}

static void benchmark() {
    std::fprintf(stderr, "\n[ BENCH ] single-producer/single-consumer throughput, 1M items\n");
    constexpr int N = 1'000'000;
    MpmcQueue<int> q;
    using clk = std::chrono::steady_clock;

    auto t0 = clk::now();
    std::thread prod([&]() {
        for (int i = 0; i < N; ++i) q.enqueue(i);
    });
    std::thread cons([&]() {
        int got = 0;
        while (got < N) {
            if (q.dequeue()) ++got;
        }
    });
    prod.join();
    cons.join();
    auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  clk::now() - t0).count();
    std::fprintf(stderr, "  %lld ns total, %.1f ns/item\n",
                 static_cast<long long>(dt), double(dt) / N);
}

int main() {
    test_single_threaded();
    test_interleaved();
    test_mpmc_stress();
    std::fprintf(stderr, "\n%s\n",
                 failures == 0 ? "all tests passed" : "TESTS FAILED");
    benchmark();
    return failures == 0 ? 0 : 1;
}
