// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_data_link_layer.hpp
 * @brief TP1 data link layer implementation
 * 
 * Implements KNX TP1 data link layer services (L_Data).
 * Handles frame encoding/decoding, address filtering, and flow control.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/platform/platform.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include <algorithm>
#include <array>
#include <concepts>
#include <initializer_list>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>
#include <chrono>
#include <atomic>

namespace knx {

// Forward declarations to avoid extra includes in this header.
namespace application {
enum class APCIService : uint16_t;
} // namespace application

namespace transport {
enum class TPCI : uint8_t;
} // namespace transport

namespace datalink {

template <typename PhysicalT>
concept Tp1PhysicalLike = requires(PhysicalT& physical,
                                   const PhysicalT& constPhysical,
                                   std::span<const uint8_t> frame,
                                   uint32_t timeoutMs,
                                   uint32_t sequence,
                                   physical::ReceiveCallback callback,
                                   void* callbackContext,
                                   Toggle mode) {
    { physical.init() } -> std::same_as<util::Result<void>>;
    { physical.close() } -> std::same_as<void>;
    { constPhysical.isOpen() } -> std::convertible_to<bool>;
    { physical.sendFrame(frame) } -> std::same_as<util::Result<size_t>>;
    { physical.beginTransmit(frame) } -> std::same_as<util::Result<uint32_t>>;
    { physical.pollTransmit(sequence) } -> std::same_as<util::Result<util::OperationProgressState>>;
    { physical.beginReceive(timeoutMs) } -> std::same_as<util::Result<void>>;
    { physical.pollReceive() } -> std::same_as<util::Result<util::OperationProgressState>>;
    { physical.receivedFrameView() } -> std::same_as<util::Result<std::span<const uint8_t>>>;
    { physical.setReceiveCallback(std::move(callback), callbackContext) } -> std::same_as<void>;
    { constPhysical.getState() } -> std::same_as<physical::PhysicalLayerState>;
    { physical.setBusMonitorMode(mode) } -> std::same_as<util::Result<void>>;
};

/**
 * @brief Promiscuous mode configuration
 */
enum class PromiscuousMode : uint8_t {
    Disable = 0,
    Enable = 1
};

/**
 * @brief L_Data service primitives
 */
enum class LDataService {
    Request,      // L_Data.req - send frame
    Confirm,      // L_Data.con - send confirmation
    Indication,   // L_Data.ind - receive frame
};

/**
 * @brief L_Data frame structure
 */
struct LDataFrame {
    // KNX TP1 constraints
    static constexpr size_t MAX_APDU_SIZE = 254;  // Maximum data size per KNX spec
    static constexpr size_t MAX_FRAME_SIZE = 256; // Max frame including header bytes
    bool standardFrame;      // True for standard, false for extended
    bool repeated;           // True = this frame is a repetition. The TP1/cEMI
                             // wire bit is inverted (1 = not repeated); the
                             // codecs translate.
    Priority priority;       // Message priority
    bool ackRequested;       // Acknowledge requested
    bool confirmation;       // Confirmation (0=error, 1=success)
    
    // Addressing
    IndividualAddress source;
    GroupAddress destination;     // Destination address (interpreted by destinationType)
    AddressType destinationType;  // Destination address type
    
    // Hop count (for routing)
    uint8_t hopCount;
    
    // TPDU (TPCI/APCI header + APDU payload bytes)
    // Canonical representation across the stack.
    // Bounded to MAX_FRAME_SIZE (256 bytes): 2-byte header + 254-byte APDU.
    util::FixedVector<uint8_t, MAX_FRAME_SIZE> tpdu;
    
    LDataFrame() 
        : standardFrame(true)
        , repeated(false)
        , priority(Priority::Low)
        , ackRequested(true)
        , confirmation(false)
        , source(0)
        , destination()
        , destinationType(AddressType::Group)
        , hopCount(6)
    {}
    
