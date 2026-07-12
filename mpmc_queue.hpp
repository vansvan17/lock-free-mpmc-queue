// mpmc_queue.hpp — Lock-free Multi-Producer Multi-Consumer queue.
//
// Algorithm: Michael & Scott (1996), the canonical lock-free linked-list queue.
//   https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf
//
// Why this exists:
//   Mutex-based queues serialize producers and consumers through a kernel
//   futex. Under contention, threads block and incur a context switch
//   (~1–10 µs each). For latency-sensitive workloads, that's catastrophic.
//
//   The Michael-Scott queue uses lock-free linked-list manipulation via
//   compare-and-swap (CAS). Producers and consumers race; the loser of
//   each race retries against the new state. No thread ever blocks.
//
// The two CAS tricks at the heart of M-S:
//   1) The "cooperative tail advance": a producer first CASes its new node
//      into tail->next, THEN tries to CAS the tail pointer itself to point
//      to the new node. If the second CAS races and loses, the producer
//      knows the tail has *already been advanced by someone else*, which
//      is fine. But more importantly, any thread observing a non-null
//      tail->next means a previous producer was preempted between its two
//      CASes — and the observer COOPERATIVELY completes the work by doing
//      the second CAS on their behalf. This is what makes the queue
//      lock-free (no waiting on a stalled producer).
//
//   2) The dummy/sentinel head node: the queue is never truly empty; it
//      always has at least one node (the dummy). This eliminates a class
//      of empty-vs-nonempty races on head/tail.
//
// ABA caveat:
//   The classical M-S queue assumes "tagged pointers" (a counter packed
//   alongside each pointer) to avoid the ABA problem: a thread reads
//   tail = X, sleeps, then later finds tail = X again but it's a DIFFERENT
//   node X that happens to have been re-allocated at the same address.
//
//   We avoid ABA here by deferring reclamation until the queue is destroyed.
//   Each dequeue retires the old dummy node on a separate list, so it is never
//   re-used while concurrent operations may still hold its address. This keeps
//   the learning implementation simple while ensuring a finished queue releases
//   all of its allocations. A long-lived production queue should use hazard
//   pointers or epoch-based reclamation to reclaim retired nodes incrementally.
//
//   (You can also pair this with a typed memory pool that recycles nodes
//   only after a grace period — same idea as RCU.)

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace lockfree {

template <typename T>
class MpmcQueue {
public:
    MpmcQueue() {
        // Allocate the dummy node. Head and tail both point to it initially.
        Node* dummy = new Node();
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~MpmcQueue() {
        // Drain any remaining nodes. Safe in destructor because no other
        // thread can be touching us by definition.
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* next = n->next.load(std::memory_order_relaxed);
            delete n;
            n = next;
        }
        n = retired_.load(std::memory_order_relaxed);
        while (n) {
            Node* next = n->retired_next;
            delete n;
            n = next;
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    // Enqueue. Always succeeds (modulo new throwing on OOM).
    void enqueue(T value) {
        Node* node = new Node(std::move(value));

        for (;;) {
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = tail->next.load(std::memory_order_acquire);

            // Re-check tail. If it changed under us, restart — we read a
            // potentially stale tail pointer.
            if (tail != tail_.load(std::memory_order_acquire)) continue;

            if (next == nullptr) {
                // Tail genuinely points to the last node. Try to link our
                // new node in. compare_exchange_weak can spuriously fail
                // even when expected matches — that's fine here; we just
                // loop and retry.
                Node* expected = nullptr;
                if (tail->next.compare_exchange_weak(
                        expected, node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    // Step 2 of the two-CAS protocol: advance tail.
                    // If this CAS fails, someone else already advanced
                    // tail for us — that's the cooperative-tail-advance
                    // mechanic and it's fine.
                    tail_.compare_exchange_strong(
                        tail, node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
                // Lost the race; retry.
            } else {
                // We observed tail->next != null, meaning another producer
                // was preempted between its two CASes. Help them: advance
                // tail on their behalf, then retry our own insert.
                tail_.compare_exchange_strong(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

    // Dequeue. Returns std::nullopt if empty.
    std::optional<T> dequeue() {
        for (;;) {
            Node* head = head_.load(std::memory_order_acquire);
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire);

            if (head != head_.load(std::memory_order_acquire)) continue;

            if (head == tail) {
                if (next == nullptr) {
                    // Truly empty.
                    return std::nullopt;
                }
                // head == tail but next != null: a producer was preempted
                // mid-enqueue. Help them advance tail, then retry.
                tail_.compare_exchange_strong(
                    tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;
            }

            // We have a real item to return. Snapshot the value BEFORE we
            // CAS head — otherwise another dequeuer could win the CAS and
            // race with us reading the moved-from node.
            T value = next->value;

            if (head_.compare_exchange_weak(
                    head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                retire(head);
                return value;
            }
            // CAS lost; retry.
        }
    }

    // Approximate emptiness check. Useful for spinning consumers as a hint
    // to back off, but not a synchronization primitive.
    bool empty_hint() const {
        Node* head = head_.load(std::memory_order_acquire);
        Node* next = head->next.load(std::memory_order_acquire);
        return next == nullptr;
    }

private:
    struct Node {
        T                  value;
        std::atomic<Node*> next{nullptr};
        Node*              retired_next = nullptr;

        Node() = default;
        explicit Node(T v) : value(std::move(v)) {}
    };

    // Place head and tail on separate cache lines to avoid false sharing.
    // Producers hammer tail_, consumers hammer head_; if they were on the
    // same line, every producer write would invalidate the consumer's
    // cached copy and vice versa.
    alignas(64) std::atomic<Node*> head_;
    alignas(64) std::atomic<Node*> tail_;
    std::atomic<Node*> retired_{nullptr};

    void retire(Node* node) {
        Node* previous = retired_.load(std::memory_order_relaxed);
        do {
            node->retired_next = previous;
        } while (!retired_.compare_exchange_weak(
            previous, node, std::memory_order_release, std::memory_order_relaxed));
    }
};

} // namespace lockfree
