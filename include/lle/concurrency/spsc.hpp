#pragma once
// Single-producer / single-consumer (SPSC) queues: one lock-free ring in several
// configurations, plus two mutex-based baselines. E3 measures them against each other.
//
// Why SPSC at all: in a pipeline (feed thread -> book thread -> strategy thread) every hop
// has exactly one writer and one reader. With one writer per index, neither side ever
// needs a read-modify-write (no CAS, no lock): the producer alone writes `tail`, the
// consumer alone writes `head`, and each only *reads* the other's index. The whole
// synchronization is one store and one load per message (see docs/concurrency.md for the
// happens-before argument).
//
// The ring:
//   * Capacity is a power of two, so slot = index & (Capacity - 1) (an AND, not a division).
//   * head/tail are 64-bit counters that only grow; they never wrap in practice (2^64
//     messages), so "full" is tail - head == Capacity and "empty" is tail == head, without
//     the classic "waste one slot" trick.
//
// Configuration knobs (template parameters), each one an E3 variant:
//   * Ordering::kSeqCst  every atomic access is sequentially consistent (std::atomic's
//                        default). On x86 a seq_cst store compiles to XCHG, a locked
//                        instruction that drains the store buffer: a full barrier per message.
//   * Ordering::kAcqRel  release store of the own index, acquire load of the other's. On x86
//                        both are plain MOVs (x86 is TSO: stores are not reordered with older
//                        stores, loads not with older loads); the ordering costs only
//                        compiler constraints. This is the minimum the protocol needs.
//   * kCached = true     each side keeps a private copy of the other side's index and
//                        re-reads the shared one only when the ring *looks* full (producer)
//                        or empty (consumer). Without it, every push reads `head` and every
//                        pop reads `tail`, so the line holding the other index bounces
//                        between the cores on every message even when there is plenty of
//                        room / data. With it, the shared index is read about once per
//                        "catch-up" instead of once per message.
//   * kPad = 0 / 64 / 128  alignment of the producer-owned and consumer-owned index blocks.
//                        0 packs both into one cache line, so every index write by one core
//                        invalidates the line the other core is reading (false sharing).
//                        64 puts them on separate lines. 128 also keeps them out of the same
//                        128-byte line pair, which Intel's adjacent-line (spatial) prefetcher
//                        fetches together.
// The slot array is always aligned to 128 bytes, so only the index layout varies with kPad.

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace lle::concurrency {

enum class Ordering : std::uint8_t { kSeqCst, kAcqRel };

namespace detail {
// alignas(0) is ignored by the language, so "no padding" falls back to the natural alignment.
template <std::size_t Pad>
inline constexpr std::size_t kAlign = Pad == 0 ? alignof(std::atomic<std::uint64_t>) : Pad;
}  // namespace detail

template <class T, std::size_t Capacity, Ordering Ord, bool kCached, std::size_t kPad>
class SpscRing {
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied with plain assignment");
    static_assert(kPad == 0 || kPad == 64 || kPad == 128, "padding: 0, 64 or 128 bytes");

    static constexpr std::uint64_t kMask = Capacity - 1;
    static constexpr auto kLoad = Ord == Ordering::kSeqCst ? std::memory_order_seq_cst : std::memory_order_acquire;
    static constexpr auto kStore = Ord == Ordering::kSeqCst ? std::memory_order_seq_cst : std::memory_order_release;
    // Reading one's *own* index needs no ordering (nobody else writes it); the seq_cst variant
    // still uses seq_cst so it really is "default atomics everywhere".
    static constexpr auto kOwnLoad = Ord == Ordering::kSeqCst ? std::memory_order_seq_cst : std::memory_order_relaxed;

public:
    static constexpr std::size_t kCapacity = Capacity;

    // Producer thread only. Returns false if the ring is full.
    bool try_push(const T& v) noexcept {
        const std::uint64_t t = prod_.tail.load(kOwnLoad);
        if constexpr (kCached) {
            if (t - prod_.head_cache == Capacity) {
                prod_.head_cache = cons_.head.load(kLoad);  // looks full: refresh the copy
                if (t - prod_.head_cache == Capacity) return false;
            }
        } else {
            if (t - cons_.head.load(kLoad) == Capacity) return false;
        }
        buf_[t & kMask] = v;
        // Publishes the slot: a consumer that sees tail == t + 1 also sees buf_[t] (release).
        prod_.tail.store(t + 1, kStore);
        return true;
    }

    // Consumer thread only. Returns false if the ring is empty.
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = cons_.head.load(kOwnLoad);
        if constexpr (kCached) {
            if (h == cons_.tail_cache) {
                cons_.tail_cache = prod_.tail.load(kLoad);  // looks empty: refresh the copy
                if (h == cons_.tail_cache) return false;
            }
        } else {
            if (h == prod_.tail.load(kLoad)) return false;
        }
        out = buf_[h & kMask];
        // Frees the slot: the producer may overwrite buf_[h] only after it sees head == h + 1,
        // and the release orders our read of the slot before that.
        cons_.head.store(h + 1, kStore);
        return true;
    }

private:
    // Each block holds what one side writes. head_cache/tail_cache are private to their side
    // (plain fields), so they sit next to the index that side writes.
    struct alignas(detail::kAlign<kPad>) ProducerSide {
        std::atomic<std::uint64_t> tail{0};
        std::uint64_t head_cache = 0;
    };
    struct alignas(detail::kAlign<kPad>) ConsumerSide {
        std::atomic<std::uint64_t> head{0};
        std::uint64_t tail_cache = 0;
    };

    ProducerSide prod_;
    ConsumerSide cons_;
    alignas(128) std::array<T, Capacity> buf_{};
};

