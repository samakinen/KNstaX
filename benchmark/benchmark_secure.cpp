// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <chrono>
#include <iostream>
#include <vector>
#include <iomanip>
#include "knx/security/data_secure.hpp"
#include "knx/security/key_derivation.hpp"

using namespace knx::security;
using knx::GroupAddress;
using knx::IndividualAddress;
using Clock = std::chrono::high_resolution_clock;

struct BenchmarkResult {
    std::string name;
    size_t operations;
    double microseconds;

    double opsPerSecond() const {
        return operations * 1e6 / microseconds;
    }

    double microsecondsPerOp() const {
        return microseconds / operations;
    }
};

std::vector<BenchmarkResult> results;

void benchmarkEncryption() {
    DataSecureSession::Key key{};
    for (int i = 0; i < 16; ++i) key[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

    DataSecureSession session(key);

    DataSecureContext ctx{IndividualAddress(0x1101), GroupAddress(0x0F81), 0xE0, 0};
    std::vector<uint8_t> plaintext(64);
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i);
    }
    // Ensure minimal APDU (>= 2 bytes)
    plaintext[0] = 0x00;
    plaintext[1] = 0x80;
    std::vector<uint8_t> secure;

    constexpr size_t ITERATIONS = 1000;
    auto start = Clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        (void)session.protect(ctx, plaintext, secure);
    }
    auto end = Clock::now();

    double microseconds = std::chrono::duration<double, std::micro>(end - start).count();
    results.push_back({"KNX Data Secure protect (64-byte)", ITERATIONS, microseconds});
}

void benchmarkDecryption() {
    DataSecureSession::Key key{};
    for (int i = 0; i < 16; ++i) key[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

    DataSecureSession tx_session(key);
    DataSecureSession rx_session(key);

    DataSecureContext ctx{IndividualAddress(0x1101), GroupAddress(0x0F81), 0xE0, 0};

    std::vector<uint8_t> plaintext(64);
    for (size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> decrypted;

    // Prepare encrypted messages with monotonically increasing sequence
    constexpr size_t ITERATIONS = 1000;
    std::vector<std::vector<uint8_t>> secured;
    secured.reserve(ITERATIONS);
    for (size_t i = 0; i < ITERATIONS; ++i) {
        std::vector<uint8_t> s;
        (void)tx_session.protect(ctx, plaintext, s);
        secured.push_back(std::move(s));
    }
    auto start = Clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        decrypted.clear();
        (void)rx_session.unprotect(ctx, secured[i], decrypted);
    }
    auto end = Clock::now();

    double microseconds = std::chrono::duration<double, std::micro>(end - start).count();
    results.push_back({"KNX Data Secure unprotect (64-byte)", ITERATIONS, microseconds});
}

void benchmarkKeyDerivation() {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = i;

    KeyDerivation::Key session_key;

    constexpr size_t ITERATIONS = 10;
    auto start = Clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        KeyDerivation::DeviceId device_id{static_cast<uint8_t>(i >> 8),
                                         static_cast<uint8_t>(i & 0xFF)};
        (void)KeyDerivation::deriveSessionKey(master_key, device_id, session_key).isOk();
    }
    auto end = Clock::now();

    double microseconds = std::chrono::duration<double, std::micro>(end - start).count();
    results.push_back({"Key Derivation (PBKDF2-SHA256)", ITERATIONS, microseconds});
}

void benchmarkLargePayloads() {
    DataSecureSession::Key key{};
    for (int i = 0; i < 16; ++i) key[static_cast<size_t>(i)] = static_cast<uint8_t>(i);

    DataSecureSession session(key);

    DataSecureContext ctx{IndividualAddress(0x1101), GroupAddress(0x0F81), 0xE0, 0};
    std::vector<uint8_t> secure;

    // Test different payload sizes
    std::vector<size_t> payload_sizes{128, 256, 512, 1024};

    for (size_t payload_size : payload_sizes) {
        std::vector<uint8_t> plaintext(payload_size);
        for (size_t i = 0; i < plaintext.size(); ++i) {
            plaintext[i] = static_cast<uint8_t>(i % 256);
        }
        if (plaintext.size() >= 2) {
            plaintext[0] = 0x00;
            plaintext[1] = 0x80;
        }

        constexpr size_t ITERATIONS = 50;
        auto start = Clock::now();
        for (size_t i = 0; i < ITERATIONS; ++i) {
            (void)session.protect(ctx, plaintext, secure);
        }
        auto end = Clock::now();

        double microseconds = std::chrono::duration<double, std::micro>(end - start).count();
        std::string name = "protect (" + std::to_string(payload_size) + "-byte payload)";
        results.push_back({name, ITERATIONS, microseconds});
    }
}

void printResults() {
    std::cout << "\n" << std::string(90, '=') << "\n";
    std::cout << "KNX Secure Performance Benchmark Results\n";
    std::cout << std::string(90, '=') << "\n\n";

    std::cout << std::left << std::setw(50) << "Operation"
              << std::right << std::setw(15) << "µs/op"
              << std::setw(20) << "ops/second" << "\n";
    std::cout << std::string(90, '-') << "\n";

    for (const auto& result : results) {
        std::cout << std::left << std::setw(50) << result.name
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(15) << result.microsecondsPerOp()
                  << std::setw(20) << result.opsPerSecond() << "\n";
    }

    std::cout << "\n" << std::string(90, '=') << "\n";
}

int main() {
    std::cout << "Starting KNX Secure performance benchmarks...\n\n";

    benchmarkEncryption();
    benchmarkDecryption();
    benchmarkKeyDerivation();
    benchmarkLargePayloads();

    printResults();

    return 0;
}
