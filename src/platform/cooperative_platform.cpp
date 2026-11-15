// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/platform/cooperative_platform.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace knx::platform {

namespace {

using util::ErrorCode;
using util::Result;

/// Longest log line handed to the sink; longer output is truncated.
constexpr size_t kLogLineCapacity = 96;

/// Non-null handle returned for every mutex. A no-op lock needs no state,
/// but callers treat a null handle as "creation failed".
char g_mutexToken;

}  // namespace

/// Fixed ring of equally sized items; storage is allocated once in createQueue().
struct CooperativePlatform::Ring {
    std::vector<uint8_t> storage;
    size_t itemSize{0};
    size_t capacity{0};
    size_t head{0};
    size_t count{0};
};

struct CooperativePlatform::EventGroup {
    uint32_t bits{0};
};

struct CooperativePlatform::Semaphore {
    bool given{false};
};

CooperativePlatform::CooperativePlatform(const CooperativeHooks& hooks) : _hooks(hooks) {}

CooperativePlatform::~CooperativePlatform() = default;

bool CooperativePlatform::consumeNotification() {
    const bool was = _notified;
    _notified = false;
    return was;
}

void CooperativePlatform::restart() {
    if (_hooks.restart) {
        _hooks.restart(_hooks.context);
    }
    for (;;) {
    }
}

void CooperativePlatform::fatalError() {
    if (_hooks.fatalError) {
        _hooks.fatalError(_hooks.context);
    }
    for (;;) {
    }
}

uint32_t CooperativePlatform::millis() const {
    return _hooks.millis ? _hooks.millis(_hooks.context) : 0u;
}

uint64_t CooperativePlatform::micros() const {
    return _hooks.micros ? _hooks.micros(_hooks.context) : 0u;
}

void CooperativePlatform::delay(uint32_t ms) {
    const uint32_t start = millis();
    while (static_cast<uint32_t>(millis() - start) < ms) {
        if (_hooks.idle) {
            _hooks.idle(_hooks.context);
        }
    }
}

void CooperativePlatform::delayMicroseconds(uint32_t us) {
    const uint64_t start = micros();
    while (static_cast<uint64_t>(micros() - start) < us) {
    }
}

uint32_t CooperativePlatform::uniqueSerialNumber() const {
    return _hooks.serialNumber ? _hooks.serialNumber(_hooks.context) : 0u;
}

void CooperativePlatform::macAddress(std::span<uint8_t, 6> mac) const {
    // No Ethernet interface on this class of target; derive a stable value
    // from the serial number so callers that ask still get a unique one.
    const uint32_t serial = uniqueSerialNumber();
    mac[0] = 0x02;  // locally administered, unicast
    mac[1] = 0x00;
    mac[2] = static_cast<uint8_t>(serial >> 24);
    mac[3] = static_cast<uint8_t>(serial >> 16);
    mac[4] = static_cast<uint8_t>(serial >> 8);
    mac[5] = static_cast<uint8_t>(serial);
}

NetworkInterface* CooperativePlatform::network() {
    return nullptr;
}

TaskHandle CooperativePlatform::createTask(const TaskConfig& /*config*/) {
    // There is no scheduler. Callers learn this through the null handle
    // (Tp1DataLinkLayer reports ResourceUnavailable) and must use the
    // polled path instead.
    return nullptr;
}

void CooperativePlatform::deleteTask(TaskHandle /*task*/) {}

TaskHandle CooperativePlatform::currentTask() {
    return nullptr;
}

void CooperativePlatform::taskDelay(uint32_t ms) {
    delay(ms);
}

void CooperativePlatform::taskYield() {}

Result<void> CooperativePlatform::taskNotifyGive(TaskHandle /*task*/) {
    _notified = true;
    return Result<void>::ok();
}

Result<void> CooperativePlatform::taskNotifyGiveFromISR(TaskHandle /*task*/) {
    _notified = true;
    return Result<void>::ok();
}

uint32_t CooperativePlatform::taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t /*timeout_ms*/) {
    // Never blocks: a wait could not be ended by anything but an interrupt,
    // and the main loop owns the decision to sleep.
    const bool was = _notified;
    if (clearMode == TaskNotifyClearMode::Clear) {
        _notified = false;
    }
    return was ? 1u : 0u;
}

MutexHandle CooperativePlatform::createMutex() {
    return &g_mutexToken;
}

void CooperativePlatform::deleteMutex(MutexHandle /*mutex*/) {}

Result<void> CooperativePlatform::lockMutex(MutexHandle /*mutex*/, uint32_t /*timeout_ms*/) {
    return Result<void>::ok();
}

void CooperativePlatform::unlockMutex(MutexHandle /*mutex*/) {}

