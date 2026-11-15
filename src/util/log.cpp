// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/util/log.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include "knx/platform/mutex.hpp"
#include <string_view>

#if __has_include("esp_log.h")
#include "esp_log.h"
#endif

namespace knx {
namespace log {
namespace {

void defaultSink(Level level, const char* tag, const char* fmt, va_list args) {
#if __has_include("esp_log.h")
    esp_log_level_t espLevel = ESP_LOG_INFO;
    char levelChar = 'I';
    switch (level) {
        case Level::Error:   espLevel = ESP_LOG_ERROR;   levelChar = 'E'; break;
        case Level::Warn:    espLevel = ESP_LOG_WARN;    levelChar = 'W'; break;
        case Level::Info:    espLevel = ESP_LOG_INFO;    levelChar = 'I'; break;
        case Level::Debug:   espLevel = ESP_LOG_DEBUG;   levelChar = 'D'; break;
        case Level::Verbose: espLevel = ESP_LOG_VERBOSE; levelChar = 'V'; break;
    }

    // esp_log_writev() emits the format string verbatim — the "I (1234) TAG: "
    // prefix that ESP_LOGx output carries is baked in by the ESP_LOGx macros,
    // not by the write function. Formatting the message first and then emitting
    // it with the prefix is what makes KNX lines attributable in a mixed log:
    // without it they appear as bare sentences among the IDF's tagged output,
    // with no level, no timestamp and no module to trace them back to.
    const char* effectiveTag = (tag != nullptr && tag[0] != '\0') ? tag : "KNX";
    char message[192];
    const int written = std::vsnprintf(message, sizeof(message), fmt ? fmt : "", args);

    // A message longer than the buffer is truncated rather than split; mark it
    // so a reader is never left guessing whether the line is complete.
    const bool truncated = (written < 0) || (static_cast<size_t>(written) >= sizeof(message));

    // Callers are inconsistent about a trailing newline; the prefix format adds
    // exactly one, so drop any the message brought with it.
    size_t length = std::strlen(message);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
        message[--length] = '\0';
    }

    esp_log_write(espLevel, effectiveTag, "%c (%" PRIu32 ") %s: %s%s\n",
                  levelChar, esp_log_timestamp(), effectiveTag,
                  message, truncated ? "…[truncated]" : "");
#else
    const char* levelStr = "INFO";
    FILE* out = stdout;
    switch (level) {
        case Level::Error:   levelStr = "ERROR"; out = stderr; break;
        case Level::Warn:    levelStr = "WARN"; break;
        case Level::Info:    levelStr = "INFO"; break;
        case Level::Debug:   levelStr = "DEBUG"; break;
        case Level::Verbose: levelStr = "VERBOSE"; break;
    }

    std::fputs("[", out);
    std::fputs(levelStr, out);
    std::fputs("]", out);
    if (tag && tag[0] != '\0') {
        std::fputs(" ", out);
        std::fputs(tag, out);
    }
    std::fputs(": ", out);
    std::vfprintf(out, fmt, args);
    std::fputc('\n', out);
    std::fflush(out);
#endif
}

Sink currentSink = &defaultSink;
Level minLevel = Level::Info;  // Default runtime level

// Per-module log levels. std::less<> lets a lookup take the tag as a
// string_view, so resolving a level never builds a std::string.
std::map<std::string, Level, std::less<>> g_moduleLevels;
platform::LocalMutex g_moduleLevelsMutex;
std::atomic<bool> g_hasModuleLevels{false};

// Every log call resolves its tag, twice with KNX_LOGx (isLevelEnabled, then
// write), so the result of the map lookup is kept per tag pointer. Tags are
// static strings, as with ESP_LOGx; a tag with the same text at another
// address simply gets its own entry. The cache stores whether the tag has an
// override rather than the global level, so setLevel() needs no invalidation;
// every change to the module levels clears it. Guarded by g_moduleLevelsMutex.
struct TagCacheEntry {
    const char* tag = nullptr;
    bool hasOverride = false;
    Level level = Level::Info;
};
constexpr size_t kTagCacheSize = 8;
std::array<TagCacheEntry, kTagCacheSize> g_tagCache{};
size_t g_tagCacheNext = 0;

void clearTagCacheLocked() {
    g_tagCache.fill(TagCacheEntry{});
    g_tagCacheNext = 0;
}

Level resolveEffectiveLevel(const char* tag) {
    if (!tag || tag[0] == '\0') {
        return minLevel;
    }

    if (!g_hasModuleLevels.load(std::memory_order_acquire)) {
        return minLevel;
    }

    platform::LocalLock<platform::LocalMutex> lock(g_moduleLevelsMutex);
    for (const TagCacheEntry& entry : g_tagCache) {
        if (entry.tag == tag) {
            return entry.hasOverride ? entry.level : minLevel;
        }
    }

    auto it = g_moduleLevels.find(std::string_view(tag));
    TagCacheEntry& slot = g_tagCache[g_tagCacheNext];
    g_tagCacheNext = (g_tagCacheNext + 1) % kTagCacheSize;
    slot.tag = tag;
    slot.hasOverride = (it != g_moduleLevels.end());
    slot.level = slot.hasOverride ? it->second : Level::Info;
    return slot.hasOverride ? slot.level : minLevel;
}

} // namespace

void setSink(Sink sink) {
    currentSink = sink ? sink : &defaultSink;
}

void setLevel(Level level) {
    minLevel = level;
}

Level getLevel() {
    return minLevel;
}

void setModuleLevel(const char* tag, Level level) {
    if (!tag || tag[0] == '\0') return;  // Ignore null or empty tags
    platform::LocalLock<platform::LocalMutex> lock(g_moduleLevelsMutex);
    g_moduleLevels[tag] = level;
    clearTagCacheLocked();
    g_hasModuleLevels.store(true, std::memory_order_release);
}

Level getModuleLevel(const char* tag) {
    return resolveEffectiveLevel(tag);
}

void clearModuleLevel(const char* tag) {
    if (!tag || tag[0] == '\0') return;  // Ignore null or empty tags
    platform::LocalLock<platform::LocalMutex> lock(g_moduleLevelsMutex);
    auto it = g_moduleLevels.find(std::string_view(tag));
    if (it != g_moduleLevels.end()) g_moduleLevels.erase(it);
    clearTagCacheLocked();
    g_hasModuleLevels.store(!g_moduleLevels.empty(), std::memory_order_release);
}

void clearAllModuleLevels() {
    platform::LocalLock<platform::LocalMutex> lock(g_moduleLevelsMutex);
    g_moduleLevels.clear();
    clearTagCacheLocked();
    g_hasModuleLevels.store(false, std::memory_order_release);
}

bool isLevelEnabled(Level level, const char* tag) {
    return level <= resolveEffectiveLevel(tag);
}

void writeVa(Level level, const char* tag, const char* fmt, va_list args) {
    // Check module-specific level first, fall back to global
    Level effectiveLevel = resolveEffectiveLevel(tag);
    
    // Runtime level filtering
    if (level > effectiveLevel) {
        return;
    }
    currentSink(level, tag, fmt, args);
}

void write(Level level, const char* tag, const char* fmt, ...) {
    // Check module-specific level first, fall back to global
    Level effectiveLevel = resolveEffectiveLevel(tag);
    
    // Early return for runtime filtering before va_start
    if (level > effectiveLevel) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    currentSink(level, tag, fmt, args);
    va_end(args);
}

} // namespace log
} // namespace knx
