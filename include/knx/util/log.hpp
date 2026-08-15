// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "knx/types.hpp"

// Compile-time log level configuration
// Define KNX_LOG_LEVEL_COMPILE to one of these values to disable logs at compile time.
// On ESP-IDF builds the Kconfig option KNX_LOG_LEVEL (menuconfig) provides the value.
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#ifndef KNX_LOG_LEVEL_COMPILE
    #if defined(CONFIG_KNX_LOG_LEVEL)
        #define KNX_LOG_LEVEL_COMPILE CONFIG_KNX_LOG_LEVEL
    #else
        #define KNX_LOG_LEVEL_COMPILE 5  // Default: all logs compiled in (runtime-filtered)
    #endif
#endif

#define KNX_LOG_LEVEL_NONE    0
#define KNX_LOG_LEVEL_ERROR   1
#define KNX_LOG_LEVEL_WARN    2
#define KNX_LOG_LEVEL_INFO    3
#define KNX_LOG_LEVEL_DEBUG   4
#define KNX_LOG_LEVEL_VERBOSE 5

namespace knx {
namespace log {

enum class Level {
    Error,
    Warn,
    Info,
    Debug,
    Verbose
};

using Sink = void(*)(Level level, const char* tag, const char* fmt, va_list args);

void setSink(Sink sink);

/**
 * @brief Set minimum runtime log level (global)
 * Logs below this level will be filtered out even if compiled in
 */
void setLevel(Level minLevel);

/**
 * @brief Get current minimum runtime log level (global)
 */
Level getLevel();

/**
 * @brief Set minimum log level for a specific module/tag
 * @param tag Module identifier (e.g., "KNX.BAU", "KNX.DataLink")
 * @param level Minimum level for this module
 */
void setModuleLevel(const char* tag, Level level);

/**
 * @brief Get log level for a specific module/tag
 * @param tag Module identifier
 * @return Module-specific level, or global level if not set
 */
Level getModuleLevel(const char* tag);

/**
 * @brief Clear module-specific log level (revert to global)
 * @param tag Module identifier
 */
void clearModuleLevel(const char* tag);

/**
 * @brief Clear all module-specific log levels
 */
void clearAllModuleLevels();

/**
 * @brief Check whether a message at @p level for @p tag would be emitted.
 *
 * Used by the KNX_LOGx macros to skip evaluating log arguments entirely when
 * the message is runtime-filtered — argument expressions may be expensive
 * (hex formatting, string building) and must not run on hot paths when the
 * level is disabled.
 */
bool isLevelEnabled(Level level, const char* tag);

void write(Level level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void writeVa(Level level, const char* tag, const char* fmt, va_list args);

inline const char* addressTypeName(AddressType type) {
    return (type == AddressType::Group) ? "Group" : "Individual";
}

inline std::string formatAddress(AddressType type, const GroupAddress& address) {
    char buffer[32];
    if (type == AddressType::Group) {
        std::snprintf(buffer, sizeof(buffer), "%u/%u/%u",
                      address.main(), address.middle(), address.sub());
    } else {
        const IndividualAddress individual(address.raw);
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u",
                      individual.area(), individual.line(), individual.device());
    }
    return std::string(buffer);
}

inline std::string formatAddress(const IndividualAddress& address) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u",
                  address.area(), address.line(), address.device());
    return std::string(buffer);
}

} // namespace log
} // namespace knx

// Compile-time log filtering macros.
//
// The runtime level check is done in the macro, BEFORE the argument list is
// evaluated: log arguments may be expensive (hex dumps, string formatting)
// and must not execute on hot paths when the message is filtered anyway.
#define KNX_LOG_WRITE_IF_ENABLED(level, TAG, fmt, ...)                          \
    do {                                                                        \
        if (::knx::log::isLevelEnabled(level, TAG)) {                           \
            ::knx::log::write(level, TAG, fmt, ##__VA_ARGS__);                  \
        }                                                                       \
    } while (0)

#if KNX_LOG_LEVEL_COMPILE >= KNX_LOG_LEVEL_ERROR
    #define KNX_LOGE(TAG, fmt, ...) KNX_LOG_WRITE_IF_ENABLED(::knx::log::Level::Error, TAG, fmt, ##__VA_ARGS__)
#else
    #define KNX_LOGE(TAG, fmt, ...) ((void)0)
#endif

#if KNX_LOG_LEVEL_COMPILE >= KNX_LOG_LEVEL_WARN
    #define KNX_LOGW(TAG, fmt, ...) KNX_LOG_WRITE_IF_ENABLED(::knx::log::Level::Warn, TAG, fmt, ##__VA_ARGS__)
#else
    #define KNX_LOGW(TAG, fmt, ...) ((void)0)
#endif

#if KNX_LOG_LEVEL_COMPILE >= KNX_LOG_LEVEL_INFO
    #define KNX_LOGI(TAG, fmt, ...) KNX_LOG_WRITE_IF_ENABLED(::knx::log::Level::Info, TAG, fmt, ##__VA_ARGS__)
#else
    #define KNX_LOGI(TAG, fmt, ...) ((void)0)
#endif

#if KNX_LOG_LEVEL_COMPILE >= KNX_LOG_LEVEL_DEBUG
    #define KNX_LOGD(TAG, fmt, ...) KNX_LOG_WRITE_IF_ENABLED(::knx::log::Level::Debug, TAG, fmt, ##__VA_ARGS__)
#else
    #define KNX_LOGD(TAG, fmt, ...) ((void)0)
#endif

#if KNX_LOG_LEVEL_COMPILE >= KNX_LOG_LEVEL_VERBOSE
    #define KNX_LOGV(TAG, fmt, ...) KNX_LOG_WRITE_IF_ENABLED(::knx::log::Level::Verbose, TAG, fmt, ##__VA_ARGS__)
#else
    #define KNX_LOGV(TAG, fmt, ...) ((void)0)
#endif

