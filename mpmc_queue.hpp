// Michael–Scott MPMC queue. Retired nodes are not reused until destruction,
// which avoids ABA at the cost of unbounded memory use for a long-lived queue.

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
