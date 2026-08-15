// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file constants.hpp
 * @brief KNX protocol constants and magic number definitions
 * 
 * This file centralizes all magic numbers, timeout values, scaling factors,
 * and protocol constants used throughout the KNstaX stack. Using named
 * constants improves code readability and maintainability.
 */

#pragma once

#include "knx/netip/netip_config.hpp"

#include <cstdint>
#include <cstddef>

#include "knx/types.hpp"

namespace knx {
namespace constants {

// ============================================================================
// Protocol Constants
// ============================================================================

namespace protocol {
    // Frame sizes
    constexpr size_t MIN_FRAME_SIZE = 6;
    constexpr size_t MAX_FRAME_SIZE = 263;
    constexpr size_t STANDARD_FRAME_SIZE = 23;
    constexpr size_t EXTENDED_FRAME_SIZE = 263;
    
    // Frame offsets
    constexpr uint8_t OFFSET_CTRL_FIELD = 0;
    constexpr uint8_t OFFSET_SOURCE_ADDR = 1;
    constexpr uint8_t OFFSET_DEST_ADDR = 3;
    constexpr uint8_t OFFSET_LENGTH = 5;
    constexpr uint8_t OFFSET_TPCI = 6;
    constexpr uint8_t OFFSET_APCI = 6;
    constexpr uint8_t OFFSET_DATA = 7;
    
    // Control field masks
    constexpr uint8_t CTRL_FRAME_TYPE_MASK = 0x80;
    constexpr uint8_t CTRL_REPEAT_MASK = 0x20;
    constexpr uint8_t CTRL_BROADCAST_MASK = 0x10;
    constexpr uint8_t CTRL_PRIORITY_MASK = 0x0C;
    constexpr uint8_t CTRL_ACK_MASK = 0x02;
    constexpr uint8_t CTRL_CONFIRM_MASK = 0x01;
    
    // Priority values
    constexpr uint8_t PRIORITY_SYSTEM = 0x00;
    constexpr uint8_t PRIORITY_NORMAL = 0x04;
    constexpr uint8_t PRIORITY_URGENT = 0x08;
    constexpr uint8_t PRIORITY_LOW = 0x0C;
    
    // TPCI masks and values
    constexpr uint8_t TPCI_MASK = 0xFC;
    constexpr uint8_t TPCI_UCD = 0x00;          // Unnumbered Control Data
    constexpr uint8_t TPCI_NCD = 0x40;          // Numbered Control Data
    constexpr uint8_t TPCI_UDT = 0x80;          // Unnumbered Data
    constexpr uint8_t TPCI_NDT = 0xC0;          // Numbered Data
    
    // APCI masks
    constexpr uint16_t APCI_MASK = 0x03C0;
    constexpr uint8_t APCI_DATA_MASK = 0x3F;
    
    // Address type bit in the routing/length octet
    constexpr uint8_t DEST_ADDR_TYPE_MASK = 0x80;  // 0=individual, 1=group
}

// ============================================================================
// Timing Constants
// ============================================================================

namespace timing {
    // ── Connection-oriented transport (KNX 03/03/04 §5.5) ────────────────────
    //
    // These three are normative and must not be tuned:
    //   connection_timeout_timer      6 s  — breaks down an idle connection
    //   acknowledgment_timeout_timer  3 s  — waits for T_ACK
    //   repetitions                   3    — before giving up and disconnecting
    //
    // CONNECTION_TIMEOUT_MS previously said 10 s, which is why call sites wrote
    // the literal 6000 instead of using it. Corrected here; the literals are
    // gone.
    constexpr uint32_t CONNECTION_TIMEOUT_MS = 6000;        // 03/03/04 §5.5.1
    constexpr uint32_t ACK_TIMEOUT_MS = 3000;               // 03/03/04 §5.5.1
    constexpr uint8_t  CONNECTION_MAX_REPETITIONS = 3;      // 03/03/04 §5.4.3

    constexpr uint32_t CONNECT_RESPONSE_TIMEOUT_MS = 3000;  // 3 seconds
    constexpr uint32_t DISCONNECT_TIMEOUT_MS = 3000;        // 3 seconds

    // Retransmission
    constexpr uint32_t RETRANSMIT_TIMEOUT_MS = ACK_TIMEOUT_MS;
    constexpr uint8_t MAX_RETRANSMIT_COUNT = CONNECTION_MAX_REPETITIONS;
    constexpr uint32_t RETRANSMIT_BACKOFF_MS = 500;         // 500 milliseconds
    
