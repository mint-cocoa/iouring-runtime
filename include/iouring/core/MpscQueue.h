#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

namespace iouring::core {

// Lock-free Multi-Producer Single-Consumer queue.
//
// Based on Dmitry Vyukov's intrusive MPSC queue algorithm.
// https://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
//
// - Push(): any number of threads (lock-free, XCHG on produce_end)
// - Drain(): exactly ONE consumer thread
//
// stub_ is stack-allocated and recycled back into the queue when the consumer
// reaches the last node, avoiding the need for heap-allocated sentinels.

template<typename T>
class MpscQueue {
    struct Node {
        T value;
        std::atomic<Node*> next{nullptr};

        Node() = default;
        explicit Node(T val) : value(std::move(val)) {}
    };

public:
    MpscQueue()
        : stub_()
        , consume_end_(&stub_)
        , produce_end_(&stub_) {}

    ~MpscQueue() {
        T item;
        while (TryPop(item)) {}
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // ── Producer side (multiple threads) ──────────────────────

    void Push(T item) {
        PushNode(new Node(std::move(item)));
    }

    // ── Consumer side (single thread only) ────────────────────

    // Drain all available items, calling handler(T&&) for each.
    template<typename Fn>
    std::size_t Drain(Fn&& handler) {
        std::size_t count = 0;
        T item;
        while (TryPop(item)) {
            handler(std::move(item));
            ++count;
        }
        return count;
    }

private:
    bool TryPop(T& out) {
        Node* node = consume_end_.load(std::memory_order_relaxed);
        Node* next = node->next.load(std::memory_order_acquire);

        // Skip stub node on first consume
        if (node == &stub_) {
            if (!next)
                return false;
            consume_end_.store(next, std::memory_order_relaxed);
            node = next;
            next = node->next.load(std::memory_order_acquire);
        }

        // Fast path: next node exists
        if (next) {
            consume_end_.store(next, std::memory_order_relaxed);
            out = std::move(node->value);
            delete node;
            return true;
        }

        // Last node: check if it's truly the tail
        if (node != produce_end_.load(std::memory_order_acquire))
            return false; // producer is mid-push (blocking window)

        // Recycle stub back into the queue
        stub_.next.store(nullptr, std::memory_order_relaxed);
        PushNode(&stub_);

        next = node->next.load(std::memory_order_acquire);
        if (next) {
            consume_end_.store(next, std::memory_order_relaxed);
            out = std::move(node->value);
            delete node;
            return true;
        }

        // Producer still linking (blocking window)
        return false;
    }

    void PushNode(Node* node) {
        Node* prev = produce_end_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    Node stub_;
    alignas(64) std::atomic<Node*> consume_end_;  // tail: consumer reads here
    alignas(64) std::atomic<Node*> produce_end_;  // head: producers push here
};

} // namespace iouring::core
