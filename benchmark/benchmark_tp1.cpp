// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/tp1_mac_physical.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace knx;
using namespace knx::physical;
using Clock = std::chrono::high_resolution_clock;

namespace {

struct BenchmarkResult {
    std::string name;
    size_t operations{0};
    double microseconds{0.0};

    double microsecondsPerOp() const {
        return operations == 0 ? 0.0 : microseconds / static_cast<double>(operations);
    }

    double opsPerSecond() const {
        return microseconds == 0.0 ? 0.0 : (static_cast<double>(operations) * 1000000.0) / microseconds;
    }
};

class BenchmarkBackend final : public Tp1MediumBackend {
public:
    util::Result<void> init(const Tp1MediumConfig& config) override {
        _config = config;
        _state = Tp1MediumState::Idle;
        _initialized = true;
        return util::Result<void>::ok();
    }

    void close() override {
        _initialized = false;
        _state = Tp1MediumState::Uninitialized;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        if (!_initialized) {
            return util::ErrorCode::NotInitialized;
        }
        return frame.size();
    }

    void setEventCallback(Tp1EventCallback callback, void* context) override {
        _callback = callback;
        _context = context;
    }

    Tp1MediumState getState() const override {
        return _state;
    }

    Tp1CapabilityProfile getCapabilities() const override {
        Tp1CapabilityProfile capabilities;
        capabilities.supportsDetailedTxConfirm = true;
        return capabilities;
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _config.busMonitorMode = enabled;
        return util::Result<void>::ok();
    }

    util::Result<void> service() override {
        return util::Result<void>::ok();
    }

    void emitFrame(std::span<const uint8_t> frame) {
        if (!_callback) {
            return;
        }

        Tp1RxEvent event;
        event.type = Tp1RxEventType::TelegramEnd;
        event.frame = frame;
        _callback(event, _context);
    }

private:
    Tp1EventCallback _callback{};
    void* _context{nullptr};
    Tp1MediumConfig _config{};
    Tp1MediumState _state{Tp1MediumState::Uninitialized};
    bool _initialized{false};
};

std::vector<uint8_t> makeFrame(uint8_t discriminator) {
    return {0xBC, 0x11, 0x01, 0x12, 0x34, discriminator, static_cast<uint8_t>(0xFFu - discriminator)};
}

BenchmarkResult benchmarkRxRoundTrip() {
    auto backend = std::make_unique<BenchmarkBackend>();
    auto* backendPtr = backend.get();
    Tp1MacPhysical physical(std::move(backend));

    if (physical.init().isError()) {
        throw std::runtime_error("failed to initialize TP1 benchmark physical");
    }

    constexpr size_t iterations = 10000;
    auto frame = makeFrame(0x42);

    auto start = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        backendPtr->emitFrame(frame);
        auto received = physical.receiveFrame(0);
        if (received.isError() || received.value().size() != frame.size()) {
            throw std::runtime_error("TP1 round-trip benchmark validation failed");
        }
    }
    auto end = Clock::now();

    physical.close();

    return {"TP1 MAC RX enqueue + dequeue", iterations,
            std::chrono::duration<double, std::micro>(end - start).count()};
}

BenchmarkResult benchmarkQueueOverflowRetention() {
    auto backend = std::make_unique<BenchmarkBackend>();
    auto* backendPtr = backend.get();
    Tp1MacPhysical physical(std::move(backend));

    if (physical.init().isError()) {
        throw std::runtime_error("failed to initialize TP1 overflow benchmark physical");
    }

    constexpr size_t extraFrames = 64;
    const size_t totalFrames = Tp1MacPhysical::RX_QUEUE_CAPACITY + extraFrames;

    auto start = Clock::now();
    for (size_t i = 0; i < totalFrames; ++i) {
        backendPtr->emitFrame(makeFrame(static_cast<uint8_t>(i & 0xFFu)));
    }

    for (size_t i = 0; i < Tp1MacPhysical::RX_QUEUE_CAPACITY; ++i) {
        auto received = physical.receiveFrame(0);
        if (received.isError()) {
            throw std::runtime_error("TP1 overflow benchmark failed to drain bounded queue");
        }

        const uint8_t expectedDiscriminator = static_cast<uint8_t>((extraFrames + i) & 0xFFu);
        if (received.value().size() < 6 || received.value()[5] != expectedDiscriminator) {
            throw std::runtime_error("TP1 overflow benchmark retained incorrect frames");
        }
    }
    auto end = Clock::now();

    auto emptyResult = physical.receiveFrame(0);
    if (!emptyResult.isError() || emptyResult.error() != util::ErrorCode::Timeout) {
        throw std::runtime_error("TP1 overflow benchmark expected empty bounded queue");
    }

    physical.close();

    return {"TP1 MAC overflow retain-latest", totalFrames,
            std::chrono::duration<double, std::micro>(end - start).count()};
}

void printResults(const std::vector<BenchmarkResult>& results) {
    std::cout << "\n" << std::string(88, '=') << "\n";
    std::cout << "TP1 Performance Benchmarks\n";
    std::cout << std::string(88, '=') << "\n\n";

    std::cout << std::left << std::setw(42) << "Operation"
              << std::right << std::setw(14) << "ops"
              << std::setw(16) << "µs/op"
              << std::setw(16) << "ops/sec" << "\n";
    std::cout << std::string(88, '-') << "\n";

    for (const auto& result : results) {
        std::cout << std::left << std::setw(42) << result.name
                  << std::right << std::setw(14) << result.operations
                  << std::setw(16) << std::fixed << std::setprecision(3) << result.microsecondsPerOp()
                  << std::setw(16) << std::fixed << std::setprecision(1) << result.opsPerSecond() << "\n";
    }

    std::cout << "\n" << std::string(88, '=') << "\n";
}

} // namespace

int main() {
    try {
        std::vector<BenchmarkResult> results;
        results.push_back(benchmarkRxRoundTrip());
        results.push_back(benchmarkQueueOverflowRetention());
        printResults(results);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "TP1 benchmark failed: " << ex.what() << '\n';
        return 1;
    }
}