    // Physical layer timing
    constexpr uint32_t BIT_TIME_US = 104;                   // 9600 baud (~104 µs per bit)
    constexpr uint32_t BYTE_TIME_US = 1040;                 // ~1ms per byte
    constexpr uint32_t INTERFRAME_GAP_US = 2000;            // 2ms between frames
    
    // TP1 specific timeouts
    constexpr uint32_t TP1_CHAR_TIMEOUT_MS = 50;            // Character timeout
    constexpr uint32_t TP1_BYTE_TIMEOUT_MS = 100;           // Byte timeout
    constexpr uint32_t TP1_ACK_TIMEOUT_MS = 15;             // ACK timeout (15ms)
    
    // KNXnet/IP Tunneling
    constexpr uint32_t TUNNELING_REQUEST_TIMEOUT_MS = 1000; // 1 second
    constexpr uint32_t TUNNELING_HEARTBEAT_MS = 60000;      // 1 minute
    constexpr uint32_t TUNNELING_CONNECTION_TIMEOUT_MS = 120000; // 2 minutes
    
    // ETS load state
    constexpr uint32_t LOAD_STATE_TIMEOUT_MS = 5000;        // 5 seconds
    constexpr uint32_t LOAD_VERIFY_DELAY_MS = 100;          // 100 milliseconds
    
    // Generic wait times
    constexpr uint32_t DEFAULT_POLL_INTERVAL_MS = 10;       // 10ms polling interval
    constexpr uint32_t TASK_DELAY_MS = 1;                   // 1ms task delay
}

// ============================================================================
// DPT Constants (Datapoint Types)
// ============================================================================

namespace dpt {
    // DPT 1 (Boolean - 1 bit)
    constexpr uint8_t DPT1_SIZE = 1;                        // 1 bit
    constexpr uint8_t DPT1_FALSE = 0x00;
    constexpr uint8_t DPT1_TRUE = 0x01;
    constexpr uint8_t DPT1_MASK = 0x01;
    
    // DPT 2 (1 bit controlled)
    constexpr uint8_t DPT2_SIZE = 2;                        // 2 bits
    constexpr uint8_t DPT2_CONTROL_MASK = 0x02;
    constexpr uint8_t DPT2_VALUE_MASK = 0x01;
    
    // DPT 3 (3 bit controlled)
    constexpr uint8_t DPT3_SIZE = 4;                        // 4 bits
    constexpr uint8_t DPT3_CONTROL_MASK = 0x08;
    constexpr uint8_t DPT3_STEPCODE_MASK = 0x07;
    
    // DPT 5 (Unsigned 8-bit - 0..255)
    constexpr uint8_t DPT5_SIZE = 8;                        // 1 byte
    constexpr uint8_t DPT5_MIN = 0;
    constexpr uint8_t DPT5_MAX = 255;
    constexpr float DPT5_PERCENT_SCALE = 100.0f / 255.0f;   // Scaling for percentage
    constexpr float DPT5_ANGLE_SCALE = 360.0f / 255.0f;     // Scaling for angle
    
    // DPT 6 (Signed 8-bit - -128..127)
    constexpr uint8_t DPT6_SIZE = 8;                        // 1 byte
    constexpr int8_t DPT6_MIN = -128;
    constexpr int8_t DPT6_MAX = 127;
    
    // DPT 7 (Unsigned 16-bit - 0..65535)
    constexpr uint8_t DPT7_SIZE = 16;                       // 2 bytes
    constexpr uint16_t DPT7_MIN = 0;
    constexpr uint16_t DPT7_MAX = 65535;
    
    // DPT 8 (Signed 16-bit - -32768..32767)
    constexpr uint8_t DPT8_SIZE = 16;                       // 2 bytes
    constexpr int16_t DPT8_MIN = -32768;
    constexpr int16_t DPT8_MAX = 32767;
    
    // DPT 9 (2-byte float - 16-bit floating point)
    // Layout: S EEEE MMMMMMMMMMM, where the 11-bit mantissa is signed.
    constexpr uint8_t DPT9_SIZE = 16;                       // 2 bytes
    constexpr float DPT9_SCALE_FACTOR = 0.01f;              // Mantissa scaling
    constexpr int16_t DPT9_MIN_MANTISSA = -2048;            // 11-bit signed mantissa minimum
    constexpr int16_t DPT9_MAX_MANTISSA = 2047;             // 11-bit signed mantissa maximum
    constexpr uint8_t DPT9_MANTISSA_BITS = 11;
    constexpr uint8_t DPT9_EXPONENT_BITS = 4;
    constexpr uint8_t DPT9_SIGN_BIT = 15;
    constexpr uint8_t DPT9_MAX_EXPONENT = 15;
    constexpr uint16_t DPT9_MANTISSA_MASK = 0x07FF;
    constexpr uint16_t DPT9_EXPONENT_MASK = 0x7800;
    constexpr uint16_t DPT9_SIGN_MASK = 0x8000;
    constexpr float DPT9_MIN_VALUE = -671088.64f;           // -2048 * 2^15 * 0.01
    constexpr float DPT9_MAX_VALUE = 670760.96f;            // +2047 * 2^15 * 0.01
    
