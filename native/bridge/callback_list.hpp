#pragma once

#include "guest/guest_address.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace codboz {

template <typename Callback> class CallbackList {
  public:
    void add(GuestAddr function, GuestAddr userData) {
        if (!function) {
            return;
        }
        const auto existing = std::find_if(
            callbacks_.begin(), callbacks_.end(),
            [function](const Callback &callback) { return callback.function == function; });
        if (existing == callbacks_.end()) {
            callbacks_.push_back({function, userData});
        } else {
            existing->userData = userData;
        }
    }

    void remove(GuestAddr function) {
        if (!function) {
            callbacks_.clear();
            return;
        }
        std::erase_if(callbacks_, [function](const Callback &callback) {
            return callback.function == function;
        });
    }

    const std::vector<Callback> &entries() const {
        return callbacks_;
    }

    std::size_t size() const {
        return callbacks_.size();
    }

  private:
    std::vector<Callback> callbacks_;
};

} // namespace codboz
