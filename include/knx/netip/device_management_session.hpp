// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/device_management.hpp"
#include "knx/netip/tunneling_session_client.hpp"

#if KNX_SECURE_ENABLED
#include "knx/netip/ip_secure/secure_tunneling_client.hpp"
#endif

#include <cstddef>
#include <span>
#include <type_traits>
#include <variant>

namespace knx {
namespace netip {

template <typename SessionClient, typename DeviceManagementClientType>
class BasicDeviceManagementSession {
public:
    using OperationType = DeviceManagementClient::OperationType;

    struct PropertyReadPlanStep {
        device_management::PropertyAccessTarget target{};
        std::span<uint8_t> responsePayload{};
        device_management::PropertyReadConfirmationView* confirmation{nullptr};
        int timeoutMs{1000};
    };

    struct PropertyWritePlanStep {
        device_management::PropertyAccessTarget target{};
        std::span<const uint8_t> data{};
        std::span<uint8_t> responsePayload{};
        device_management::PropertyWriteConfirmation* confirmation{nullptr};
        int timeoutMs{1000};
    };

    struct ConfigurationExchangePlanStep {
        std::span<const uint8_t> requestPayload{};
        std::span<uint8_t> responsePayload{};
        size_t* responseLength{nullptr};
        int timeoutMs{1000};
    };

    using PlanStep = std::variant<PropertyReadPlanStep, PropertyWritePlanStep, ConfigurationExchangePlanStep>;

    enum class PlanPhase : uint8_t {
        Idle = 0,
        Running,
        Success,
        Cancelled,
        Failed,
        Timeout,
        TransmissionFailed,
    };

    struct PlanStatus {
        bool active{false};
        PlanPhase phase{PlanPhase::Idle};
        size_t totalSteps{0};
        size_t completedSteps{0};
        size_t activeStepIndex{0};
        OperationType activeOperation{OperationType::None};
        util::ErrorCode terminalError{util::ErrorCode::Success};
    };

    enum class OpenState : uint8_t {
        Idle = 0,
        OpeningSession,
        ConnectingTunneling,
        Ready,
    };

    SessionClient& session() noexcept { return session_; }
    const SessionClient& session() const noexcept { return session_; }

    DeviceManagementClientType& deviceManagement() noexcept { return deviceManagement_; }
    const DeviceManagementClientType& deviceManagement() const noexcept { return deviceManagement_; }

    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept
    {
        session_.setTimingPlatform(timingPlatform);
        deviceManagement_.setTimingPlatform(timingPlatform);
    }

    platform::TimingPlatform* timingPlatform() const noexcept
    {
        return session_.timingPlatform();
    }

    OpenState openState() const noexcept { return openState_; }
    bool isOpen() const noexcept { return openState_ == OpenState::Ready; }

    auto activeSessionOperation() const noexcept { return session_.activeSessionOperation(); }
    OperationType activeOperation() const noexcept
    {
        return deviceManagement_.activeOperation();
    }

    bool isPlanPending() const noexcept { return plan_.active; }

    PlanStatus planStatus() const noexcept { return planStatus_; }

    bool isOperationPending() const noexcept
    {
        return deviceManagement_.isOperationPending();
    }

    void cancelOperation() noexcept
    {
        cancelPlan();
    }

    void cancelPlan() noexcept
    {
        if (plan_.active) {
            planStatus_.active = false;
            planStatus_.phase = PlanPhase::Cancelled;
            planStatus_.completedSteps = plan_.activeStepIndex;
            planStatus_.activeOperation = OperationType::None;
            planStatus_.terminalError = util::ErrorCode::Success;
        }
        plan_.reset();
        deviceManagement_.cancelOperation();
    }

    void close() noexcept
    {
        cancelPlan();
        deviceManagement_.close();
        session_.close();
        openState_ = OpenState::Idle;
        connectTimeoutMs_ = 0;
    }

