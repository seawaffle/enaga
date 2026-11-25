// Enaga: a queue from one context to the other, for note events, parameter changes or anything else a main-context
// callback hands to audio() (or back). One side only pushes and the other only pops; neither waits or allocates.
//
//     struct Event { uint8_t type, note, velocity; };
//     enaga::Queue<Event, 64> events_;                     // a member of the app
//     events_.push({1, 60, 100});                          // in on_pad, on_key, on_midi ...: false if full
//     Event e;
//     while (events_.pop(e)) handle(e);                    // in audio()
//
// It is safe because the C674x has one core: the audio interrupt can come between any two instructions of the
// main context, but each side's index is written only by that side, and a compiler fence keeps an item's copy
// ahead of the index that publishes it. Not for two producers or two consumers.
#pragma once

#include <atomic>
#include <cstdint>

namespace enaga {

// Holds up to N items of T (N a power of two, 2 or more; T copyable).
template <class T, uint32_t N> class Queue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "Queue size must be a power of two");

public:
    // The producer's side: copies item in, or returns false (nothing changed) when the queue is full.
    bool push(const T &item) {
        uint32_t w = write_;
        if (w - read_ >= N) return false;
        items_[w & (N - 1)] = item;
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the item is in place before the index moves
        write_ = w + 1;
        return true;
    }

    // The consumer's side: the oldest item into out, or false when the queue is empty.
    bool pop(T &out) {
        uint32_t r = read_;
        if (r == write_) return false;
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the index is read before the item
        out = items_[r & (N - 1)];
        std::atomic_signal_fence(std::memory_order_seq_cst);   // the item is copied before its slot is freed
        read_ = r + 1;
        return true;
    }

    // Items waiting, as seen from either side (the other side may change it at any moment).
    uint32_t size() const { return write_ - read_; }
    bool empty() const { return size() == 0; }
    static constexpr uint32_t capacity() { return N; }

private:
    T items_[N] = {};
    volatile uint32_t write_ = 0, read_ = 0;
};

}  // namespace enaga
