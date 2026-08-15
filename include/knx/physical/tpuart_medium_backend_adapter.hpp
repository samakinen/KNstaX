// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_medium_backend_adapter.hpp
 * @brief TP1 medium backend adapter for TPUART-class physical implementations
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/physical/tp1_physical_layer.hpp"

#include <span>

namespace knx {
namespace physical {

template <typename PhysicalT>
concept TpuartCompatiblePhysical = requires(PhysicalT& physical,
                                            const PhysicalT& constPhysical,
                                            std::span<const uint8_t> frame,
                                            Toggle mode) {
    { physical.init() } -> std::same_as<util::Result<void>>;
    { physical.close() } -> std::same_as<void>;
    { physical.sendFrame(frame) } -> std::same_as<util::Result<size_t>>;
    { constPhysical.getState() } -> std::same_as<PhysicalLayerState>;
    { physical.setBusMonitorMode(mode) } -> std::same_as<util::Result<void>>;
    { physical.receiveFrameView(0u) } -> std::same_as<util::Result<std::span<const uint8_t>>>;
};

class TpuartMediumBackendAdapter : public Tp1MediumBackend {
public:
    template <TpuartCompatiblePhysical PhysicalT>
    explicit TpuartMediumBackendAdapter(PhysicalT& physical,
                                        Tp1PhysicalFrameSource* frameSource = nullptr)
        : _physical(bindPhysical(physical))
        , _frameSource(frameSource)
        , _eventCallback(nullptr)
        , _eventContext(nullptr)
        , _config()
        , _initialized(false)
    {
    }

    util::Result<void> init(const Tp1MediumConfig& config) override;
    void close() override;

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override;

    void setEventCallback(Tp1EventCallback callback, void* context) override;

    Tp1MediumState getState() const override;
    Tp1CapabilityProfile getCapabilities() const override;

    util::Result<void> setBusMonitorMode(bool enabled) override;
    util::Result<void> service() override;

private:
    struct PhysicalPort {
        void* context{nullptr};
        util::Result<void> (*init)(void*){nullptr};
        void (*close)(void*){nullptr};
        util::Result<void> (*sendFrame)(void*, std::span<const uint8_t>){nullptr};
        PhysicalLayerState (*getState)(const void*){nullptr};
        util::Result<void> (*setBusMonitorMode)(void*, Toggle){nullptr};
        util::Result<std::span<const uint8_t>> (*receiveFrameView)(void*, uint32_t){nullptr};
    };

    template <TpuartCompatiblePhysical PhysicalT>
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
            .sendFrame = [](void* context, std::span<const uint8_t> frame) -> util::Result<void> {
                auto result = static_cast<ConcretePhysical*>(context)->sendFrame(frame);
                if (result.isError()) {
                    return result.error();
                }
                return util::Result<void>::ok();
            },
            .getState = [](const void* context) -> PhysicalLayerState {
                return static_cast<const ConcretePhysical*>(context)->getState();
            },
            .setBusMonitorMode = [](void* context, Toggle mode) -> util::Result<void> {
                return static_cast<ConcretePhysical*>(context)->setBusMonitorMode(mode);
            },
            .receiveFrameView = [](void* context, uint32_t timeoutMs) -> util::Result<std::span<const uint8_t>> {
                return static_cast<ConcretePhysical*>(context)->receiveFrameView(timeoutMs);
            },
        };
    }

    static Tp1MediumState mapState(PhysicalLayerState state);
    static Tp1AckClass mapSendErrorToAckClass(util::ErrorCode code);

    PhysicalPort _physical;
    Tp1PhysicalFrameSource* _frameSource;
    Tp1EventCallback _eventCallback;
    void* _eventContext;
    Tp1MediumConfig _config;
    bool _initialized;
};

} // namespace physical
} // namespace knx