    /**
     * @brief Validate frame size constraints
     * @return true if frame data is within KNX limits, false otherwise
     */
    bool isValid() const {
        // Source may be a normal operational address or the commissioning
        // marker used before a unique individual address has been assigned.
        if (!source.isValid() && !isInitialIndividualAddress(source)) {
            return false;
        }

        // If destination is a group address, allow both normal group addresses
        // and 0/0/0 system-broadcast used by management procedures.
        if (destinationType == AddressType::Group &&
            !destination.isValid() &&
            !isGroupBroadcastAddress(destination)) {
            return false;
        }

        // Individual destinations: allow normal operational addresses plus the
        // default/initial address 15.15.255 — the ETS "programming via default
        // address" management procedures (03_05_02) address it directly.
        if (destinationType == AddressType::Individual) {
            const IndividualAddress individualDestination(destination.raw);
            if (!individualDestination.isValid()
                && !isInitialIndividualAddress(individualDestination)
                && !isIndividualBroadcastAddress(individualDestination)) {
                return false;
            }
        }

        // TPDU must contain at least one byte; some management/control
        // telegrams are encoded as 1-byte TPDU on TP1.
        if (tpdu.empty()) {
            return false;
        }

        // Check APDU payload size limit (254 bytes for TP1)
        if (knx::protocol::tpduPayloadLength(tpdu) > MAX_APDU_SIZE) {
            return false;
        }
        // Hop count is 3-bit field, so valid range 0-7 (though spec recommends 0-6)
        if (hopCount > 7) {
            return false;
        }
        return true;
    }
    
    void setTpdu(protocol::TPCIField tpci, application::APCIField apci, std::span<const uint8_t> payload) {
        if (!knx::protocol::buildTpduInPlace(tpci, apci, payload, tpdu).isOk()) {
            tpdu.clear();
        }
    }

    void setTpdu(protocol::TPCIField tpci, application::APCIService apci, std::span<const uint8_t> payload) {
        setTpdu(tpci, application::APCIField::create(apci), payload);
    }

    void setTpdu(protocol::TPCI tpci, application::APCIField apci, std::span<const uint8_t> payload) {
        setTpdu(protocol::TPCIField::create(tpci), apci, payload);
    }

    void setTpdu(protocol::TPCI tpci, application::APCIService apci, std::span<const uint8_t> payload) {
        setTpdu(protocol::TPCIField::create(tpci), application::APCIField::create(apci), payload);
    }

    void setTpdu(protocol::TPCIField tpci, application::APCIField apci, std::initializer_list<uint8_t> payload) {
        if (!knx::protocol::buildTpduInPlace(tpci, apci, payload, tpdu).isOk()) {
            tpdu.clear();
        }
    }

    void setTpdu(protocol::TPCIField tpci, application::APCIService apci, std::initializer_list<uint8_t> payload) {
        setTpdu(tpci, application::APCIField::create(apci), payload);
    }

    void setTpdu(protocol::TPCI tpci, application::APCIField apci, std::initializer_list<uint8_t> payload) {
        setTpdu(protocol::TPCIField::create(tpci), apci, payload);
    }

    void setTpdu(protocol::TPCI tpci, application::APCIService apci, std::initializer_list<uint8_t> payload) {
        setTpdu(protocol::TPCIField::create(tpci), application::APCIField::create(apci), payload);
    }

    protocol::TPCIField tpci() const {
        if (tpdu.size() < 2) return protocol::TPCIField(0);
        return knx::protocol::unpackTpduHeader(tpdu[0], tpdu[1]).tpci;
    }

    application::APCIField apci() const {
        if (tpdu.size() < 2) return application::APCIField(0);
        return knx::protocol::unpackTpduHeader(tpdu[0], tpdu[1]).apci;
    }

    std::span<const uint8_t> payload() const {
        if (tpdu.size() < 2) return {};
        return std::span<const uint8_t>(tpdu).subspan(2);
    }

    void setDestination(const GroupAddress& addr) {
        destination = addr;
        destinationType = AddressType::Group;
    }

    void setDestination(const IndividualAddress& addr) {
        destination.raw = addr.raw;
        destinationType = AddressType::Individual;
    }

};

/**
 * @brief L_Data callback
 *
 * The `frame` reference is only valid for the duration of the callback.
 */
using LDataCallback = util::InplaceFunction<void(const LDataFrame& frame), 64>;

/**
 * @brief TP1 Data Link Layer Configuration
 */
struct Tp1DataLinkConfig {
    uint32_t rxTaskStackSize{config::RX_TASK_STACK_SIZE};  // RX task stack size in bytes
    uint32_t rxTaskPriority{5};      // RX task priority (0 = lowest)
    uint32_t txMutexTimeout{1000};   // TX mutex timeout in milliseconds
    bool enableRxTask{true};         // Run RX processing on a background task
    