    util::Result<bool> pollSession(int timeoutMs = 0)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return session_.poll(timeoutMs);
    }

    util::Result<void> beginReadProperty(const device_management::PropertyAccessTarget& target,
                                         std::span<uint8_t> responsePayload,
                                         int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.beginReadProperty(target, responsePayload, timeoutMs);
    }

    util::Result<util::OperationProgressState> pollReadProperty(
        device_management::PropertyReadConfirmationView& outConfirmation)
    {
        return deviceManagement_.pollReadProperty(outConfirmation);
    }

    util::Result<device_management::PropertyReadConfirmationView> readProperty(
        const device_management::PropertyAccessTarget& target,
        std::span<uint8_t> responsePayload,
        int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.readProperty(target, responsePayload, timeoutMs);
    }

    util::Result<void> beginWriteProperty(const device_management::PropertyAccessTarget& target,
                                          std::span<const uint8_t> data,
                                          std::span<uint8_t> responsePayload,
                                          int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.beginWriteProperty(target, data, responsePayload, timeoutMs);
    }

    util::Result<util::OperationProgressState> pollWriteProperty(
        device_management::PropertyWriteConfirmation& outConfirmation)
    {
        return deviceManagement_.pollWriteProperty(outConfirmation);
    }

    util::Result<device_management::PropertyWriteConfirmation> writeProperty(
        const device_management::PropertyAccessTarget& target,
        std::span<const uint8_t> data,
        std::span<uint8_t> responsePayload,
        int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.writeProperty(target, data, responsePayload, timeoutMs);
    }

    util::Result<void> beginConfigurationExchange(std::span<const uint8_t> requestPayload,
                                                  std::span<uint8_t> responsePayload,
                                                  int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.beginConfigurationExchange(requestPayload, responsePayload, timeoutMs);
    }

    util::Result<util::OperationProgressState> pollConfigurationExchange(size_t& responseLength)
    {
        return deviceManagement_.pollConfigurationExchange(responseLength);
    }

    util::Result<size_t> configurationExchange(std::span<const uint8_t> requestPayload,
                                               std::span<uint8_t> responsePayload,
                                               int timeoutMs = 1000)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        return deviceManagement_.configurationExchange(requestPayload, responsePayload, timeoutMs);
    }

