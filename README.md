# Lock-Free MPMC Queue (Michael-Scott)

A lock-free, concurrent Multi-Producer Multi-Consumer queue in C++ based on
the [Michael & Scott 1996 algorithm](https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf).
Producers and consumers race via atomic CAS loops; no thread ever blocks
on a mutex or context-switches into the kernel.

> **Status:** functional correctness verified under stress (4 producers ×
> 4 consumers × 100,000 items each, 400 K total — every item dequeued,
> sum matches exactly). ThreadSanitizer clean.

## What this demonstrates

- **Lock-free progress:** no mutex, no condition variable, no kernel
  futex. Every operation completes in a bounded number of steps regardless
  of what other threads are doing (modulo the CAS retry on contention).
- **The Michael-Scott two-CAS protocol:** producers first CAS their new
  node into `tail->next`, then CAS the `tail` pointer itself. This makes
  the queue tolerate producer preemption between the two steps.
- **Cooperative tail-advancement:** if any thread observes a non-null
  `tail->next`, it knows a previous producer was preempted mid-enqueue.
  Rather than wait, it *helps* by advancing the tail on the preempted
  producer's behalf, then retries its own work. This is what makes the
  queue lock-free instead of merely obstruction-free.
- **Dummy sentinel node:** the queue always contains at least one node
  (the dummy at `head`). This eliminates a class of "is it empty?" races
  on the head/tail pointers.
- **Cache-line padding:** `head` and `tail` are `alignas(64)` to keep
  them on separate cache lines. Otherwise every producer write would
  invalidate the consumer's cached `head` and vice versa — classic false
  sharing.

## The ABA problem and how this code handles it

The naive Michael-Scott algorithm has an ABA hazard: a thread reads
`tail = X`, sleeps, and later sees `tail = X` again — but it's a *different
node* that happens to live at the same address because the original `X`
was freed and reused.

Production lock-free queues use one of:
- **Tagged pointers** — pack a generation counter into the high bits of
  each pointer so the same address with a new tag is recognized as a
  different value.
- **Hazard pointers** — each thread publishes which nodes it currently
  references, so the reclaimer waits until no hazard pointer references
  a node before freeing it.
- **Epoch-based reclamation** — defer frees until all threads have passed
  through a quiescent period.

**This implementation takes the simplest path: it never frees dequeued
nodes during operation** (only at destruction). This is honest about what
the queue is — a learning project that demonstrates the algorithm
correctly — and avoids the substantial complexity of safe memory
reclamation. A production version would pair this with a hazard-pointer
scheme or an epoch-based pool.

## Build & test

```bash
g++ -O2 -std=c++17 -pthread -Wall -Wextra test_mpmc.cpp -o test_mpmc
./test_mpmc
```

ThreadSanitizer run:

```bash
g++ -O1 -g -std=c++17 -pthread -fsanitize=thread test_mpmc.cpp -o test_mpmc_tsan
./test_mpmc_tsan
```

## Test results

```
[ RUN ] single_threaded                OK
[ RUN ] interleaved_enqueue_dequeue    OK  (FIFO ordering preserved)
[ RUN ] mpmc_stress                    OK  (4 prod × 4 cons × 100 K items)
        dequeued: 400000 (expected 400000)
        sum:      79999800000 (expected 79999800000)
```

The stress test is the meaningful one: producers enqueue disjoint integer
ranges; consumers accumulate everything they dequeue. After all threads
join, the dequeued count and the sum must both match exactly — any lost,
duplicated, or torn item would show up. The queue passes consistently
across many runs.

ThreadSanitizer: clean. No data races, no missed synchronization.

## Benchmark

Single-producer / single-consumer, 1 M items, x86-64:

| Run | ns / item |
|---|---|
| `-O2`, no sanitizer | **105.5** |
| `-O1`, TSan         | ~9600 (TSan overhead, not representative) |

105 ns/item is fair for a Michael-Scott queue with per-item `new` and
move construction. A production lock-free queue specialized for SPSC
(single-producer single-consumer) — e.g., a Lamport ring buffer — would
clock in single-digit ns/item by eliminating the linked-list allocation
and the CAS entirely (a single producer just bumps a write index;
a single consumer just bumps a read index).

## Known limitations

- **No safe memory reclamation.** Dequeued nodes leak until destruction.
  Discussed above.
- **`new` per enqueue.** A typed node pool would amortize this to near-zero
  on the hot path. Pairs naturally with the companion arena allocator.
- **MPMC overhead even when uncontended.** If you know your access pattern
  is SPSC, a Lamport ring buffer is dramatically faster (~5 ns vs ~100 ns).
- **No backpressure.** The queue is unbounded; producers always succeed.
  A bounded variant would CAS against a size counter.
