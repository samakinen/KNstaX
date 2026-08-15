// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/inplace_function.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace knx {
namespace netip {

class SessionKeepaliveRunner {
public:
    using PulseFunction = util::InplaceFunction<void(), 32>;

    explicit SessionKeepaliveRunner(PulseFunction pulse = {})
        : pulse_(std::move(pulse))
    {
    }

    ~SessionKeepaliveRunner() { stop(); }

    SessionKeepaliveRunner(const SessionKeepaliveRunner&) = delete;
    SessionKeepaliveRunner& operator=(const SessionKeepaliveRunner&) = delete;

    void start(uint32_t intervalMs)
    {
        intervalMs_.store(intervalMs);
        if (active_.load()) {
            cv_.notify_all();
            return;
        }

        if (thread_.joinable()) {
            thread_.join();
        }

        active_.store(true);
        thread_ = std::thread([this]() {
            using clock = std::chrono::steady_clock;
            while (true) {
                if (!active_.load()) break;

                const uint32_t interval = intervalMs_.load();
                const auto deadline = clock::now() + std::chrono::milliseconds(interval);

                std::unique_lock<std::mutex> lock(mutex_);
                const auto status = cv_.wait_until(lock, deadline);
                lock.unlock();

                if (!active_.load()) break;
                if (status != std::cv_status::timeout) continue;

                if (pulse_) {
                    pulse_();
                }
            }
        });
    }

    void stop()
    {
        active_.store(false);
        cv_.notify_all();
        if (thread_.joinable()) {
            if (std::this_thread::get_id() == thread_.get_id()) {
                thread_.detach();
                return;
            }
            thread_.join();
        }
    }

    bool isActive() const noexcept
    {
        return active_.load();
    }

private:
    PulseFunction pulse_;
    std::atomic_bool active_{false};
    std::atomic<uint32_t> intervalMs_{60000};
    std::thread thread_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace netip
} // namespace knx