    util::Result<void> beginPlan(std::span<PlanStep> steps)
    {
        if (!isOpen()) return util::ErrorCode::NotInitialized;
        if (steps.empty()) return util::ErrorCode::InvalidParameter;
        if (plan_.active || deviceManagement_.isOperationPending()) return util::ErrorCode::Busy;

        plan_.active = true;
        plan_.activeStepIndex = 0;
        plan_.steps = steps;
        planStatus_.active = true;
        planStatus_.phase = PlanPhase::Running;
        planStatus_.totalSteps = steps.size();
        planStatus_.completedSteps = 0;
        planStatus_.activeStepIndex = 0;
        planStatus_.activeOperation = OperationType::None;
        planStatus_.terminalError = util::ErrorCode::Success;

        auto beginResult = beginCurrentPlanStep_();
        if (beginResult.isError()) {
            planStatus_.active = false;
            planStatus_.phase = PlanPhase::Failed;
            planStatus_.activeOperation = OperationType::None;
            planStatus_.terminalError = beginResult.error();
            plan_.reset();
            return beginResult.error();
        }

        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollPlan()
    {
        if (!plan_.active) return util::ErrorCode::OperationNotReady;

        const auto stepProgress = std::visit(
            [this](auto& step) -> util::Result<util::OperationProgressState> {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, PropertyReadPlanStep>) {
                    device_management::PropertyReadConfirmationView confirmation{};
                    auto progress = deviceManagement_.pollReadProperty(confirmation);
                    if (progress.isError()) return progress.error();
                    if (progress.value() != util::OperationProgressState::Success) return progress.value();
                    if (step.confirmation) {
                        *step.confirmation = confirmation;
                    }
                    return util::OperationProgressState::Success;
                } else if constexpr (std::is_same_v<Step, PropertyWritePlanStep>) {
                    device_management::PropertyWriteConfirmation confirmation{};
                    auto progress = deviceManagement_.pollWriteProperty(confirmation);
                    if (progress.isError()) return progress.error();
                    if (progress.value() != util::OperationProgressState::Success) return progress.value();
                    if (step.confirmation) {
                        *step.confirmation = confirmation;
                    }
                    return util::OperationProgressState::Success;
                } else {
                    size_t responseLength = 0;
                    auto progress = deviceManagement_.pollConfigurationExchange(responseLength);
                    if (progress.isError()) return progress.error();
                    if (progress.value() != util::OperationProgressState::Success) return progress.value();
                    if (step.responseLength) {
                        *step.responseLength = responseLength;
                    }
                    return util::OperationProgressState::Success;
                }
            },
            plan_.steps[plan_.activeStepIndex]);
        if (stepProgress.isError()) {
            planStatus_.active = false;
            planStatus_.phase = PlanPhase::Failed;
            planStatus_.completedSteps = plan_.activeStepIndex;
            planStatus_.activeStepIndex = plan_.activeStepIndex;
            planStatus_.activeOperation = OperationType::None;
            planStatus_.terminalError = stepProgress.error();
            plan_.reset();
            return stepProgress.error();
        }

        switch (stepProgress.value()) {
            case util::OperationProgressState::Success:
                ++plan_.activeStepIndex;
                planStatus_.completedSteps = plan_.activeStepIndex;
                if (plan_.activeStepIndex >= plan_.steps.size()) {
                    planStatus_.active = false;
                    planStatus_.phase = PlanPhase::Success;
                    planStatus_.activeStepIndex = planStatus_.totalSteps;
                    planStatus_.activeOperation = OperationType::None;
                    planStatus_.terminalError = util::ErrorCode::Success;
                    plan_.reset();
                    return util::OperationProgressState::Success;
                }
                break;
            case util::OperationProgressState::Pending:
            case util::OperationProgressState::Busy:
                planStatus_.active = true;
                planStatus_.phase = PlanPhase::Running;
                planStatus_.activeStepIndex = plan_.activeStepIndex;
                planStatus_.activeOperation = activeOperation();
                return stepProgress.value();
            case util::OperationProgressState::Timeout:
                planStatus_.active = false;
                planStatus_.phase = PlanPhase::Timeout;
                planStatus_.completedSteps = plan_.activeStepIndex;
                planStatus_.activeStepIndex = plan_.activeStepIndex;
                planStatus_.activeOperation = OperationType::None;
                planStatus_.terminalError = util::ErrorCode::Timeout;
                plan_.reset();
                return stepProgress.value();
            case util::OperationProgressState::TransmissionFailed:
                planStatus_.active = false;
                planStatus_.phase = PlanPhase::TransmissionFailed;
                planStatus_.completedSteps = plan_.activeStepIndex;
                planStatus_.activeStepIndex = plan_.activeStepIndex;
                planStatus_.activeOperation = OperationType::None;
                planStatus_.terminalError = util::ErrorCode::TransmissionFailed;
                plan_.reset();
                return stepProgress.value();
        }

        auto beginResult = beginCurrentPlanStep_();
        if (beginResult.isError()) {
            planStatus_.active = false;
            planStatus_.phase = PlanPhase::Failed;
            planStatus_.activeStepIndex = plan_.activeStepIndex;
            planStatus_.activeOperation = OperationType::None;
            planStatus_.terminalError = beginResult.error();
            plan_.reset();
            return beginResult.error();
        }

        planStatus_.active = true;
        planStatus_.phase = PlanPhase::Running;
        planStatus_.activeStepIndex = plan_.activeStepIndex;
        planStatus_.activeOperation = currentPlanStepOperation_();

        return util::OperationProgressState::Pending;
    }