    // DPT 10 (Time - 3 bytes: day, hour, minute, second)
    constexpr uint8_t DPT10_SIZE = 24;                      // 3 bytes
    constexpr uint8_t DPT10_DAY_MASK = 0xE0;
    constexpr uint8_t DPT10_HOUR_MASK = 0x1F;
    constexpr uint8_t DPT10_MINUTE_MASK = 0x3F;
    constexpr uint8_t DPT10_SECOND_MASK = 0x3F;
    constexpr uint8_t DPT10_MAX_DAY = 7;                    // 0=no day, 1-7=Mon-Sun
    constexpr uint8_t DPT10_MAX_HOUR = 23;
    constexpr uint8_t DPT10_MAX_MINUTE = 59;
    constexpr uint8_t DPT10_MAX_SECOND = 59;
    
    // DPT 11 (Date - 3 bytes: day, month, year)
    constexpr uint8_t DPT11_SIZE = 24;                      // 3 bytes
    constexpr uint8_t DPT11_DAY_MASK = 0x1F;
    constexpr uint8_t DPT11_MONTH_MASK = 0x0F;
    constexpr uint8_t DPT11_YEAR_MASK = 0x7F;
    constexpr uint8_t DPT11_MIN_DAY = 1;
    constexpr uint8_t DPT11_MAX_DAY = 31;
    constexpr uint8_t DPT11_MIN_MONTH = 1;
    constexpr uint8_t DPT11_MAX_MONTH = 12;
    constexpr uint8_t DPT11_MIN_YEAR = 0;                   // 1990 + year
    constexpr uint8_t DPT11_MAX_YEAR = 99;                  // 1990-2089
    constexpr uint16_t DPT11_BASE_YEAR = 1990;
    
    // DPT 12 (Unsigned 32-bit - 0..4294967295)
    constexpr uint8_t DPT12_SIZE = 32;                      // 4 bytes
    constexpr uint32_t DPT12_MIN = 0;
    constexpr uint32_t DPT12_MAX = 4294967295;
    
    // DPT 13 (Signed 32-bit - -2147483648..2147483647)
    constexpr uint8_t DPT13_SIZE = 32;                      // 4 bytes
    constexpr int32_t DPT13_MIN = -2147483648;
    constexpr int32_t DPT13_MAX = 2147483647;
    
    // DPT 14 (4-byte float - IEEE 754 single precision)
    constexpr uint8_t DPT14_SIZE = 32;                      // 4 bytes
    constexpr float DPT14_MIN = -3.40282347e38f;
    constexpr float DPT14_MAX = 3.40282347e38f;
    
    // DPT 16 (String - ASCII, 14 characters)
    constexpr uint8_t DPT16_SIZE = 112;                     // 14 bytes
    constexpr uint8_t DPT16_MAX_LENGTH = 14;
    constexpr char DPT16_NULL_CHAR = '\0';
    
    // DPT 232 (RGB - 3 bytes: R, G, B)
    constexpr uint8_t DPT232_SIZE = 24;                     // 3 bytes
    constexpr uint8_t DPT232_MIN_VALUE = 0;
    constexpr uint8_t DPT232_MAX_VALUE = 255;
}

// ============================================================================
// Security Constants (KNX Data Security)
// ============================================================================

namespace security {
    // AES-128 CCM parameters
    constexpr size_t AES128_KEY_SIZE = 16;                  // 128 bits
    constexpr size_t AES128_BLOCK_SIZE = 16;                // 128 bits
    constexpr size_t CCM_MAC_SIZE = 4;                      // 32-bit MAC
    constexpr size_t CCM_NONCE_SIZE = 13;                   // 104 bits
    constexpr size_t CCM_AUTH_DATA_SIZE = 1;                // 1 byte additional data
    
    // Security algorithms
    constexpr uint8_t SECURITY_ALGORITHM_CCM = 0x01;
    
