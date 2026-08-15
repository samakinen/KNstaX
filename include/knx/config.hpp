// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file config.hpp
 * @brief KNX Stack Configuration
 * 
 * This file contains compile-time configuration for the KNX stack.
 * Values can be overridden via Kconfig or build definitions.
 */

#pragma once

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#include <cstddef>
#include <cstdint>

namespace knx {
namespace config {

// ============================================================================
// Medium Configuration
// ============================================================================

#if defined(CONFIG_KNX_MEDIUM_TP1)
    constexpr bool MEDIUM_TP1 = true;
    constexpr bool MEDIUM_IP = false;
    constexpr bool MEDIUM_RF = false;
#elif defined(CONFIG_KNX_MEDIUM_IP)
    constexpr bool MEDIUM_TP1 = false;
    constexpr bool MEDIUM_IP = true;
    constexpr bool MEDIUM_RF = false;
#elif defined(CONFIG_KNX_MEDIUM_RF)
    constexpr bool MEDIUM_TP1 = false;
    constexpr bool MEDIUM_IP = false;
    constexpr bool MEDIUM_RF = true;
#else
    // Default to TP1 if not specified
    constexpr bool MEDIUM_TP1 = true;
    constexpr bool MEDIUM_IP = false;
    constexpr bool MEDIUM_RF = false;
#endif

// ============================================================================
// Security Configuration
// ============================================================================

#ifdef CONFIG_KNX_ENABLE_SECURITY
    constexpr bool ENABLE_SECURITY = CONFIG_KNX_ENABLE_SECURITY;
#else
    constexpr bool ENABLE_SECURITY = false;
#endif

// ============================================================================
// Task Configuration
// ============================================================================

#ifdef CONFIG_KNX_TASK_STACK_SIZE
    constexpr uint32_t TASK_STACK_SIZE = CONFIG_KNX_TASK_STACK_SIZE;
#else
    constexpr uint32_t TASK_STACK_SIZE = 4096;
#endif

#ifdef CONFIG_KNX_TASK_PRIORITY
    constexpr uint32_t TASK_PRIORITY = CONFIG_KNX_TASK_PRIORITY;
#else
    constexpr uint32_t TASK_PRIORITY = 5;
#endif

#ifdef CONFIG_KNX_RX_TASK_STACK_SIZE
    constexpr uint32_t RX_TASK_STACK_SIZE = CONFIG_KNX_RX_TASK_STACK_SIZE;
#else
    constexpr uint32_t RX_TASK_STACK_SIZE = 3072;
#endif

#ifdef CONFIG_KNX_RX_TASK_PRIORITY
    constexpr uint32_t RX_TASK_PRIORITY = CONFIG_KNX_RX_TASK_PRIORITY;
#else
    constexpr uint32_t RX_TASK_PRIORITY = 10;  // Higher priority for RX
#endif

// ============================================================================
// Buffer Sizes
// ============================================================================

constexpr size_t MAX_KNX_TELEGRAM_SIZE = 263;  // Max APDU size + overhead
constexpr size_t MAX_CEMI_FRAME_SIZE = 64;     // Typical CEMI frame
constexpr size_t MAX_APDU_LENGTH = 254;        // Maximum APDU length
constexpr size_t MAX_TPDU_LENGTH = 255;        // Maximum TPDU length

/**
 * @brief Payload capacity reserved per group object, in octets.
 *
 * Deliberately far below MAX_APDU_LENGTH.  Each group object holds two buffers
 * of this size (the current value and the last transmitted value, for
 * send-on-change), so the cost is paid twice per object and a device with
 * dozens of objects pays it dozens of times.  Sizing them for a 254-octet
 * extended APDU spends ~500 bytes of RAM each to carry payloads that are
 * almost always one or two bytes.
 *
 * 16 covers every datapoint type in the catalog — the widest is DPT 16 at 14
 * octets — and group_object.hpp static_asserts that against the catalog, so a
 * future wide DPT cannot silently outgrow it.
 *
 * Raise this only for a product that uses group objects as opaque transport
 * for larger blobs; oversized payloads are rejected with BufferTooSmall, never
 * truncated.
 */
#ifdef CONFIG_KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES
constexpr size_t MAX_GROUP_OBJECT_PAYLOAD_BYTES = CONFIG_KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES;
#else
constexpr size_t MAX_GROUP_OBJECT_PAYLOAD_BYTES = 16;
#endif
static_assert(MAX_GROUP_OBJECT_PAYLOAD_BYTES <= MAX_APDU_LENGTH,
              "A group object payload cannot exceed the maximum APDU");

/**
 * @brief Live KNX Data Secure sessions held at once, one per secured group address.
 *
 * Each session is roughly 48 bytes (key, sequence counters, replay window), so
 * the default costs about 1.5 KB and only in a `KNX_SECURE_ENABLED` build.
 *
 * A device that secures more group addresses than this will reject telegrams
 * for the excess ones rather than evicting a live session — dropping a session
 * would discard its replay window. Size this at or above the number of secured
 * group addresses the product declares.
 */
#ifdef CONFIG_KNX_MAX_DATA_SECURE_SESSIONS
constexpr size_t MAX_DATA_SECURE_SESSIONS = CONFIG_KNX_MAX_DATA_SECURE_SESSIONS;
#else
constexpr size_t MAX_DATA_SECURE_SESSIONS = 32;
#endif

#ifdef CONFIG_KNX_RX_QUEUE_SIZE
    constexpr size_t RX_QUEUE_SIZE = CONFIG_KNX_RX_QUEUE_SIZE;
#else
    constexpr size_t RX_QUEUE_SIZE = 10;
#endif

#ifdef CONFIG_KNX_TX_QUEUE_SIZE
    constexpr size_t TX_QUEUE_SIZE = CONFIG_KNX_TX_QUEUE_SIZE;
#else
    constexpr size_t TX_QUEUE_SIZE = 5;
#endif

// ============================================================================
// Timing Configuration (microseconds)
// ============================================================================

constexpr uint32_t TP1_BIT_TIME_US = 104;
constexpr uint32_t TP1_CHAR_TIME_US = TP1_BIT_TIME_US * 11;  // Start + 8 data + parity + stop
constexpr uint32_t TP1_ACK_TIMEOUT_US = TP1_BIT_TIME_US * 35;

constexpr uint32_t IP_ROUTING_INDICATION_INTERVAL_MS = 100;

#ifdef CONFIG_KNX_IP_PORT
    constexpr uint16_t IP_PORT = CONFIG_KNX_IP_PORT;
#else
    constexpr uint16_t IP_PORT = 3671;
#endif

#ifdef CONFIG_KNX_NETIP_UDP_BUFFER_SIZE
    constexpr size_t NETIP_UDP_BUFFER_SIZE = CONFIG_KNX_NETIP_UDP_BUFFER_SIZE;
#else
    constexpr size_t NETIP_UDP_BUFFER_SIZE = 1500;
#endif

#ifdef CONFIG_KNX_NETIP_DEVICE_MANAGEMENT_BUFFER_SIZE
    constexpr size_t NETIP_DEVICE_MANAGEMENT_BUFFER_SIZE = CONFIG_KNX_NETIP_DEVICE_MANAGEMENT_BUFFER_SIZE;
#else
    constexpr size_t NETIP_DEVICE_MANAGEMENT_BUFFER_SIZE = 2048;
#endif

#ifdef CONFIG_KNX_NETIP_TCP_BUFFER_SIZE
    constexpr size_t NETIP_TCP_BUFFER_SIZE = CONFIG_KNX_NETIP_TCP_BUFFER_SIZE;
#else
    constexpr size_t NETIP_TCP_BUFFER_SIZE = 4096;
#endif

#ifdef CONFIG_KNX_NETIP_TUNNELING_KEEPALIVE_INTERVAL_MS
    constexpr uint32_t NETIP_TUNNELING_KEEPALIVE_INTERVAL_MS = CONFIG_KNX_NETIP_TUNNELING_KEEPALIVE_INTERVAL_MS;
#else
    constexpr uint32_t NETIP_TUNNELING_KEEPALIVE_INTERVAL_MS = 60000;
#endif

// ============================================================================
// Memory Configuration
// ============================================================================

#ifdef CONFIG_KNX_FLASH_SIZE
    constexpr size_t FLASH_SIZE = CONFIG_KNX_FLASH_SIZE;
#else
    constexpr size_t FLASH_SIZE = 4096;  // 4KB default
#endif

#ifdef CONFIG_KNX_FLASH_PAGE_SIZE
    constexpr size_t FLASH_PAGE_SIZE = CONFIG_KNX_FLASH_PAGE_SIZE;
#else
    constexpr size_t FLASH_PAGE_SIZE = 256;
#endif

// ============================================================================
// Debug Configuration
// ============================================================================

#ifdef CONFIG_KNX_DEBUG_ENABLED
    constexpr bool DEBUG_ENABLED = CONFIG_KNX_DEBUG_ENABLED;
#else
    constexpr bool DEBUG_ENABLED = false;
#endif

#ifdef CONFIG_KNX_VERBOSE_LOGGING
    constexpr bool VERBOSE_LOGGING = CONFIG_KNX_VERBOSE_LOGGING;
#else
    constexpr bool VERBOSE_LOGGING = false;
#endif

// ============================================================================
// Feature Flags
// ============================================================================

// KNX_FEATURE_NETIP / KNX_FEATURE_TP1 are set by the build system (Kconfig in
// ESP-IDF mode, CMake options in standalone mode) and gate whole media out of
// the build, headers included.  The fallbacks below only apply to consumers
// that compile a translation unit without the KNstaX build system, and they
// deliberately default to "everything available" so such a build still works.
#ifndef KNX_FEATURE_NETIP
#  if defined(CONFIG_KNX_MEDIUM_IP)
#    define KNX_FEATURE_NETIP 1
#  elif defined(CONFIG_KNX_MEDIUM_TP1)
#    define KNX_FEATURE_NETIP 0
#  else
#    define KNX_FEATURE_NETIP 1
#  endif
#endif

#ifndef KNX_FEATURE_TP1
#  if defined(CONFIG_KNX_MEDIUM_IP) && !defined(CONFIG_KNX_MEDIUM_TP1)
#    define KNX_FEATURE_TP1 0
#  else
#    define KNX_FEATURE_TP1 1
#  endif
#endif

// KNX_SECURE_ENABLED gates the whole KNX Secure surface (Data Secure, IP
// Secure) and is used in `#if` guards, so an undefined macro silently disables
// security rather than failing the build.  Same fallback contract as the media
// flags above: honour the Kconfig symbol when the build system did not set it.
#ifndef KNX_SECURE_ENABLED
#  if defined(CONFIG_KNX_ENABLE_SECURITY)
#    define KNX_SECURE_ENABLED 1
#  else
#    define KNX_SECURE_ENABLED 0
#  endif
#endif

namespace feature {
/// True when the KNXnet/IP medium is compiled into this build.
constexpr bool NETIP = (KNX_FEATURE_NETIP != 0);
/// True when the TP1 medium is compiled into this build.
constexpr bool TP1 = (KNX_FEATURE_TP1 != 0);
} // namespace feature

#ifdef CONFIG_KNX_ENABLE_ROUTING
    constexpr bool ENABLE_ROUTING = CONFIG_KNX_ENABLE_ROUTING;
#else
    constexpr bool ENABLE_ROUTING = false;
#endif

#ifdef CONFIG_KNX_ENABLE_TUNNELING
    constexpr bool ENABLE_TUNNELING = CONFIG_KNX_ENABLE_TUNNELING;
#else
    constexpr bool ENABLE_TUNNELING = false;
#endif

#ifdef CONFIG_KNX_ENABLE_GROUP_ADDRESS_FILTER
    constexpr bool ENABLE_GROUP_ADDRESS_FILTER = CONFIG_KNX_ENABLE_GROUP_ADDRESS_FILTER;
#else
    constexpr bool ENABLE_GROUP_ADDRESS_FILTER = true;
#endif

} // namespace config
} // namespace knx