    util::Result<void> executePlan(std::span<PlanStep> steps)
    {
        auto beginResult = beginPlan(steps);
        if (beginResult.isError()) return beginResult.error();

        auto terminal = detail::waitForTerminalProgress(timingPlatform(), [this]() { return pollPlan(); });
        if (terminal.isError()) return terminal.error();
        return detail::completionResult(terminal.value());
    }

protected:
    BasicDeviceManagementSession() = default;
    ~BasicDeviceManagementSession() = default;

    void markOpeningSession() noexcept { openState_ = OpenState::OpeningSession; }
    void markConnectingTunneling() noexcept { openState_ = OpenState::ConnectingTunneling; }
    void markReady() noexcept { openState_ = OpenState::Ready; }
    int connectTimeoutMs() const noexcept { return connectTimeoutMs_; }
    void setConnectTimeoutMs(int timeoutMs) noexcept { connectTimeoutMs_ = timeoutMs; }

    SessionClient session_{};
    DeviceManagementClientType deviceManagement_{};

private:
    struct Plan {
        bool active{false};
        size_t activeStepIndex{0};
        std::span<PlanStep> steps{};

        constexpr void reset() noexcept
        {
            active = false;
            activeStepIndex = 0;
            steps = {};
        }
    };

    constexpr OperationType currentPlanStepOperation_() const noexcept
    {
        if (!plan_.active || plan_.activeStepIndex >= plan_.steps.size()) {
            return OperationType::None;
        }

        return std::visit(
            [](const auto& step) constexpr noexcept -> OperationType {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, PropertyReadPlanStep>) {
                    return OperationType::PropertyRead;
                } else if constexpr (std::is_same_v<Step, PropertyWritePlanStep>) {
                    return OperationType::PropertyWrite;
                } else {
                    return OperationType::ConfigurationExchange;
                }
            },
            plan_.steps[plan_.activeStepIndex]);
    }

    util::Result<void> beginCurrentPlanStep_()
    {
        planStatus_.activeStepIndex = plan_.activeStepIndex;
        planStatus_.activeOperation = currentPlanStepOperation_();
        return std::visit(
            [this](const auto& step) -> util::Result<void> {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, PropertyReadPlanStep>) {
                    return deviceManagement_.beginReadProperty(step.target, step.responsePayload, step.timeoutMs);
                } else if constexpr (std::is_same_v<Step, PropertyWritePlanStep>) {
                    return deviceManagement_.beginWriteProperty(step.target, step.data, step.responsePayload, step.timeoutMs);
                } else {
                    return deviceManagement_.beginConfigurationExchange(step.requestPayload,
                                                                        step.responsePayload,
                                                                        step.timeoutMs);
                }
            },
            plan_.steps[plan_.activeStepIndex]);
    }

    OpenState openState_{OpenState::Idle};
    int connectTimeoutMs_{0};
    Plan plan_{};
    PlanStatus planStatus_{};
};

