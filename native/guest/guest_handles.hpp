#pragma once

#include "guest_address.hpp"

#include <cstdint>
#include <vector>

namespace codboz {

// Typed handle table for host objects exposed to the guest as 32-bit handles.
// A handle encodes a generation in its high 16 bits and a slot index in its low
// 16 bits, so stale handles (use-after-free) are detected. A handle value of 0
// is never returned and means "no object".
template <typename T> class HandleTable {
  public:
    GuestHandle create(T value) {
        uint32_t slot;
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            slot = static_cast<uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        Slot &s = slots_[slot];
        s.used = true;
        s.generation = static_cast<uint16_t>(s.generation + 1);
        if (s.generation == 0) {
            s.generation = 1;
        }
        s.value = std::move(value);
        return (static_cast<uint32_t>(s.generation) << 16) | slot;
    }

    T *get(GuestHandle handle) {
        uint32_t slot = handle & 0xFFFFu;
        uint16_t gen = static_cast<uint16_t>(handle >> 16);
        if (slot >= slots_.size()) {
            return nullptr;
        }
        Slot &s = slots_[slot];
        if (!s.used || s.generation != gen) {
            return nullptr;
        }
        return &s.value;
    }

    bool release(GuestHandle handle) {
        uint32_t slot = handle & 0xFFFFu;
        uint16_t gen = static_cast<uint16_t>(handle >> 16);
        if (slot >= slots_.size()) {
            return false;
        }
        Slot &s = slots_[slot];
        if (!s.used || s.generation != gen) {
            return false;
        }
        s.used = false;
        s.value = {};
        freeSlots_.push_back(slot);
        return true;
    }

    std::size_t size() const {
        return slots_.size();
    }

  private:
    struct Slot {
        bool used = false;
        uint16_t generation = 0;
        T value{};
    };
    std::vector<Slot> slots_;
    std::vector<uint32_t> freeSlots_;
};

} // namespace codboz