// Baseline 1: a ring under one std::mutex; an empty consumer / full producer sleeps on a
// condition variable (futex) until the other side signals. The textbook "blocking queue".
// Every message costs a lock + unlock on both sides (two locked RMW instructions each) and,
// whenever a side was asleep, a futex wake system call plus a scheduler wake-up.
template <class T, std::size_t Capacity>
class MutexCvQueue {
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");

public:
    static constexpr std::size_t kCapacity = Capacity;

    void push(const T& v) {
        std::unique_lock lock(m_);
        not_full_.wait(lock, [&] { return tail_ - head_ < Capacity; });
        buf_[tail_++ & (Capacity - 1)] = v;
        lock.unlock();
        not_empty_.notify_one();  // no system call if nobody is waiting
    }

    void pop(T& out) {
        std::unique_lock lock(m_);
        not_empty_.wait(lock, [&] { return tail_ != head_; });
        out = buf_[head_++ & (Capacity - 1)];
        lock.unlock();
        not_full_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable not_empty_, not_full_;
    std::uint64_t head_ = 0, tail_ = 0;
    std::array<T, Capacity> buf_{};
};

// Baseline 2: the same ring under one std::mutex, but nobody sleeps: callers poll
// try_push/try_pop in a loop. Removes the futex wake-up from the latency, keeps the lock
// (both threads write the mutex word, so its cache line bounces on every attempt).
template <class T, std::size_t Capacity>
class MutexSpinQueue {
    static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");

public:
    static constexpr std::size_t kCapacity = Capacity;

    bool try_push(const T& v) {
        std::lock_guard lock(m_);
        if (tail_ - head_ == Capacity) return false;
        buf_[tail_++ & (Capacity - 1)] = v;
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard lock(m_);
        if (tail_ == head_) return false;
        out = buf_[head_++ & (Capacity - 1)];
        return true;
    }

private:
    std::mutex m_;
    std::uint64_t head_ = 0, tail_ = 0;
    std::array<T, Capacity> buf_{};
};

// Uniform blocking interface for tests and benchmarks. The polling queues busy-spin
// without PAUSE: the measured threads own their cores, and PAUSE would add ~140 cycles to
// the reaction time on this CPU (Skylake-derived cores lengthened it). On SMT siblings the
// spinning thread competes with its sibling for issue slots; E3's placement sweep shows it.
template <class Q, class T>
inline void push_blocking(Q& q, const T& v) {
    if constexpr (requires { q.push(v); }) {
        q.push(v);
    } else {
        while (!q.try_push(v)) {
        }
    }
}

template <class Q, class T>
inline void pop_blocking(Q& q, T& out) {
    if constexpr (requires { q.pop(out); }) {
        q.pop(out);
    } else {
        while (!q.try_pop(out)) {
        }
    }
}

}  // namespace lle::concurrency