    static Tp1DataLinkConfig defaults() {
        return Tp1DataLinkConfig{};
    }
    
    /**
     * @brief Validate configuration parameters
     * @return Result<void> indicating success or specific error
     */
    util::Result<void> validate() const {
        // Validate stack size (must be reasonable for FreeRTOS)
        // Minimum 512 bytes, maximum 64KB
        if (rxTaskStackSize < 512 || rxTaskStackSize > 65536) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        // Validate priority (FreeRTOS allows 0-24, with 24 being highest)
        // But we use tskIDLE_PRIORITY (0) to configMAX_PRIORITIES-1 range
        if (rxTaskPriority > 24) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        // Validate timeout (0 = no wait, portMAX_DELAY = infinite wait)
        // Reasonable range: 0 to 60 seconds (60000 ms)
        if (txMutexTimeout > 60000) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        return util::Result<void>::ok();
    }
};

/**
 * @brief TP1 data link layer
 * 
 * Uses the platform tasking/synchronization abstraction for RX processing.
 */
class Tp1DataLinkLayer {
public:
    static constexpr size_t MAX_GROUP_ADDRESSES = 256;

    using ProgressState = util::OperationProgressState;
    using TxProgressState = util::OperationProgressState;
    using RxProgressState = util::OperationProgressState;

    template <Tp1PhysicalLike PhysicalT>
    explicit Tp1DataLinkLayer(platform::TaskingPlatform& platform,
                              PhysicalT& physical,
                              const Tp1DataLinkConfig& config)
        : Tp1DataLinkLayer(platform, physical, nullptr, config)
    {
    }

    template <Tp1PhysicalLike PhysicalT>
    explicit Tp1DataLinkLayer(platform::TaskingPlatform& platform,
                              PhysicalT& physical,
                              platform::TimingPlatform* timingPlatform = nullptr,
                              const Tp1DataLinkConfig& config = Tp1DataLinkConfig::defaults())
        : _platform(platform)
        , _physicalPort(bindPhysical(physical))
        , _timingPlatform(timingPlatform)
        , _ownAddress(0)
        , _promiscuousMode(false)
        , _initialized(false)
        , _rxCallback(nullptr)
        , _config(config)
        , _txMutex(platform)
    {
        std::fill(std::begin(_frameInUse), std::end(_frameInUse), false);
    }
    virtual ~Tp1DataLinkLayer();
    
    // Delete copy and move operations (manages platform tasking resources)
    Tp1DataLinkLayer(const Tp1DataLinkLayer&) = delete;
    Tp1DataLinkLayer& operator=(const Tp1DataLinkLayer&) = delete;
    Tp1DataLinkLayer(Tp1DataLinkLayer&&) = delete;
    Tp1DataLinkLayer& operator=(Tp1DataLinkLayer&&) = delete;
    
    /**
     * @brief Initialize the data link layer
     */
    virtual util::Result<void> init(const IndividualAddress& ownAddress);
    
    /**
     * @brief Close the data link layer
     */
    virtual void close();
    
    /**
     * @brief Check if initialized
     */
    virtual bool isOpen() const;
    
    /**
     * @brief Send L_Data.req
     */
    virtual util::Result<void> sendFrame(const LDataFrame& frame);
    virtual util::Result<void> beginTransmit(const LDataFrame& frame);
    virtual util::Result<ProgressState> pollTransmit();
    util::Result<void> beginReceive(uint32_t firstReadTimeoutMs = 100);
    util::Result<ProgressState> pollReceive();
    size_t rxProgressDeliveredCount() const { return _lastRxProgressDeliveredCount; }
    
    /**
     * @brief Set receive callback
     *
     * @thread_safety Register during initialization, before RX processing
     * starts (init() with enableRxTask, or the first processRxAvailable()).
     * Replacing the callback while the RX task may be invoking it is a race
     * on the callback storage.
     */
    virtual void setReceiveCallback(LDataCallback callback);