    // Session management
    constexpr uint32_t SESSION_TIMEOUT_MS = 3600000;        // 1 hour
    constexpr uint32_t SESSION_RENEWAL_MS = 3000000;        // 50 minutes (renew before timeout)
    
    // Sequence numbers
    constexpr uint64_t MAX_SEQUENCE_NUMBER = 0xFFFFFFFFFFFFULL; // 48-bit sequence
    constexpr uint64_t SEQUENCE_WINDOW_SIZE = 64;           // Anti-replay window
    
    // Tool keys and access levels
    constexpr uint8_t TOOL_KEY_SIZE = 16;                   // 128-bit tool key
    constexpr uint8_t MAX_TOOL_ACCESS_LEVEL = 15;
}

// ============================================================================
// Memory Constants (Buffer sizes, table limits)
// ============================================================================

namespace memory {
    // Queue sizes (FreeRTOS queues)
    constexpr size_t DEFAULT_RX_QUEUE_SIZE = 16;            // Receive queue entries
    constexpr size_t DEFAULT_TX_QUEUE_SIZE = 16;            // Transmit queue entries
    constexpr size_t EVENT_QUEUE_SIZE = 32;                 // Event queue entries
    
    // Buffer sizes
    constexpr size_t FRAME_BUFFER_SIZE = 512;               // Frame processing buffer
    constexpr size_t CEMI_BUFFER_SIZE = 512;                // cEMI frame buffer
    constexpr size_t STRING_BUFFER_SIZE = 256;              // General string buffer
    constexpr size_t LOG_BUFFER_SIZE = 256;                 // Logging buffer
    
    // Connection limits
    constexpr size_t MAX_CONNECTIONS = 4;                   // Max concurrent connections

    // Memory sizes
    constexpr size_t DEFAULT_MEMORY_SIZE = 4096;            // 4KB default memory
    constexpr size_t MAX_MEMORY_SIZE = 65536;               // 64KB max addressable
}

// ============================================================================
// ETS Constants (ETS import/export)
// ============================================================================

namespace ets {
    // Load state values (PID_LOAD_STATE_CONTROL)
    constexpr uint8_t LOAD_STATE_UNLOADED = 0x00;           // Not loaded
    constexpr uint8_t LOAD_STATE_LOADING = 0x01;            // Loading in progress
    constexpr uint8_t LOAD_STATE_LOADED = 0x02;             // Successfully loaded
    constexpr uint8_t LOAD_STATE_ERROR = 0x03;              // Load error
    constexpr uint8_t LOAD_STATE_UNLOADING = 0x04;          // Unloading
    
    // Run state values (PID_RUN_STATE_CONTROL)
    constexpr uint8_t RUN_STATE_HALTED = 0x00;              // Application halted
    constexpr uint8_t RUN_STATE_RUNNING = 0x01;             // Application running
    constexpr uint8_t RUN_STATE_READY = 0x02;               // Ready to run
    constexpr uint8_t RUN_STATE_TERMINATED = 0x03;          // Terminated with error
    
    // File size limits
    constexpr size_t MAX_KNXPROD_SIZE = 1048576;            // 1MB max product file
    constexpr size_t MAX_PARAMETER_SIZE = 65536;            // 64KB max parameter data
    constexpr size_t MAX_PROJECT_SIZE = 10485760;           // 10MB max project file
    
    // Programming mode
    constexpr uint8_t PROGMODE_OFF = 0x00;
    constexpr uint8_t PROGMODE_ON = 0x01;
    
    // Property IDs (common ones)
    constexpr uint16_t PID_OBJECT_TYPE = 1;
    constexpr uint16_t PID_LOAD_STATE_CONTROL = 5;
    constexpr uint16_t PID_RUN_STATE_CONTROL = 6;
    constexpr uint16_t PID_SERIAL_NUMBER = 11;
    constexpr uint16_t PID_MANUFACTURER_ID = 12;
    constexpr uint16_t PID_DEVICE_CONTROL = 13;
    constexpr uint16_t PID_ORDER_INFO = 14;
    constexpr uint16_t PID_PROGMODE = 54;
}

// ============================================================================
// Network Constants
// ============================================================================

namespace network {
    // Hop count limits
    constexpr uint8_t DEFAULT_HOP_COUNT = 6;                // Default hop count
    constexpr uint8_t MAX_HOP_COUNT = 7;                    // Maximum hop count
    
    // Address ranges
    constexpr uint16_t MIN_INDIVIDUAL_ADDRESS = 0x0001;
    constexpr uint16_t MAX_INDIVIDUAL_ADDRESS = 0xFFFE;
    constexpr uint16_t BROADCAST_ADDRESS = 0x0000;
    
