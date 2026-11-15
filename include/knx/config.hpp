// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file config.hpp
 * @brief KNX Stack Configuration
 * 
 * This file contains compile-time configuration for the KNX stack.
 * The values come from Kconfig: through sdkconfig.h on ESP-IDF, through the
 * generated knx/autoconf.h in a standalone CMake build (knx_configure(), a
 * profile under configs/profiles/). The media and feature flags may also be
 * given as build definitions.
 *
 * A symbol that Kconfig defines without a dependency (the task stack, the group
 * object payload, the restart time) is present in both and has no fallback
 * here: a build without the configuration stops with #error rather than
 * silently taking a number. Symbols under a menu that depends on the medium
 * (the KNXnet/IP ones) are absent from an ESP-IDF TP1 image, and bools that are
 * n are absent everywhere, so those keep a fallback.
 */

#pragma once

#if __has_include("knx/autoconf.h")
#include "knx/autoconf.h"
#elif __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#include <cstddef>
#include <cstdint>

namespace knx {
namespace config {

// ============================================================================
// Task Configuration
// ============================================================================

// Whether the TP1 data link layer runs receive processing on its own task.
// A single-threaded (KNX_PLATFORM_BAREMETAL) build has no scheduler, so the
// main loop drives it through processRxAvailable() instead.
#if defined(KNX_PLATFORM_BAREMETAL)
    constexpr bool RX_TASK_ENABLED_DEFAULT = false;
#else
    constexpr bool RX_TASK_ENABLED_DEFAULT = true;
#endif

#ifdef CONFIG_KNX_RX_TASK_STACK_SIZE
    constexpr uint32_t RX_TASK_STACK_SIZE = CONFIG_KNX_RX_TASK_STACK_SIZE;
#else
#   error "CONFIG_KNX_RX_TASK_STACK_SIZE is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif

// ============================================================================
// Buffer Sizes
// ============================================================================

// ============================================================================
// Capacities
//
// Every capacity comes from Kconfig (menu "Capacities"), which defines all of
// them in every build, so none has a fallback here. A capacity is a promise: a
// full table or queue answers with an error, never an overwrite.
// ============================================================================

#ifndef CONFIG_KNX_MAX_APDU_LENGTH
#   error "CONFIG_KNX_MAX_APDU_LENGTH is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_TP1_RX_QUEUE_FRAMES
#   error "CONFIG_KNX_TP1_RX_QUEUE_FRAMES is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_TRANSPORT_RX_QUEUE
#   error "CONFIG_KNX_TRANSPORT_RX_QUEUE is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_ADDRESS_TABLE_ENTRIES
#   error "CONFIG_KNX_ADDRESS_TABLE_ENTRIES is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_ASSOCIATION_TABLE_ENTRIES
#   error "CONFIG_KNX_ASSOCIATION_TABLE_ENTRIES is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES
#   error "CONFIG_KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif
#ifndef CONFIG_KNX_SEND_PIPELINE_DEPTH
#   error "CONFIG_KNX_SEND_PIPELINE_DEPTH is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif

/// Largest APDU the device accepts, sends and reports in PID_MAX_APDU_LENGTH.
constexpr size_t MAX_APDU_LENGTH = CONFIG_KNX_MAX_APDU_LENGTH;
static_assert(MAX_APDU_LENGTH >= 15 && MAX_APDU_LENGTH <= 254,
              "KNX_MAX_APDU_LENGTH must be 15 (standard frames) to 254 (extended frames)");

/// True when the APDU limit allows standard frames only (4-bit length field).
constexpr bool STANDARD_FRAMES_ONLY = MAX_APDU_LENGTH <= 15;

/**
 * @brief Longest TP1 frame on the wire, FCS included, for MAX_APDU_LENGTH.
 *
 * 03/02/02 §2.2: a standard frame is CTRL (1) + source (2) + destination (2) +
 * address type/hop count/length (1) + TPDU (length + 1) + FCS (1) = length + 8
 * octets, 23 for the 15-octet maximum. An extended frame adds CTRLE (1) and
 * has an 8-bit length: length + 9 octets, 263 for 254.
 */
constexpr size_t MAX_TP1_FRAME_LENGTH = MAX_APDU_LENGTH + (STANDARD_FRAMES_ONLY ? 8u : 9u);

/// Largest TPDU (TPCI/APCI octets plus payload) a frame object holds: the two
/// header octets and MAX_APDU_LENGTH of payload.
constexpr size_t MAX_TPDU_LENGTH = 2u + MAX_APDU_LENGTH;

/// Frames the TP1 MAC layer queues between the medium and the data link layer.
constexpr size_t TP1_RX_QUEUE_FRAMES = CONFIG_KNX_TP1_RX_QUEUE_FRAMES;
static_assert(TP1_RX_QUEUE_FRAMES >= 2, "KNX_TP1_RX_QUEUE_FRAMES must be at least 2");
/// Frames the transport layer queues for processing.
constexpr size_t TRANSPORT_RX_QUEUE = CONFIG_KNX_TRANSPORT_RX_QUEUE;
static_assert(TRANSPORT_RX_QUEUE >= 1, "KNX_TRANSPORT_RX_QUEUE must be at least 1");
/// Group addresses in the address table and in the data link layer's list.
constexpr size_t ADDRESS_TABLE_ENTRIES = CONFIG_KNX_ADDRESS_TABLE_ENTRIES;
static_assert(ADDRESS_TABLE_ENTRIES >= 1, "KNX_ADDRESS_TABLE_ENTRIES must be at least 1");
/// Entries in the association table.
constexpr size_t ASSOCIATION_TABLE_ENTRIES = CONFIG_KNX_ASSOCIATION_TABLE_ENTRIES;
static_assert(ASSOCIATION_TABLE_ENTRIES >= 1, "KNX_ASSOCIATION_TABLE_ENTRIES must be at least 1");
/// Individual addresses the data link layer answers to besides its own (may be 0).
constexpr size_t ADDITIONAL_INDIVIDUAL_ADDRESSES = CONFIG_KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES;
/// Send outcomes the application layer holds for the application to collect.
constexpr size_t SEND_PIPELINE_DEPTH = CONFIG_KNX_SEND_PIPELINE_DEPTH;
static_assert(SEND_PIPELINE_DEPTH >= 1, "KNX_SEND_PIPELINE_DEPTH must be at least 1");

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
#   error "CONFIG_KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
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
#   error "CONFIG_KNX_RESTART_PROCESS_TIME_S is not defined: build with the KNstaX configuration (ESP-IDF Kconfig, or knx_configure() in CMake)"
#endif

// The KNXnet/IP settings below live under the IP-medium menu, so an ESP-IDF TP1
// image does not define them, and a standalone build leaves that menu hidden:
// they keep their defaults.
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
// ESP-IDF mode, the resolved profile in standalone mode) and gate whole media out of
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

// KNX_FEATURE_COUPLER gates the whole router role of a line or backbone
// coupler: the Filter Table, the Router Object, the two-port coupler and its
// routing policy, the product API that declares them, and the layer-2 group
// acknowledge in the TP1 receive path (physical::GroupAckFilter: the hook in
// the receive ISR and the plumbing to it). An end device never needs any of it,
// and the ISR is IRAM. Set by the build system (CONFIG_KNX_ENABLE_COUPLER on
// ESP-IDF, the KNX_FEATURE_COUPLER variable derived from
// CONFIG_KNX_ENABLE_COUPLER on a standalone build); the fallback follows the
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

// Optional features (Kconfig menu "Optional Features"). Each KNX_FEATURE_*
// below is 1 when its CONFIG_KNX_* symbol is y and 0 otherwise: Kconfig defines
// a bool that is y and leaves one that is n out, and this file stops with
// #error above when no configuration is present at all, so there is no
// fallback to get wrong. A feature that is 0 is absent: its translation units
// are not compiled and the declarations that need them are removed, so using
// one is a compile error rather than a link error. KNX_FEATURE_COUPLER (above)
// and KNX_SECURE_ENABLED (below) are the same kind of switch.
#if defined(CONFIG_KNX_ENABLE_FUNCTION_PROPERTIES)
#  define KNX_FEATURE_FUNCTION_PROPERTIES 1
#else
#  define KNX_FEATURE_FUNCTION_PROPERTIES 0
#endif
#if defined(CONFIG_KNX_ENABLE_PROPERTY_EXT)
#  define KNX_FEATURE_PROPERTY_EXT 1
#else
#  define KNX_FEATURE_PROPERTY_EXT 0
#endif
#if defined(CONFIG_KNX_ENABLE_MEMORY_EXT)
#  define KNX_FEATURE_MEMORY_EXT 1
#else
#  define KNX_FEATURE_MEMORY_EXT 0
#endif
#if defined(CONFIG_KNX_ENABLE_NETWORK_PARAMETER)
#  define KNX_FEATURE_NETWORK_PARAMETER 1
#else
#  define KNX_FEATURE_NETWORK_PARAMETER 0
#endif
#if defined(CONFIG_KNX_ENABLE_GO_DIAGNOSTICS)
#  define KNX_FEATURE_GO_DIAGNOSTICS 1
#else
#  define KNX_FEATURE_GO_DIAGNOSTICS 0
#endif
#if defined(CONFIG_KNX_DPT_FLOAT)
#  define KNX_FEATURE_DPT_FLOAT 1
#else
#  define KNX_FEATURE_DPT_FLOAT 0
#endif
#if defined(CONFIG_KNX_DPT_TIME)
#  define KNX_FEATURE_DPT_TIME 1
#else
#  define KNX_FEATURE_DPT_TIME 0
#endif
#if defined(CONFIG_KNX_DPT_STRING)
#  define KNX_FEATURE_DPT_STRING 1
#else
#  define KNX_FEATURE_DPT_STRING 0
#endif
#if defined(CONFIG_KNX_DPT_COLOUR)
#  define KNX_FEATURE_DPT_COLOUR 1
#else
#  define KNX_FEATURE_DPT_COLOUR 0
#endif
#if defined(CONFIG_KNX_DPT_ACCESS)
#  define KNX_FEATURE_DPT_ACCESS 1
#else
#  define KNX_FEATURE_DPT_ACCESS 0
#endif
#if defined(CONFIG_KNX_ENABLE_DPT_CATALOGUE)
#  define KNX_FEATURE_DPT_CATALOGUE 1
#else
#  define KNX_FEATURE_DPT_CATALOGUE 0
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
// CONFIG_KNX_ENABLE_TUNNELING (both ESP-IDF and standalone); the
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