    /**
     * @brief Deterministically process any pending RX work
     *
     * Primarily intended for simulator-backed tests and explicit progression hooks.
     * Returns the number of frames successfully decoded and delivered to the upper layer.
     */
    size_t processRxAvailable(uint32_t firstReadTimeoutMs = 100);
    
    /**
     * @brief Add group address to filter table
     */
    util::Result<void> addGroupAddress(const GroupAddress& address);
    
    /**
     * @brief Remove group address from filter table
     */
    util::Result<void> removeGroupAddress(const GroupAddress& address);
    
    /**
     * @brief Clear group address filter table
     */
    void clearGroupAddresses();
    
    /**
     * @brief Enable/disable promiscuous mode (receive all frames)
     */
    void setPromiscuousMode(PromiscuousMode mode);
    void setOwnAddress(const IndividualAddress& ownAddress);
    
    /**
     * @brief Get own physical address
     */
    IndividualAddress getOwnAddress() const { return _ownAddress; }

    /**
     * @brief Diagnostics counters
     *
     * TP1 short-acknowledge (ACK/NAK/BUSY characters) is generated at the
     * MAC/ISR level of the physical backend, so it is not counted here.
     */
    struct Statistics {
        uint32_t rxFrames{0};          // Successfully received frames
        uint32_t txFrames{0};          // Successfully transmitted frames
        uint32_t rxDropped{0};         // Frames dropped (frame pool exhausted)
        uint32_t decodeFailed{0};      // Frame decode failures
        uint32_t filterDropped{0};     // Frames dropped by address filter
        uint32_t duplicatesDropped{0}; // L2 repetitions suppressed (duplication prevention)
        uint32_t txRepetitions{0};     // Frames repeated after a NAK or missing L_ACK
    };

    /**
     * @brief Get diagnostic statistics
     */
    Statistics getStatistics() const {
        Statistics out;
        out.rxFrames = _stats.rxFrames.load(std::memory_order_relaxed);
        out.txFrames = _stats.txFrames.load(std::memory_order_relaxed);
        out.rxDropped = _stats.rxDropped.load(std::memory_order_relaxed);
        out.decodeFailed = _stats.decodeFailed.load(std::memory_order_relaxed);
        out.filterDropped = _stats.filterDropped.load(std::memory_order_relaxed);
        out.duplicatesDropped = _stats.duplicatesDropped.load(std::memory_order_relaxed);
        out.txRepetitions = _stats.txRepetitions.load(std::memory_order_relaxed);
        return out;
    }

private:
    struct PhysicalPort {
        void* context{nullptr};
        util::Result<void> (*init)(void*){nullptr};
        void (*close)(void*){nullptr};
        bool (*isOpen)(const void*){nullptr};
        util::Result<size_t> (*sendFrame)(void*, std::span<const uint8_t>){nullptr};
        util::Result<uint32_t> (*beginTransmit)(void*, std::span<const uint8_t>){nullptr};
        util::Result<ProgressState> (*pollTransmit)(void*, uint32_t){nullptr};
        util::Result<void> (*beginReceive)(void*, uint32_t){nullptr};
        util::Result<ProgressState> (*pollReceive)(void*){nullptr};
        util::Result<std::span<const uint8_t>> (*receivedFrameView)(void*){nullptr};
        void (*setReceiveCallback)(void*, physical::ReceiveCallback, void*){nullptr};
        void (*setQueueNotifyCallback)(void*, physical::Tp1MediumBackend::QueueNotifyCallback, void*){nullptr};
        physical::PhysicalLayerState (*getState)(const void*){nullptr};
        util::Result<void> (*setBusMonitorMode)(void*, Toggle){nullptr};
        util::Result<void> (*setOwnAddress)(void*, uint16_t){nullptr};
        void (*setAckGroupAddresses)(void*, std::span<const uint16_t>){nullptr};
        // True when the physical can notify the RX task about queued frames.
        // Physicals without it (e.g. the IP adapters, which only surface
        // frames when polled) are polled on a short interval instead of the
        // long notify timeout.
        bool hasQueueNotify{false};
    };

