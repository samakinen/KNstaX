// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstdint>

namespace knx::security {

class ReplayWindow {
public:
    static constexpr size_t WINDOW_SIZE = 64;

    ReplayWindow() : highestSeq_(0), bitmap_(0) {}

    bool wouldAccept(uint64_t seq) const {
        if (seq > highestSeq_) return true;
        if (seq == highestSeq_) return false;
        const uint64_t diff = highestSeq_ - seq;
        if (diff >= WINDOW_SIZE) return false;
        const uint64_t bit = 1ULL << diff;
        return (bitmap_ & bit) == 0;
    }

    void accept(uint64_t seq) {
        if (seq > highestSeq_) {
            const uint64_t diff = seq - highestSeq_;
            if (diff < WINDOW_SIZE) {
                bitmap_ <<= diff;
                bitmap_ |= 1ULL;
            } else {
                bitmap_ = 1ULL;
            }
            highestSeq_ = seq;
            return;
        }

        const uint64_t diff = highestSeq_ - seq;
        if (diff < WINDOW_SIZE) {
            bitmap_ |= (1ULL << diff);
        }
    }

    void reset() {
        highestSeq_ = 0;
        bitmap_ = 0;
    }

    uint64_t highestSeq() const { return highestSeq_; }

private:
    uint64_t highestSeq_;
    uint64_t bitmap_;
};

} // namespace knx::security
