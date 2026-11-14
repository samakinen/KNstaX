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
// Task Configuration
// ============================================================================

#ifdef CONFIG_KNX_RX_TASK_STACK_SIZE
    constexpr uint32_t RX_TASK_STACK_SIZE = CONFIG_KNX_RX_TASK_STACK_SIZE;
#else
    constexpr uint32_t RX_TASK_STACK_SIZE = 3072;
#endif

// ============================================================================
// Buffer Sizes
// ============================================================================

constexpr size_t MAX_APDU_LENGTH = 254;        // Maximum APDU length

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
 * @brief Seconds a management client should wait after a master reset.
 *
 * Reported in the Process_Time field of A_Restart_Response (03/03/07 §3.4.2.2,
 * 2 octets, DPT_TimePeriodSec). ETS waits this long before it touches the
 * device again; 0 declares "back in under 5 s" (03/05/02 §3.7.1.2.2), which is
 * the right answer for a bus-powered TP1 device that only reboots its own
 * firmware.
 *
 * A device that has to bring a network link back up needs longer and must say
 * so. A KNXnet/IP device on Wi-Fi takes seconds to associate and get an
 * address before it can answer anything, and understating that is not a slow
 * download but a failed one: ETS starts polling immediately, exhausts its
 * retries while the device is still booting, and reports the download as
 * failed. Overstating it only costs the client some patience, once per
 * download, so err upwards.
 */
#ifdef CONFIG_KNX_RESTART_PROCESS_TIME_S
constexpr uint16_t RESTART_PROCESS_TIME_S = CONFIG_KNX_RESTART_PROCESS_TIME_S;
#else
constexpr uint16_t RESTART_PROCESS_TIME_S = 0;
#endif

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

// KNX_FEATURE_COUPLER gates the coupler's layer-2 group acknowledge in the TP1
// receive path (physical::GroupAckFilter): the hook in the receive ISR and the
// plumbing to it. An end device never needs it, and the ISR is IRAM. Set by
// the build system (CONFIG_KNX_ENABLE_COUPLER on ESP-IDF, the
// KNX_FEATURE_COUPLER CMake option on a host build); the fallback follows the
// same rule as the media flags.
#ifndef KNX_FEATURE_COUPLER
#  if defined(CONFIG_KNX_ENABLE_COUPLER)
#    define KNX_FEATURE_COUPLER 1
#  elif defined(ESP_PLATFORM)
#    define KNX_FEATURE_COUPLER 0
#  else
#    define KNX_FEATURE_COUPLER 1
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

// KNX_IP_TUNNELING gates the KNXnet/IP *client* role: the tunnelling client
// (IpTunnelingPhysical, TunnelingSessionClient), gateway discovery and the
// device-management connection. A KNX IP device (03/02/06) is never a
// tunnelling client, so an image that is only commissioned by ETS builds
// without it and links none of that code; the public entry points that would
// construct a client are absent, not stubbed. Set by the build system from
// CONFIG_KNX_ENABLE_TUNNELING (ESP-IDF) or KNX_FEATURE_TUNNELING (host); the
// fallback follows the same "everything available" contract as above.
#ifndef KNX_IP_TUNNELING
#  if defined(CONFIG_KNX_ENABLE_TUNNELING)
#    define KNX_IP_TUNNELING KNX_FEATURE_NETIP
#  elif defined(CONFIG_KNX_MEDIUM_IP)
#    define KNX_IP_TUNNELING 0
#  else
#    define KNX_IP_TUNNELING KNX_FEATURE_NETIP
#  endif
#endif

// KNX_SECURE_IP_TUNNELING gates the IP Secure *session* machinery — ECDH key
// agreement, PBKDF2, the SESSION_REQUEST handshake and the secure tunnelling
// client — separately from the rest of KNX Secure. It implies
// KNX_IP_TUNNELING.
//
// It has to be separable because the two IP Secure features need different
// amounts of cryptography. Secure Routing is AES-CCM under a group key the
// project downloads, and the AES already present for Data Secure covers it. A
// secure tunnelling session additionally needs X25519 and PBKDF2, which on
// ESP-IDF means legacy mbedTLS headers that mbedTLS 4 no longer ships. A KNX
// IP device does not open tunnelling sessions — it is not a client — so
// compiling that machinery in would cost a device a build failure for a
// feature it cannot use.
#ifndef KNX_SECURE_IP_TUNNELING
#  define KNX_SECURE_IP_TUNNELING (KNX_SECURE_ENABLED && KNX_IP_TUNNELING)
#endif

namespace feature {
/// True when the KNXnet/IP medium is compiled into this build.
constexpr bool NETIP = (KNX_FEATURE_NETIP != 0);
/// True when the TP1 medium is compiled into this build.
constexpr bool TP1 = (KNX_FEATURE_TP1 != 0);
/// True when the KNXnet/IP client role (tunnelling) is compiled into this build.
constexpr bool IP_TUNNELING = (KNX_IP_TUNNELING != 0);
} // namespace feature

} // namespace config
} // namespace knx