    template <Tp1PhysicalLike PhysicalT>
    static PhysicalPort bindPhysical(PhysicalT& physical)
    {
        using ConcretePhysical = std::remove_reference_t<PhysicalT>;

        return PhysicalPort{
            .context = &physical,
            .init = [](void* context) -> util::Result<void> {
                return static_cast<ConcretePhysical*>(context)->init();
            },
            .close = [](void* context) {
                static_cast<ConcretePhysical*>(context)->close();
            },
            .isOpen = [](const void* context) -> bool {
                return static_cast<const ConcretePhysical*>(context)->isOpen();
            },
            .sendFrame = [](void* context, std::span<const uint8_t> frame) -> util::Result<size_t> {
                return static_cast<ConcretePhysical*>(context)->sendFrame(frame);
            },
            .beginTransmit = [](void* context, std::span<const uint8_t> frame) -> util::Result<uint32_t> {
                return static_cast<ConcretePhysical*>(context)->beginTransmit(frame);
            },
            .pollTransmit = [](void* context, uint32_t sequence) -> util::Result<ProgressState> {
                return static_cast<ConcretePhysical*>(context)->pollTransmit(sequence);
            },
            .beginReceive = [](void* context, uint32_t timeoutMs) -> util::Result<void> {
                return static_cast<ConcretePhysical*>(context)->beginReceive(timeoutMs);
            },
            .pollReceive = [](void* context) -> util::Result<ProgressState> {
                return static_cast<ConcretePhysical*>(context)->pollReceive();
            },
            .receivedFrameView = [](void* context) -> util::Result<std::span<const uint8_t>> {
                return static_cast<ConcretePhysical*>(context)->receivedFrameView();
            },
            .setReceiveCallback = [](void* context, physical::ReceiveCallback callback, void* callbackContext) {
                static_cast<ConcretePhysical*>(context)->setReceiveCallback(std::move(callback), callbackContext);
            },
            .setQueueNotifyCallback = [](void* context, physical::Tp1MediumBackend::QueueNotifyCallback callback, void* callbackContext) {
                if constexpr (requires(ConcretePhysical* p) { p->setQueueNotifyCallback(callback, callbackContext); }) {
                    static_cast<ConcretePhysical*>(context)->setQueueNotifyCallback(callback, callbackContext);
                } else {
                    (void)context;
                    (void)callback;
                    (void)callbackContext;
                }
            },
            .getState = [](const void* context) -> physical::PhysicalLayerState {
                return static_cast<const ConcretePhysical*>(context)->getState();
            },
            .setBusMonitorMode = [](void* context, Toggle mode) -> util::Result<void> {
                return static_cast<ConcretePhysical*>(context)->setBusMonitorMode(mode);
            },
            .setOwnAddress = [] (void* context, uint16_t addressRaw) -> util::Result<void> {
                if constexpr (requires(ConcretePhysical* p) { p->setOwnAddress(addressRaw); }) {
                    return static_cast<ConcretePhysical*>(context)->setOwnAddress(addressRaw);
                }
                return util::Result<void>::ok();
            },
            .setAckGroupAddresses = [] (void* context, std::span<const uint16_t> addresses) {
                if constexpr (requires(ConcretePhysical* p) { p->setAckGroupAddresses(addresses); }) {
                    static_cast<ConcretePhysical*>(context)->setAckGroupAddresses(addresses);
                } else {
                    (void)context;
                    (void)addresses;
                }
            },
            .hasQueueNotify = requires(ConcretePhysical* p,
                                       physical::Tp1MediumBackend::QueueNotifyCallback cb,
                                       void* ctx) { p->setQueueNotifyCallback(cb, ctx); },
        };
    }

    static constexpr size_t FRAME_POOL_SIZE = 16;

    /// Push the current group-address list down to the physical layer's
    /// ISR-side L_ACK matcher (no-op for media without that capability).
    void publishAckGroupAddresses();

    platform::TaskingPlatform& _platform;
    PhysicalPort _physicalPort;
    platform::TimingPlatform* _timingPlatform;
    IndividualAddress _ownAddress;
    util::FixedVector<GroupAddress, MAX_GROUP_ADDRESSES> _groupAddresses;
    bool _promiscuousMode;
    bool _initialized;
    
    std::atomic<const LDataCallback*> _rxCallback;
    LDataCallback _rxCallbackStorage;

