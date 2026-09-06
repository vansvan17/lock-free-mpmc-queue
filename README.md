# Michael–Scott MPMC queue

A C++17 implementation of the [Michael–Scott concurrent queue](https://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf).
Producers link a node and then advance `tail`; a thread that observes a lagging
tail helps complete that update. Producers and consumers operate on separate,
cache-line-aligned atomics.

## Memory reclamation

Dequeued dummy nodes are placed on a lock-free retired list and reclaimed by
the destructor. This avoids use-after-free and ABA caused by immediate address
reuse, but memory consumption grows with the number of dequeues while the
queue is alive. A long-lived implementation would need hazard pointers or
epoch-based reclamation.

## Build and test

```sh
make test
make tsan
```

The stress test runs four producers and four consumers over 400,000 unique
integers, then verifies the item count and sum. The current implementation
allocates one node with `new` per enqueue; its benchmark therefore measures
both queue synchronization and allocation cost.
