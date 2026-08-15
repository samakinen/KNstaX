// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/virtual_tp1_trace.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace knx {
namespace physical {

namespace {

bool parseUint64(const std::string& token, uint64_t& value)
{
    const char* begin = token.data();
    const char* end = begin + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

bool parseUint32(const std::string& token, uint32_t& value)
{
    const char* begin = token.data();
    const char* end = begin + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

bool parseInt32(const std::string& token, int32_t& value)
{
    const char* begin = token.data();
    const char* end = begin + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

std::vector<std::string> split(const std::string& line, char delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream stream(line);
    std::string token;
    while (std::getline(stream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

} // namespace

std::string VirtualTp1Trace::encode(const Header& header, const std::vector<Event>& events)
{
    std::ostringstream out;
    out << "TRACE|" << header.traceVersionMajor << '|' << header.traceVersionMinor << '|'
        << header.simProfile << '|' << header.seed << '\n';

    for (const auto& event : events) {
        out << event.tsUs << '|'
            << static_cast<uint32_t>(event.type) << '|'
            << static_cast<uint32_t>(event.source) << '|'
            << event.pin << '|'
            << event.level << '|'
            << event.meta << '\n';
    }

    return out.str();
}

bool VirtualTp1Trace::decode(const std::string& payload, Header& outHeader, std::vector<Event>& outEvents)
{
    outEvents.clear();

    std::stringstream stream(payload);
    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }

    const auto headerTokens = split(line, '|');
    if (headerTokens.size() != 5 || headerTokens[0] != "TRACE") {
        return false;
    }

    if (!parseUint32(headerTokens[1], outHeader.traceVersionMajor) ||
        !parseUint32(headerTokens[2], outHeader.traceVersionMinor) ||
        !parseUint32(headerTokens[4], outHeader.seed)) {
        return false;
    }

    if (outHeader.traceVersionMajor != 1u) {
        return false;
    }

    outHeader.simProfile = headerTokens[3];

    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }

        const auto tokens = split(line, '|');
        if (tokens.size() != 6) {
            return false;
        }

        Event event;
        uint64_t tsUs = 0;
        uint32_t type = 0;
        uint32_t source = 0;
        uint32_t pin = 0;
        int32_t level = -1;
        uint32_t meta = 0;

        if (!parseUint64(tokens[0], tsUs) ||
            !parseUint32(tokens[1], type) ||
            !parseUint32(tokens[2], source) ||
            !parseUint32(tokens[3], pin) ||
            !parseInt32(tokens[4], level) ||
            !parseUint32(tokens[5], meta)) {
            return false;
        }

        event.tsUs = tsUs;
        event.type = static_cast<EventType>(type);
        event.source = static_cast<EventSource>(source);
        event.pin = static_cast<uint16_t>(pin & 0xFFFFu);
        event.level = level;
        event.meta = meta;
        outEvents.push_back(event);
    }

    std::stable_sort(outEvents.begin(), outEvents.end(), [](const Event& lhs, const Event& rhs) {
        return lhs.tsUs < rhs.tsUs;
    });

    return true;
}

std::vector<VirtualTp1Trace::Event> VirtualTp1Trace::captureTxTransitions(
    const std::vector<TimerGpioHalVirtualBus::TxTransition>& transitions)
{
    std::vector<Event> events;
    events.reserve(transitions.size());
    for (const auto& transition : transitions) {
        Event event;
        event.tsUs = transition.timestampUs;
        event.type = EventType::TxLevelSet;
        event.source = EventSource::Driver;
        event.pin = transition.pin;
        event.level = static_cast<int32_t>(transition.level);
        event.meta = 0;
        events.push_back(event);
    }
    return events;
}

bool VirtualTp1Trace::replayToVirtualBus(TimerGpioHalVirtualBus& bus, const std::vector<Event>& events)
{
    for (const auto& event : events) {
        if (event.type != EventType::GpioEdge) {
            continue;
        }

        if (!bus.scheduleRxLevelAtUs(event.tsUs, static_cast<uint8_t>(event.level & 0x1))) {
            return false;
        }
    }
    return true;
}

} // namespace physical
} // namespace knx