    // Lock-free diagnostics counters (relaxed ordering; approximate reads are fine).
    struct AtomicStatistics {
        std::atomic<uint32_t> rxFrames{0};
        std::atomic<uint32_t> txFrames{0};
        std::atomic<uint32_t> rxDropped{0};
        std::atomic<uint32_t> decodeFailed{0};
        std::atomic<uint32_t> filterDropped{0};
        std::atomic<uint32_t> duplicatesDropped{0};
        std::atomic<uint32_t> txRepetitions{0};
    };
    AtomicStatistics _stats;

    // Duplication prevention (03_02_02 §2.3): an L_Data.ind is delivered only
    // if the frame is not a repetition of the directly preceding correctly
    // received frame. Single-slot cache of that frame (RX-path only).
    struct LastRxFrameCache {
        bool valid{false};
        uint16_t source{0};
        uint16_t destination{0};
        AddressType destinationType{AddressType::Individual};
        Priority priority{Priority::Low};
        uint8_t hopCount{0};
        util::FixedVector<uint8_t, LDataFrame::MAX_FRAME_SIZE> tpdu;
    };
    LastRxFrameCache _lastRxFrame;

    bool isRepetitionOfLastRxFrame(const LDataFrame& frame) const;
    void rememberRxFrame(const LDataFrame& frame);
    Tp1DataLinkConfig _config;

    platform::Mutex _txMutex;
    platform::TaskHandle _rxTask{nullptr};
    std::atomic<bool> _rxTaskStopRequested{false};

    struct TxOperationState {
        bool active{false};
        bool waitingPhysicalOutcome{false};
        bool usingPhysicalProgression{false};
        // Two independent retry budgets. A "busy" outcome is purely local
        // backpressure — our own driver is still transmitting — while a NAK or
        // a missing L_ACK is a protocol event that 03/02/02 answers with a
        // frame repetition. Sharing one counter let local backpressure consume
        // the spec-mandated repetition budget, so a frame that hit busy three
        // times gave up on the very first missing L_ACK, which a management
        // client only ever sees as "the device does not respond".
        uint8_t busyAttempt{0};
        uint8_t repeatAttempt{0};
        uint32_t physicalOutcomeSequence{0};
        uint64_t retryNotBeforeMs{0};
        size_t length{0};
        // Sized for a full L_Data_Extended frame: 8-byte header + 254-byte APDU + FCS.
        std::array<uint8_t, 263> buffer{};
        constexpr std::span<uint8_t> availableSpan() noexcept {return std::span<uint8_t>(buffer);} 
        constexpr std::span<uint8_t> filledSpan() noexcept {return std::span<uint8_t>(buffer).first(length);}
        constexpr std::span<const uint8_t> filledSpan() const noexcept {return std::span<const uint8_t>(buffer).first(length);}
    };
    TxOperationState _txOperation{};

    struct RxOperationState {
        bool active{false};
        bool waitingPhysicalReceive{false};
        uint32_t firstReadTimeoutMs{0};
        uint32_t readIndex{0};
        size_t deliveredCount{0};
        LDataFrame* currentFrame{nullptr};
    };
    RxOperationState _rxOperation{};
    size_t _lastRxProgressDeliveredCount{0};

    // Frame pool to avoid heap allocation in RX path
    LDataFrame _framePool[FRAME_POOL_SIZE];
    bool _frameInUse[FRAME_POOL_SIZE] = {false};

    // Guard against re-entrant RX callback processing (important for test/mocked physical layers).
    std::atomic<bool> _rxProcessing{false};

    LDataFrame* allocateFrame();
    void releaseFrame(LDataFrame* frame);

    void finishTxOperation();
    /// Mark the buffered TX frame as a repetition (clears the "not repeated"
    /// control bit) and recompute the FCS over the modified frame.
    void markTxFrameAsRepeated();
    void finishRxOperation();

    // Address filtering
    bool isAddressMatch(const GroupAddress& address, AddressType addressType) const;
    
    // RX task
    static void rxTaskFunc(void* param);
    size_t processRxAvailableInternal(uint32_t firstReadTimeoutMs);
    
    // Physical layer callbacks
    static void physicalRxCallback(void* context);
    static void physicalQueueNotifyCallback(void* context);
};

} // namespace datalink
} // namespace knx