QueueHandle CooperativePlatform::createQueue(size_t itemSize, size_t length) {
    if (itemSize == 0 || length == 0) {
        return nullptr;
    }
    auto* ring = new Ring();
    ring->storage.resize(itemSize * length);
    ring->itemSize = itemSize;
    ring->capacity = length;
    return ring;
}

void CooperativePlatform::deleteQueue(QueueHandle queue) {
    delete static_cast<Ring*>(queue);
}

Result<void> CooperativePlatform::queueSend(QueueHandle queue, const void* item, uint32_t /*timeout_ms*/) {
    auto* ring = static_cast<Ring*>(queue);
    if (ring == nullptr || item == nullptr) {
        return ErrorCode::InvalidParameter;
    }
    if (ring->count == ring->capacity) {
        return ErrorCode::ResourceUnavailable;
    }
    const size_t tail = (ring->head + ring->count) % ring->capacity;
    std::memcpy(ring->storage.data() + tail * ring->itemSize, item, ring->itemSize);
    ++ring->count;
    return Result<void>::ok();
}

Result<void> CooperativePlatform::queueReceive(QueueHandle queue, void* item, uint32_t /*timeout_ms*/) {
    auto* ring = static_cast<Ring*>(queue);
    if (ring == nullptr || item == nullptr) {
        return ErrorCode::InvalidParameter;
    }
    if (ring->count == 0) {
        return ErrorCode::Timeout;
    }
    std::memcpy(item, ring->storage.data() + ring->head * ring->itemSize, ring->itemSize);
    ring->head = (ring->head + 1) % ring->capacity;
    --ring->count;
    return Result<void>::ok();
}

size_t CooperativePlatform::queueCount(QueueHandle queue) {
    const auto* ring = static_cast<const Ring*>(queue);
    return ring != nullptr ? ring->count : 0u;
}

EventGroupHandle CooperativePlatform::createEventGroup() {
    return new EventGroup();
}

void CooperativePlatform::deleteEventGroup(EventGroupHandle group) {
    delete static_cast<EventGroup*>(group);
}

void CooperativePlatform::eventGroupSetBits(EventGroupHandle group, uint32_t bits) {
    if (auto* g = static_cast<EventGroup*>(group)) {
        g->bits |= bits;
    }
}

void CooperativePlatform::eventGroupClearBits(EventGroupHandle group, uint32_t bits) {
    if (auto* g = static_cast<EventGroup*>(group)) {
        g->bits &= ~bits;
    }
}

uint32_t CooperativePlatform::eventGroupWaitBits(EventGroupHandle group, uint32_t bitsToWaitFor,
                                                 EventGroupClearMode clearOnExit,
                                                 EventGroupWaitMode waitForAll, uint32_t /*timeout_ms*/) {
    auto* g = static_cast<EventGroup*>(group);
    if (g == nullptr) {
        return 0u;
    }
    // Report the bits as they are now; never wait (see file comment).
    const uint32_t seen = g->bits;
    const uint32_t matched = seen & bitsToWaitFor;
    const bool satisfied = waitForAll == EventGroupWaitMode::All ? matched == bitsToWaitFor : matched != 0u;
    if (satisfied && clearOnExit == EventGroupClearMode::Clear) {
        g->bits &= ~bitsToWaitFor;
    }
    return seen;
}

SemaphoreHandle CooperativePlatform::createBinarySemaphore() {
    return new Semaphore();
}

void CooperativePlatform::deleteSemaphore(SemaphoreHandle sem) {
    delete static_cast<Semaphore*>(sem);
}

Result<void> CooperativePlatform::semaphoreGive(SemaphoreHandle sem) {
    auto* s = static_cast<Semaphore*>(sem);
    if (s == nullptr) {
        return ErrorCode::InvalidParameter;
    }
    s->given = true;
    return Result<void>::ok();
}

Result<void> CooperativePlatform::semaphoreTake(SemaphoreHandle sem, uint32_t /*timeout_ms*/) {
    auto* s = static_cast<Semaphore*>(sem);
    if (s == nullptr) {
        return ErrorCode::InvalidParameter;
    }
    if (!s->given) {
        return ErrorCode::Timeout;
    }
    s->given = false;
    return Result<void>::ok();
}

void CooperativePlatform::log(const char* level, const char* tag, const char* format, ...) {
    if (_hooks.logLine == nullptr) {
        return;
    }
    char line[kLogLineCapacity];
    int n = std::snprintf(line, sizeof line, "%s %s: ", level != nullptr ? level : "", tag != nullptr ? tag : "");
    if (n < 0) {
        return;
    }
    const size_t prefix = std::min(static_cast<size_t>(n), sizeof line - 1);
    va_list args;
    va_start(args, format);
    std::vsnprintf(line + prefix, sizeof line - prefix, format, args);
    va_end(args);
    _hooks.logLine(_hooks.context, line);
}

}  // namespace knx::platform