class TunnelingDeviceManagementSession final
    : public BasicDeviceManagementSession<TunnelingSessionClient, DeviceManagementClient> {
public:
    TunnelingDeviceManagementSession() = default;

    util::Result<void> beginOpen(platform::NetworkInterface& network,
                                 IpAddress host,
                                 NetIpPort port,
                                 int timeoutMs = 1000)
    {
        close();

        auto deviceManagementOpen = deviceManagement_.open(network, host, port);
        if (deviceManagementOpen.isError()) return deviceManagementOpen.error();

        auto sessionOpen = session_.beginOpen(network, host, port, timeoutMs);
        if (sessionOpen.isError()) {
            close();
            return sessionOpen.error();
        }

        markOpeningSession();
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollOpen()
    {
        if (openState() == OpenState::Ready) return util::OperationProgressState::Success;
        if (openState() != OpenState::OpeningSession) return util::ErrorCode::OperationNotReady;

        auto progress = session_.pollOpen();
        if (progress.isError()) {
            close();
            return progress.error();
        }
        if (progress.value() == util::OperationProgressState::Timeout) {
            close();
            return util::OperationProgressState::Timeout;
        }
        if (progress.value() != util::OperationProgressState::Success) return progress.value();

        deviceManagement_.bindSession(session_);
        markReady();
        return util::OperationProgressState::Success;
    }

    util::Result<void> open(platform::NetworkInterface& network,
                            IpAddress host,
                            NetIpPort port,
                            int timeoutMs = 1000)
    {
        auto beginResult = beginOpen(network, host, port, timeoutMs);
        if (beginResult.isError()) return beginResult.error();

        auto terminal = detail::waitForTerminalProgress(timingPlatform(), [this]() { return pollOpen(); });
        if (terminal.isError()) return terminal.error();
        return detail::completionResult(terminal.value());
    }
};

#if KNX_SECURE_ENABLED

class SecureTunnelingDeviceManagementSession final
    : public BasicDeviceManagementSession<ip_secure::SecureTunnelingClient, SecureDeviceManagementClient> {
public:
    SecureTunnelingDeviceManagementSession() = default;

    util::Result<void> beginOpen(platform::NetworkInterface& network,
                                 const ip_secure::SecureTunnelingClient::Options& options,
                                 int timeoutMs = 1000,
                                 int connectTimeoutMs = 1000)
    {
        close();

        auto deviceManagementOpen = deviceManagement_.open(network, options.host, options.port);
        if (deviceManagementOpen.isError()) return deviceManagementOpen.error();

        auto sessionOpen = session_.beginOpen(network, options, timeoutMs);
        if (sessionOpen.isError()) {
            close();
            return sessionOpen.error();
        }

        setConnectTimeoutMs(connectTimeoutMs);
        markOpeningSession();
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollOpen()
    {
        switch (openState()) {
            case OpenState::Idle:
                return util::ErrorCode::OperationNotReady;
            case OpenState::Ready:
                return util::OperationProgressState::Success;
            case OpenState::OpeningSession: {
                auto progress = session_.pollOpen();
                if (progress.isError()) {
                    close();
                    return progress.error();
                }
                if (progress.value() == util::OperationProgressState::Timeout) {
                    close();
                    return util::OperationProgressState::Timeout;
                }
                if (progress.value() != util::OperationProgressState::Success) return progress.value();

                deviceManagement_.setNetIpSecurity(session_.security());
                auto connectBegin = session_.beginConnectTunneling(connectTimeoutMs());
                if (connectBegin.isError()) {
                    close();
                    return connectBegin.error();
                }

                markConnectingTunneling();
                return util::OperationProgressState::Pending;
            }
            case OpenState::ConnectingTunneling: {
                auto progress = session_.pollConnectTunneling();
                if (progress.isError()) {
                    close();
                    return progress.error();
                }
                if (progress.value() == util::OperationProgressState::Timeout) {
                    close();
                    return util::OperationProgressState::Timeout;
                }
                if (progress.value() != util::OperationProgressState::Success) return progress.value();

                deviceManagement_.bindSession(session_);
                markReady();
                return util::OperationProgressState::Success;
            }
        }

        return util::ErrorCode::OperationFailed;
    }

    util::Result<void> open(platform::NetworkInterface& network,
                            const ip_secure::SecureTunnelingClient::Options& options,
                            int timeoutMs = 1000,
                            int connectTimeoutMs = 1000)
    {
        auto beginResult = beginOpen(network, options, timeoutMs, connectTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        auto terminal = detail::waitForTerminalProgress(timingPlatform(), [this]() { return pollOpen(); });
        if (terminal.isError()) return terminal.error();
        return detail::completionResult(terminal.value());
    }
};

#endif

} // namespace netip
} // namespace knx