    constexpr uint16_t MIN_GROUP_ADDRESS = 0x0001;
    constexpr uint16_t MAX_GROUP_ADDRESS = 0xFFFF;
    
    // Area and line limits
    constexpr uint8_t MAX_AREA = 15;                        // Area: 0-15
    constexpr uint8_t MAX_LINE = 15;                        // Line: 0-15
    constexpr uint8_t MAX_DEVICE = 255;                     // Device: 0-255
    
    // Group address limits (3-level)
    constexpr uint8_t MAX_MAIN_GROUP = 31;                  // Main: 0-31
    constexpr uint8_t MAX_MIDDLE_GROUP = 7;                 // Middle: 0-7
    constexpr uint8_t MAX_SUB_GROUP = 255;                  // Sub: 0-255
}

// ============================================================================
// Physical Layer Constants
// ============================================================================

namespace physical {
    // TP1 (Twisted Pair) constants
    constexpr uint32_t TP1_BAUD_RATE = 9600;                // 9600 baud
    constexpr uint8_t TP1_STOP_BITS = 1;
    constexpr uint8_t TP1_DATA_BITS = 8;
    constexpr char TP1_PARITY = 'E';                        // Even parity
    
    // TPUART specific
    constexpr uint8_t TPUART_RESET_CMD = 0x01;
    constexpr uint8_t TPUART_STATE_REQ = 0x02;
    constexpr uint8_t TPUART_ACTIVATE_BUSMON = 0x05;
    constexpr uint8_t TPUART_ACK_INFO = 0x10;
    constexpr uint8_t TPUART_FRAME_END = 0x00;
    
    // IP constants (KNXnet/IP)
    constexpr NetIpPort KNXNETIP_DEFAULT_PORT = NetIpPort(netip::config::kDefaultPort);
    constexpr uint8_t KNXNETIP_PROTOCOL_VERSION = 0x10;     // Version 1.0
    constexpr size_t KNXNETIP_HEADER_SIZE = 6;
    
    // Multicast addresses
    inline const IpAddress KNXNETIP_MULTICAST_ADDR = IpAddress::fromOctets(224, 0, 23, 12);
    constexpr NetIpPort KNXNETIP_SYSTEM_SETUP_MULTICAST_PORT = NetIpPort(netip::config::kDefaultPort);
}

// ============================================================================
// Application Layer Constants
// ============================================================================

namespace application {
    // Service codes (APCI)
    constexpr uint16_t APCI_GROUP_VALUE_READ = 0x0000;
    constexpr uint16_t APCI_GROUP_VALUE_RESPONSE = 0x0040;
    constexpr uint16_t APCI_GROUP_VALUE_WRITE = 0x0080;
    
    constexpr uint16_t APCI_INDIVIDUAL_ADDR_WRITE = 0x00C0;
    constexpr uint16_t APCI_INDIVIDUAL_ADDR_READ = 0x0100;
    constexpr uint16_t APCI_INDIVIDUAL_ADDR_RESPONSE = 0x0140;
    
    constexpr uint16_t APCI_DEVICE_DESCRIPTOR_READ = 0x0300;
    constexpr uint16_t APCI_DEVICE_DESCRIPTOR_RESPONSE = 0x0340;
    
    constexpr uint16_t APCI_RESTART = 0x0380;
    
    constexpr uint16_t APCI_MEMORY_READ = 0x0200;
    constexpr uint16_t APCI_MEMORY_RESPONSE = 0x0240;
    constexpr uint16_t APCI_MEMORY_WRITE = 0x0280;
    
    constexpr uint16_t APCI_PROPERTY_VALUE_READ = 0x03D5;
    constexpr uint16_t APCI_PROPERTY_VALUE_RESPONSE = 0x03D6;
    constexpr uint16_t APCI_PROPERTY_VALUE_WRITE = 0x03D7;
    
    constexpr uint16_t APCI_PROPERTY_DESCRIPTION_READ = 0x03D8;
    constexpr uint16_t APCI_PROPERTY_DESCRIPTION_RESPONSE = 0x03D9;
    
    // Mask version (device type)
    constexpr uint16_t MASK_VERSION_SYSTEM_1 = 0x0010;
    constexpr uint16_t MASK_VERSION_SYSTEM_2 = 0x0020;
    constexpr uint16_t MASK_VERSION_SYSTEM_7 = 0x07B0;
}

} // namespace constants
} // namespace knx
