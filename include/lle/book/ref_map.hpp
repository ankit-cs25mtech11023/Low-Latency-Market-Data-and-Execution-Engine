#pragma once
// Open-addressing hash map: ITCH order reference (u64) -> pool index (u32).
//
// Why not std::unordered_map: it allocates one heap node per entry (a new/delete on every
// add/delete message) and every lookup chases a pointer from the bucket array to that node,
// which is usually a cache miss. Here all entries live inline in one flat array allocated
// once at construction, so a lookup is: hash -> one slot -> (rarely) the next few slots,
// all in the same or the adjacent cache line.
//
// Why not a plain array indexed by ref ("direct vector"): E1 measured refs spanning 0..260 M
// with at most 1.96 M live at once, so a direct array would be ~130x larger than needed.
//
// Design:
//   * Capacity is a power of two, fixed at construction (no rehash on the hot path). The
//     caller sizes it for load <= 0.5 at the expected peak; E1 peak live = 1.96 M, so 2^22.
//   * Hash policy (template parameter, an E2 variable):
//       Fibonacci: multiply by 2^64/phi and keep the top bits. The textbook choice: it
//                  spreads any key pattern evenly, so consecutive refs land far apart.
//       Identity:  slot = ref mod capacity. ITCH refs are assigned nearly sequentially and
//                  most orders die young (E1: delete lifetime p50 1.5 s), so the live refs
//                  form a moving window of mostly consecutive numbers. Identity keeps them
//                  in consecutive slots: the add of ref r and r+1 touch the same cache line
//                  and the same 4 KiB page, while Fibonacci makes nearly every lookup a cache
//                  miss AND a TLB miss in a 64 MiB table. The price: identity is only good
//                  for keys like these; adversarial keys (all multiples of the capacity)
//                  would make probe runs long. Exchange-assigned refs are not adversarial.
//   * Linear probing: on collision try the next slot. Cache-friendly (the next slot is
//     usually in the same 64-byte line: 4 slots of 16 bytes per line).
//   * Backward-shift deletion: instead of leaving a "tombstone" when erasing (tombstones make
//     later lookups longer and need periodic cleanup), the following entries of the same
//     probe run are shifted back into the hole. The table never degrades over a long day.
//
// A slot is empty when its value is kEmpty, so every 64-bit key (including 0) is usable.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lle/protocol/itch.hpp"

namespace lle::book {

enum class RefHash : std::uint8_t { Fibonacci, Identity };

template <RefHash H>
class BasicRefMap {
public:
    static constexpr std::uint32_t kEmpty = 0xFFFF'FFFFu;

    // `capacity` is rounded up to a power of two. Throws if it is 0 or too large.
    explicit BasicRefMap(std::size_t capacity) {
        if (capacity == 0 || capacity > (std::size_t{1} << 31)) throw std::invalid_argument("RefMap capacity");
        const std::size_t cap = std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity);  // shift < 64
        shift_ = static_cast<unsigned>(64 - std::countr_zero(cap));
        mask_ = cap - 1;
        // Keep >= 1/8 of the slots (at least one) free: probe runs stay short, and every probe
        // loop is guaranteed to reach an empty slot.
        max_size_ = cap - (cap / 8 > 0 ? cap / 8 : 1);
        slots_.assign(cap, Slot{0, kEmpty, 0});
    }

    // Value for `key`, or kEmpty if absent.
    [[nodiscard]] std::uint32_t find(itch::OrderRef key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.val == kEmpty) return kEmpty;
            if (s.key == key) return s.val;
        }
    }

    // Inserts key -> val. Returns false (and changes nothing) if the key is already present.
    // Throws std::length_error when the table is full beyond its load limit: that means it
    // was sized wrongly for the input, which must be fixed, not silently tolerated.
    bool insert(itch::OrderRef key, std::uint32_t val) {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.val == kEmpty) break;
            if (s.key == key) return false;
        }
        if (size_ >= max_size_) throw std::length_error("RefMap full: increase capacity");
        slots_[i] = Slot{key, val, 0};
        ++size_;
        return true;
    }

    // Removes `key`; returns its value, or kEmpty if it was absent.
    std::uint32_t erase(itch::OrderRef key) noexcept {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.val == kEmpty) return kEmpty;
            if (s.key == key) break;
        }
        const std::uint32_t val = slots_[i].val;
        // Backward shift: walk the rest of the probe run. An entry at j whose home slot h is
        // NOT cyclically inside (i, j] would become unreachable once slot i is empty (its
        // probe from h would stop at the hole), so it moves into the hole, and the hole moves
        // to j. The run ends at the first empty slot.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].val == kEmpty) break;
            const std::size_t h = home(slots_[j].key);
            const bool h_in_i_j = i <= j ? (i < h && h <= j) : (i < h || h <= j);
            if (!h_in_i_j) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].val = kEmpty;
        --size_;
        return val;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        itch::OrderRef key;
        std::uint32_t val;
        std::uint32_t pad;
    };
    static_assert(sizeof(Slot) == 16, "4 slots per cache line");

    [[nodiscard]] std::size_t home(itch::OrderRef key) const noexcept {
        if constexpr (H == RefHash::Fibonacci)
            return static_cast<std::size_t>((key * 0x9E37'79B9'7F4A'7C15ull) >> shift_) & mask_;
        else
            return static_cast<std::size_t>(key) & mask_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::size_t max_size_ = 0;
    unsigned shift_ = 0;
};

using RefMap = BasicRefMap<RefHash::Identity>;
using RefMapFib = BasicRefMap<RefHash::Fibonacci>;

}  // namespace lle::book
