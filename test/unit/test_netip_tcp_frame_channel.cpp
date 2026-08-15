// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/secure_tcp_frame_channel.hpp"
#include "knx/netip/tcp_frame_channel.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

namespace {

class PrefixSecurity final : public NetIpSecurity {
public:
    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (in.size() < 6) return util::ErrorCode::DecodeFailed;
        if (out.size() < in.size() + 3) return util::ErrorCode::BufferTooSmall;

        std::memcpy(out.data(), in.data(), 6);
        const uint16_t wrappedLen = static_cast<uint16_t>(in.size() + 3);
        out[4] = static_cast<uint8_t>((wrappedLen >> 8) & 0xFF);
        out[5] = static_cast<uint8_t>(wrappedLen & 0xFF);
        out[6] = 'S';
        out[7] = 'E';
        out[8] = 'C';
        std::memcpy(out.data() + 9, in.data() + 6, in.size() - 6);
        return in.size() + 3;
    }

    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (in.size() < 9) return util::ErrorCode::DecodeFailed;
        if (in[6] != 'S' || in[7] != 'E' || in[8] != 'C') return util::ErrorCode::DecodeFailed;
        if (out.size() < in.size() - 3) return util::ErrorCode::BufferTooSmall;

        std::memcpy(out.data(), in.data(), 6);
        const uint16_t plainLen = static_cast<uint16_t>(in.size() - 3);
        out[4] = static_cast<uint8_t>((plainLen >> 8) & 0xFF);
        out[5] = static_cast<uint8_t>(plainLen & 0xFF);
        std::memcpy(out.data() + 6, in.data() + 9, in.size() - 9);
        return in.size() - 3;
    }
};

class TestTcpSocket final : public platform::TcpSocket {
public:
    util::Result<void> connect(IpAddress, uint16_t) override
    {
        open_ = true;
        return util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    int send(std::span<const uint8_t> data) override
    {
        if (!open_) return -1;
        const size_t chunk = sendChunkSize_ == 0 ? data.size() : std::min(sendChunkSize_, data.size());
        sent_.insert(sent_.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(chunk));
        return static_cast<int>(chunk);
    }

    int receive(std::span<uint8_t> buffer) override
    {
        if (!open_ || rx_.empty()) return -1;
        const size_t chunk = receiveChunkSize_ == 0 ? std::min(buffer.size(), rx_.size()) : std::min({receiveChunkSize_, buffer.size(), rx_.size()});
        std::memcpy(buffer.data(), rx_.data(), chunk);
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(chunk));
        return static_cast<int>(chunk);
    }

    size_t available() const override { return rx_.size(); }
    uint16_t localPort() const override { return 0; }

    void setSendChunkSize(size_t chunkSize) { sendChunkSize_ = chunkSize; }
    void setReceiveChunkSize(size_t chunkSize) { receiveChunkSize_ = chunkSize; }
    void pushRx(std::span<const uint8_t> bytes) { rx_.insert(rx_.end(), bytes.begin(), bytes.end()); }

    const std::vector<uint8_t>& sent() const { return sent_; }

private:
    bool open_{true};
    size_t sendChunkSize_{0};
    size_t receiveChunkSize_{0};
    std::vector<uint8_t> rx_{};
    std::vector<uint8_t> sent_{};
};

constexpr std::array<uint8_t, 8> kPlainFrame = {0x06, 0x10, 0x02, 0x05, 0x00, 0x08, 0xAA, 0x55};
constexpr std::array<uint8_t, 11> kWrappedFrame = {0x06, 0x10, 0x02, 0x05, 0x00, 0x0B, 0x53, 0x45, 0x43, 0xAA, 0x55};

} // namespace

void test_tcp_frame_channel_sends_full_frame_across_partial_writes(void)
{
    TestTcpSocket socket;
    socket.setSendChunkSize(3);

    auto result = TcpFrameChannel::sendFrame(socket, kPlainFrame);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(kPlainFrame.size(), socket.sent().size());
    for (size_t i = 0; i < kPlainFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kPlainFrame[i], socket.sent()[i]);
    }
}

void test_tcp_frame_channel_receives_full_frame_across_partial_reads(void)
{
    TestTcpSocket socket;
    socket.setReceiveChunkSize(2);
    socket.pushRx(kPlainFrame);

    std::array<uint8_t, 16> frameBuffer{};
    auto result = TcpFrameChannel::receiveFrame(socket, frameBuffer);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(kPlainFrame.size(), result.value());
    for (size_t i = 0; i < kPlainFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kPlainFrame[i], frameBuffer[i]);
    }
}

void test_tcp_frame_channel_uses_security_wrapper(void)
{
    TestTcpSocket socket;
    PrefixSecurity security;
    std::array<uint8_t, 32> wrapScratch{};
    std::array<uint8_t, 32> unwrapScratch{};
    std::array<uint8_t, 32> frameBuffer{};
    SecureTcpFrameChannel channel(socket, security, wrapScratch, unwrapScratch);

    auto sendResult = channel.sendFrame(kPlainFrame);
    TEST_ASSERT_TRUE(sendResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(kWrappedFrame.size(), socket.sent().size());
    for (size_t i = 0; i < kWrappedFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kWrappedFrame[i], socket.sent()[i]);
    }

    socket.pushRx(kWrappedFrame);
    auto receiveResult = channel.receiveFrame(frameBuffer);
    TEST_ASSERT_TRUE(receiveResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(kPlainFrame.size(), receiveResult.value());
    for (size_t i = 0; i < kPlainFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kPlainFrame[i], frameBuffer[i]);
    }
}

void test_secure_tcp_frame_channel_binds_security_once(void)
{
    TestTcpSocket socket;
    PrefixSecurity security;
    std::array<uint8_t, 32> wrapScratch{};
    std::array<uint8_t, 32> unwrapScratch{};
    std::array<uint8_t, 32> frameBuffer{};

    SecureTcpFrameChannel channel(socket, security, wrapScratch, unwrapScratch);

    auto sendResult = channel.sendFrame(kPlainFrame);
    TEST_ASSERT_TRUE(sendResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(kWrappedFrame.size(), socket.sent().size());
    for (size_t i = 0; i < kWrappedFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kWrappedFrame[i], socket.sent()[i]);
    }

    socket.pushRx(kWrappedFrame);
    auto receiveResult = channel.receiveFrame(frameBuffer);
    TEST_ASSERT_TRUE(receiveResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(kPlainFrame.size(), receiveResult.value());
    for (size_t i = 0; i < kPlainFrame.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(kPlainFrame[i], frameBuffer[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tcp_frame_channel_sends_full_frame_across_partial_writes);
    RUN_TEST(test_tcp_frame_channel_receives_full_frame_across_partial_reads);
    RUN_TEST(test_tcp_frame_channel_uses_security_wrapper);
    RUN_TEST(test_secure_tcp_frame_channel_binds_security_once);
    return UNITY_END();
}