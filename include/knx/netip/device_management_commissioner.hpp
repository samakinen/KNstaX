// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/device_management_procedures.hpp"

#include <utility>
#include <variant>

namespace knx {
namespace netip {

template <typename WorkflowType, typename ProceduresType>
class BasicDeviceManagementCommissioner {
public:
    using Workflow = WorkflowType;
    using Procedures = ProceduresType;
    using DeviceIdentity = typename Procedures::DeviceIdentity;
    using SetProgrammingModeResult = typename Procedures::SetProgrammingModeResult;
    using AssignIndividualAddressOptions = typename Procedures::AssignIndividualAddressOptions;
    using AssignIndividualAddressResult = typename Procedures::AssignIndividualAddressResult;
    using CommissionIndividualAddressOptions = typename Procedures::CommissionIndividualAddressOptions;
    using CommissionIndividualAddressResult = typename Procedures::CommissionIndividualAddressResult;
    using ProcedureType = typename Procedures::ProcedureType;

    enum class OperationType : uint8_t {
        None = 0,
        OpenAndReadDeviceIdentity,
        OpenAndReadProgrammingMode,
        OpenAndSetProgrammingMode,
        OpenAndAssignIndividualAddress,
        OpenAndCommissionIndividualAddress,
    };

    enum class OperationPhase : uint8_t {
        Idle = 0,
        Opening,
        ExecutingProcedure,
        Success,
        Cancelled,
        Failed,
        Timeout,
        TransmissionFailed,
    };

    struct OperationStatus {
        bool active{false};
        OperationType type{OperationType::None};
        OperationPhase phase{OperationPhase::Idle};
        ProcedureType activeProcedure{ProcedureType::None};
        util::ErrorCode terminalError{util::ErrorCode::Success};
    };

    BasicDeviceManagementCommissioner() noexcept
        : procedures_(workflow_)
    {
    }

    Workflow& workflow() noexcept { return workflow_; }
    const Workflow& workflow() const noexcept { return workflow_; }

    Procedures& procedures() noexcept { return procedures_; }
    const Procedures& procedures() const noexcept { return procedures_; }

    bool isOpen() const noexcept { return workflow_.isOpen(); }
    auto openState() const noexcept { return workflow_.openState(); }
    auto activeSessionOperation() const noexcept { return workflow_.activeSessionOperation(); }
    auto activeProcedure() const noexcept { return procedures_.activeProcedure(); }
    auto procedureStatus() const noexcept { return procedures_.procedureStatus(); }
    OperationStatus operationStatus() const noexcept { return operationStatus_; }

    void cancel() noexcept
    {
        if (operationStatus_.active && operationStatus_.phase == OperationPhase::Opening) {
            workflow_.close();
        }
        procedures_.cancel();
        operation_ = {};
        operationStatus_.active = false;
        operationStatus_.phase = OperationPhase::Cancelled;
        operationStatus_.activeProcedure = ProcedureType::None;
        operationStatus_.terminalError = util::ErrorCode::Success;
    }

    void close() noexcept
    {
        cancel();
        workflow_.close();
        operationStatus_ = {};
    }

    bool shouldBeginOpen_() const noexcept
    {
        return !workflow_.isOpen() && workflow_.openState() == Workflow::OpenState::Idle;
    }

protected:
    struct OpenAndReadIdentityOperation {
        int timeoutMs{1000};
    };

    struct OpenAndReadProgrammingModeOperation {
        int timeoutMs{1000};
    };

    struct OpenAndSetProgrammingModeOperation {
        Toggle mode{Toggle::Disable};
        int timeoutMs{1000};
    };

    struct OpenAndAssignIndividualAddressOperation {
        IndividualAddress address{};
        AssignIndividualAddressOptions options{};
    };

    struct OpenAndCommissionOperation {
        IndividualAddress address{};
        CommissionIndividualAddressOptions options{};
    };

    using Operation = std::variant<std::monostate,
                                   OpenAndReadIdentityOperation,
                                   OpenAndReadProgrammingModeOperation,
                                   OpenAndSetProgrammingModeOperation,
                                   OpenAndAssignIndividualAddressOperation,
                                   OpenAndCommissionOperation>;

    util::Result<void> beginOpenAndReadDeviceIdentity_(util::Result<void> openResult, int timeoutMs)
    {
        return beginOpenAndProcedure_(OperationType::OpenAndReadDeviceIdentity,
                                      ProcedureType::ReadDeviceIdentity,
                                      openResult,
                                      OpenAndReadIdentityOperation{timeoutMs},
                                      [this, timeoutMs]() { return procedures_.beginReadDeviceIdentity(timeoutMs); });
    }

    template <typename PollOpenFn>
    util::Result<util::OperationProgressState> pollOpenAndReadDeviceIdentity_(PollOpenFn&& pollOpen,
                                                                              DeviceIdentity& outIdentity)
    {
        return pollOpenAndProcedure_<OpenAndReadIdentityOperation>(OperationType::OpenAndReadDeviceIdentity,
                                                                   ProcedureType::ReadDeviceIdentity,
                                                                   std::forward<PollOpenFn>(pollOpen),
                                                                   outIdentity,
                                                                   [this](const OpenAndReadIdentityOperation& op) {
                                                                       return procedures_.beginReadDeviceIdentity(op.timeoutMs);
                                                                   },
                                                                   [this](DeviceIdentity& result) {
                                                                       return procedures_.pollReadDeviceIdentity(result);
                                                                   });
    }

    util::Result<void> beginOpenAndReadProgrammingMode_(util::Result<void> openResult, int timeoutMs)
    {
        return beginOpenAndProcedure_(OperationType::OpenAndReadProgrammingMode,
                                      ProcedureType::ReadProgrammingMode,
                                      openResult,
                                      OpenAndReadProgrammingModeOperation{timeoutMs},
                                      [this, timeoutMs]() { return procedures_.beginReadProgrammingMode(timeoutMs); });
    }

    template <typename PollOpenFn>
    util::Result<util::OperationProgressState> pollOpenAndReadProgrammingMode_(PollOpenFn&& pollOpen,
                                                                               Toggle& outProgrammingMode)
    {
        return pollOpenAndProcedure_<OpenAndReadProgrammingModeOperation>(OperationType::OpenAndReadProgrammingMode,
                                                                          ProcedureType::ReadProgrammingMode,
                                                                          std::forward<PollOpenFn>(pollOpen),
                                                                          outProgrammingMode,
                                                                          [this](const OpenAndReadProgrammingModeOperation& op) {
                                                                              return procedures_.beginReadProgrammingMode(op.timeoutMs);
                                                                          },
                                                                          [this](Toggle& result) {
                                                                              return procedures_.pollReadProgrammingMode(result);
                                                                          });
    }

    util::Result<void> beginOpenAndSetProgrammingMode_(util::Result<void> openResult,
                                                       Toggle mode,
                                                       int timeoutMs)
    {
        return beginOpenAndProcedure_(OperationType::OpenAndSetProgrammingMode,
                                      ProcedureType::SetProgrammingMode,
                                      openResult,
                                      OpenAndSetProgrammingModeOperation{mode, timeoutMs},
                                      [this, mode, timeoutMs]() { return procedures_.beginSetProgrammingMode(mode, timeoutMs); });
    }

    template <typename PollOpenFn>
    util::Result<util::OperationProgressState> pollOpenAndSetProgrammingMode_(PollOpenFn&& pollOpen,
                                                                              SetProgrammingModeResult& outResult)
    {
        return pollOpenAndProcedure_<OpenAndSetProgrammingModeOperation>(OperationType::OpenAndSetProgrammingMode,
                                                                         ProcedureType::SetProgrammingMode,
                                                                         std::forward<PollOpenFn>(pollOpen),
                                                                         outResult,
                                                                         [this](const OpenAndSetProgrammingModeOperation& op) {
                                                                             return procedures_.beginSetProgrammingMode(op.mode, op.timeoutMs);
                                                                         },
                                                                         [this](SetProgrammingModeResult& result) {
                                                                             return procedures_.pollSetProgrammingMode(result);
                                                                         });
    }

    util::Result<void> beginOpenAndAssignIndividualAddress_(util::Result<void> openResult,
                                                            IndividualAddress address,
                                                            AssignIndividualAddressOptions options)
    {
        return beginOpenAndProcedure_(OperationType::OpenAndAssignIndividualAddress,
                                      ProcedureType::AssignIndividualAddress,
                                      openResult,
                                      OpenAndAssignIndividualAddressOperation{address, options},
                                      [this, address, options]() {
                                          return procedures_.beginAssignIndividualAddress(address, options);
                                      });
    }

    template <typename PollOpenFn>
    util::Result<util::OperationProgressState> pollOpenAndAssignIndividualAddress_(PollOpenFn&& pollOpen,
                                                                                   AssignIndividualAddressResult& outResult)
    {
        return pollOpenAndProcedure_<OpenAndAssignIndividualAddressOperation>(OperationType::OpenAndAssignIndividualAddress,
                                                                              ProcedureType::AssignIndividualAddress,
                                                                              std::forward<PollOpenFn>(pollOpen),
                                                                              outResult,
                                                                              [this](const OpenAndAssignIndividualAddressOperation& op) {
                                                                                  return procedures_.beginAssignIndividualAddress(op.address,
                                                                                                                                 op.options);
                                                                              },
                                                                              [this](AssignIndividualAddressResult& result) {
                                                                                  return procedures_.pollAssignIndividualAddress(result);
                                                                              });
    }

    util::Result<void> beginOpenAndCommissionIndividualAddress_(util::Result<void> openResult,
                                                                IndividualAddress address,
                                                                CommissionIndividualAddressOptions options)
    {
        return beginOpenAndProcedure_(OperationType::OpenAndCommissionIndividualAddress,
                                      ProcedureType::CommissionIndividualAddress,
                                      openResult,
                                      OpenAndCommissionOperation{address, options},
                                      [this, address, options]() {
                                          return procedures_.beginCommissionIndividualAddress(address, options);
                                      });
    }

    template <typename PollOpenFn>
    util::Result<util::OperationProgressState> pollOpenAndCommissionIndividualAddress_(PollOpenFn&& pollOpen,
                                                                                        CommissionIndividualAddressResult& outResult)
    {
        return pollOpenAndProcedure_<OpenAndCommissionOperation>(OperationType::OpenAndCommissionIndividualAddress,
                                                                 ProcedureType::CommissionIndividualAddress,
                                                                 std::forward<PollOpenFn>(pollOpen),
                                                                 outResult,
                                                                 [this](const OpenAndCommissionOperation& op) {
                                                                     return procedures_.beginCommissionIndividualAddress(op.address,
                                                                                                                          op.options);
                                                                 },
                                                                 [this](CommissionIndividualAddressResult& result) {
                                                                     return procedures_.pollCommissionIndividualAddress(result);
                                                                 });
    }

private:
    template <typename OperationValue, typename BeginProcedureFn>
    util::Result<void> beginOpenAndProcedure_(OperationType operationType,
                                              ProcedureType procedureType,
                                              util::Result<void> openResult,
                                              OperationValue pendingOperation,
                                              BeginProcedureFn&& beginProcedure)
    {
        auto ready = validateCombinedOperationStart_();
        if (ready.isError()) return ready.error();
        if constexpr (requires { pendingOperation.address; }) {
            if (!pendingOperation.address.isValid()) return util::ErrorCode::InvalidParameter;
        }

        if (isOpen()) {
            auto beginResult = beginProcedure();
            if (beginResult.isError()) return beginResult.error();

            operation_ = {};
            operationStatus_ = {true,
                                operationType,
                                OperationPhase::ExecutingProcedure,
                                procedureType,
                                util::ErrorCode::Success};
            return util::Result<void>::ok();
        }
        if (openResult.isError()) return openResult.error();

        operation_.template emplace<OperationValue>(std::move(pendingOperation));
        operationStatus_ = {true,
                            operationType,
                            OperationPhase::Opening,
                            ProcedureType::None,
                            util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    template <typename OperationValue, typename PollOpenFn, typename ResultType, typename BeginProcedureFn, typename PollProcedureFn>
    util::Result<util::OperationProgressState> pollOpenAndProcedure_(OperationType operationType,
                                                                     ProcedureType procedureType,
                                                                     PollOpenFn&& pollOpen,
                                                                     ResultType& outResult,
                                                                     BeginProcedureFn&& beginProcedure,
                                                                     PollProcedureFn&& pollProcedure)
    {
        if (std::holds_alternative<std::monostate>(operation_)) {
            if (procedures_.activeProcedure() == procedureType) {
                return pollProcedure_(operationType, procedureType, outResult, std::forward<PollProcedureFn>(pollProcedure));
            }
            return util::ErrorCode::OperationNotReady;
        }
        if (!std::holds_alternative<OperationValue>(operation_)) {
            return util::ErrorCode::OperationNotReady;
        }

        auto& op = std::get<OperationValue>(operation_);
        if (operationStatus_.phase == OperationPhase::Opening) {
            auto progress = pollOpen();
            if (progress.isError()) return failCombinedProgress_(progress.error());
            if (progress.value() == util::OperationProgressState::Timeout) {
                workflow_.close();
                return finishCombinedTerminal_(OperationPhase::Timeout, util::ErrorCode::Timeout, progress.value());
            }
            if (progress.value() != util::OperationProgressState::Success) return progress.value();

            auto beginResult = beginProcedure(op);
            if (beginResult.isError()) return failCombinedProgress_(beginResult.error());
            operationStatus_.phase = OperationPhase::ExecutingProcedure;
            operationStatus_.activeProcedure = procedureType;
            return util::OperationProgressState::Pending;
        }

        return pollProcedure_(operationType, procedureType, outResult, std::forward<PollProcedureFn>(pollProcedure));
    }

    util::Result<void> validateCombinedOperationStart_() const noexcept
    {
        if (operationStatus_.active) return util::ErrorCode::Busy;
        if (procedures_.procedureStatus().active) return util::ErrorCode::Busy;
        if (workflow_.isPlanPending() || workflow_.isOperationPending()) {
            return util::ErrorCode::Busy;
        }
        return util::Result<void>::ok();
    }

    template <typename ResultType, typename PollProcedureFn>
    util::Result<util::OperationProgressState> pollProcedure_(OperationType operationType,
                                                              ProcedureType procedureType,
                                                              ResultType& outResult,
                                                              PollProcedureFn&& pollProcedure)
    {
        auto progress = pollProcedure(outResult);
        if (progress.isError()) return failCombinedProgress_(progress.error());
        if (progress.value() == util::OperationProgressState::Timeout) {
            return finishCombinedTerminal_(OperationPhase::Timeout, util::ErrorCode::Timeout, progress.value());
        }
        if (progress.value() == util::OperationProgressState::TransmissionFailed) {
            return finishCombinedTerminal_(OperationPhase::TransmissionFailed,
                                           util::ErrorCode::TransmissionFailed,
                                           progress.value());
        }
        if (progress.value() != util::OperationProgressState::Success) {
            operationStatus_.active = true;
            operationStatus_.phase = OperationPhase::ExecutingProcedure;
            operationStatus_.activeProcedure = procedureType;
            operationStatus_.terminalError = util::ErrorCode::Success;
            return progress.value();
        }

        operation_ = {};
        operationStatus_ = {false,
                            operationType,
                            OperationPhase::Success,
                            ProcedureType::None,
                            util::ErrorCode::Success};
        return util::OperationProgressState::Success;
    }

    void sleepForNextPoll_() noexcept
    {
        detail::delayForNextPoll(workflow_.timingPlatform());
    }

    util::Result<util::OperationProgressState> failCombinedProgress_(util::ErrorCode error)
    {
        operation_ = {};
        operationStatus_.active = false;
        operationStatus_.phase = OperationPhase::Failed;
        operationStatus_.activeProcedure = ProcedureType::None;
        operationStatus_.terminalError = error;
        return error;
    }

    util::Result<util::OperationProgressState> finishCombinedTerminal_(OperationPhase phase,
                                                                       util::ErrorCode error,
                                                                       util::OperationProgressState progress)
    {
        operation_ = {};
        operationStatus_.active = false;
        operationStatus_.phase = phase;
        operationStatus_.activeProcedure = ProcedureType::None;
        operationStatus_.terminalError = error;
        return progress;
    }

protected:
    Workflow workflow_{};
    Procedures procedures_;

    template <typename ResultType, typename PollFn>
    util::Result<ResultType> waitForOperationResult_(PollFn&& poll)
    {
        while (true) {
            ResultType result{};
            auto progress = poll(result);
            if (progress.isError()) return progress.error();
            switch (progress.value()) {
                case util::OperationProgressState::Success:
                    return result;
                case util::OperationProgressState::Pending:
                case util::OperationProgressState::Busy:
                    sleepForNextPoll_();
                    continue;
                case util::OperationProgressState::Timeout:
                    return util::ErrorCode::Timeout;
                case util::OperationProgressState::TransmissionFailed:
                    return util::ErrorCode::TransmissionFailed;
            }
        }
    }

private:
    Operation operation_{};
    OperationStatus operationStatus_{};
};

class TunnelingDeviceManagementCommissioner final
    : public BasicDeviceManagementCommissioner<TunnelingDeviceManagementSession, TunnelingDeviceManagementProcedures> {
public:
    util::Result<void> beginOpen(platform::NetworkInterface& network,
                                 IpAddress host,
                                 NetIpPort port,
                                 int timeoutMs = 1000)
    {
        return workflow().beginOpen(network, host, port, timeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpen()
    {
        return workflow().pollOpen();
    }

    util::Result<void> open(platform::NetworkInterface& network,
                            IpAddress host,
                            NetIpPort port,
                            int timeoutMs = 1000)
    {
        return workflow().open(network, host, port, timeoutMs);
    }

    util::Result<void> beginOpenAndReadDeviceIdentity(platform::NetworkInterface& network,
                                                      IpAddress host,
                                                      NetIpPort port,
                                                      int openTimeoutMs = 1000,
                                                      int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, host, port, openTimeoutMs);
        }
        return beginOpenAndReadDeviceIdentity_(openResult, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndReadDeviceIdentity(DeviceIdentity& outIdentity)
    {
        return pollOpenAndReadDeviceIdentity_([this]() { return pollOpen(); }, outIdentity);
    }

    util::Result<DeviceIdentity> openAndReadDeviceIdentity(platform::NetworkInterface& network,
                                                           IpAddress host,
                                                           NetIpPort port,
                                                           int openTimeoutMs = 1000,
                                                           int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndReadDeviceIdentity(network, host, port, openTimeoutMs, procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<DeviceIdentity>(
            [this](DeviceIdentity& identity) { return pollOpenAndReadDeviceIdentity(identity); });
    }

    util::Result<void> beginOpenAndReadProgrammingMode(platform::NetworkInterface& network,
                                                       IpAddress host,
                                                       NetIpPort port,
                                                       int openTimeoutMs = 1000,
                                                       int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, host, port, openTimeoutMs);
        }
        return beginOpenAndReadProgrammingMode_(openResult, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndReadProgrammingMode(Toggle& outProgrammingMode)
    {
        return pollOpenAndReadProgrammingMode_([this]() { return pollOpen(); }, outProgrammingMode);
    }

    util::Result<Toggle> openAndReadProgrammingMode(platform::NetworkInterface& network,
                                                    IpAddress host,
                                                    NetIpPort port,
                                                    int openTimeoutMs = 1000,
                                                    int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndReadProgrammingMode(network, host, port, openTimeoutMs, procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<Toggle>(
            [this](Toggle& programmingMode) { return pollOpenAndReadProgrammingMode(programmingMode); });
    }

    util::Result<void> beginOpenAndSetProgrammingMode(platform::NetworkInterface& network,
                                                      IpAddress host,
                                                      NetIpPort port,
                                                      Toggle mode,
                                                      int openTimeoutMs = 1000,
                                                      int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, host, port, openTimeoutMs);
        }
        return beginOpenAndSetProgrammingMode_(openResult, mode, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndSetProgrammingMode(SetProgrammingModeResult& outResult)
    {
        return pollOpenAndSetProgrammingMode_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<SetProgrammingModeResult> openAndSetProgrammingMode(platform::NetworkInterface& network,
                                                                     IpAddress host,
                                                                     NetIpPort port,
                                                                     Toggle mode,
                                                                     int openTimeoutMs = 1000,
                                                                     int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndSetProgrammingMode(network, host, port, mode, openTimeoutMs, procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<SetProgrammingModeResult>(
            [this](SetProgrammingModeResult& result) { return pollOpenAndSetProgrammingMode(result); });
    }

    util::Result<void> beginOpenAndAssignIndividualAddress(platform::NetworkInterface& network,
                                                           IpAddress host,
                                                           NetIpPort port,
                                                           IndividualAddress address,
                                                           AssignIndividualAddressOptions options = {},
                                                           int openTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, host, port, openTimeoutMs);
        }
        return beginOpenAndAssignIndividualAddress_(openResult, address, options);
    }

    util::Result<util::OperationProgressState> pollOpenAndAssignIndividualAddress(
        AssignIndividualAddressResult& outResult)
    {
        return pollOpenAndAssignIndividualAddress_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<AssignIndividualAddressResult> openAndAssignIndividualAddress(platform::NetworkInterface& network,
                                                                               IpAddress host,
                                                                               NetIpPort port,
                                                                               IndividualAddress address,
                                                                               AssignIndividualAddressOptions options = {},
                                                                               int openTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndAssignIndividualAddress(network, host, port, address, options, openTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<AssignIndividualAddressResult>(
            [this](AssignIndividualAddressResult& result) { return pollOpenAndAssignIndividualAddress(result); });
    }

    util::Result<void> beginOpenAndCommissionIndividualAddress(
        platform::NetworkInterface& network,
        IpAddress host,
        NetIpPort port,
        IndividualAddress address,
        CommissionIndividualAddressOptions options = {},
        int openTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, host, port, openTimeoutMs);
        }
        return beginOpenAndCommissionIndividualAddress_(openResult, address, options);
    }

    util::Result<util::OperationProgressState> pollOpenAndCommissionIndividualAddress(
        CommissionIndividualAddressResult& outResult)
    {
        return pollOpenAndCommissionIndividualAddress_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<CommissionIndividualAddressResult> openAndCommissionIndividualAddress(
        platform::NetworkInterface& network,
        IpAddress host,
        NetIpPort port,
        IndividualAddress address,
        CommissionIndividualAddressOptions options = {},
        int openTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndCommissionIndividualAddress(network, host, port, address, options, openTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<CommissionIndividualAddressResult>(
            [this](CommissionIndividualAddressResult& result) { return pollOpenAndCommissionIndividualAddress(result); });
    }
};

#if KNX_SECURE_ENABLED

class SecureTunnelingDeviceManagementCommissioner final
    : public BasicDeviceManagementCommissioner<SecureTunnelingDeviceManagementSession,
                                               SecureTunnelingDeviceManagementProcedures> {
public:
    using Options = ip_secure::SecureTunnelingClient::Options;

    util::Result<void> beginOpen(platform::NetworkInterface& network,
                                 const Options& options,
                                 int timeoutMs = 1000,
                                 int connectTimeoutMs = 1000)
    {
        return workflow().beginOpen(network, options, timeoutMs, connectTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpen()
    {
        return workflow().pollOpen();
    }

    util::Result<void> open(platform::NetworkInterface& network,
                            const Options& options,
                            int timeoutMs = 1000,
                            int connectTimeoutMs = 1000)
    {
        return workflow().open(network, options, timeoutMs, connectTimeoutMs);
    }

    util::Result<void> beginOpenAndReadDeviceIdentity(platform::NetworkInterface& network,
                                                      const Options& options,
                                                      int openTimeoutMs = 1000,
                                                      int connectTimeoutMs = 1000,
                                                      int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, options, openTimeoutMs, connectTimeoutMs);
        }
        return beginOpenAndReadDeviceIdentity_(openResult, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndReadDeviceIdentity(DeviceIdentity& outIdentity)
    {
        return pollOpenAndReadDeviceIdentity_([this]() { return pollOpen(); }, outIdentity);
    }

    util::Result<DeviceIdentity> openAndReadDeviceIdentity(platform::NetworkInterface& network,
                                                           const Options& options,
                                                           int openTimeoutMs = 1000,
                                                           int connectTimeoutMs = 1000,
                                                           int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndReadDeviceIdentity(network,
                                                          options,
                                                          openTimeoutMs,
                                                          connectTimeoutMs,
                                                          procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<DeviceIdentity>(
            [this](DeviceIdentity& identity) { return pollOpenAndReadDeviceIdentity(identity); });
    }

    util::Result<void> beginOpenAndReadProgrammingMode(platform::NetworkInterface& network,
                                                       const Options& options,
                                                       int openTimeoutMs = 1000,
                                                       int connectTimeoutMs = 1000,
                                                       int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, options, openTimeoutMs, connectTimeoutMs);
        }
        return beginOpenAndReadProgrammingMode_(openResult, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndReadProgrammingMode(Toggle& outProgrammingMode)
    {
        return pollOpenAndReadProgrammingMode_([this]() { return pollOpen(); }, outProgrammingMode);
    }

    util::Result<Toggle> openAndReadProgrammingMode(platform::NetworkInterface& network,
                                                    const Options& options,
                                                    int openTimeoutMs = 1000,
                                                    int connectTimeoutMs = 1000,
                                                    int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndReadProgrammingMode(network,
                                                           options,
                                                           openTimeoutMs,
                                                           connectTimeoutMs,
                                                           procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<Toggle>(
            [this](Toggle& programmingMode) { return pollOpenAndReadProgrammingMode(programmingMode); });
    }

    util::Result<void> beginOpenAndSetProgrammingMode(platform::NetworkInterface& network,
                                                      const Options& options,
                                                      Toggle mode,
                                                      int openTimeoutMs = 1000,
                                                      int connectTimeoutMs = 1000,
                                                      int procedureTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, options, openTimeoutMs, connectTimeoutMs);
        }
        return beginOpenAndSetProgrammingMode_(openResult, mode, procedureTimeoutMs);
    }

    util::Result<util::OperationProgressState> pollOpenAndSetProgrammingMode(SetProgrammingModeResult& outResult)
    {
        return pollOpenAndSetProgrammingMode_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<SetProgrammingModeResult> openAndSetProgrammingMode(platform::NetworkInterface& network,
                                                                     const Options& options,
                                                                     Toggle mode,
                                                                     int openTimeoutMs = 1000,
                                                                     int connectTimeoutMs = 1000,
                                                                     int procedureTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndSetProgrammingMode(network,
                                                          options,
                                                          mode,
                                                          openTimeoutMs,
                                                          connectTimeoutMs,
                                                          procedureTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<SetProgrammingModeResult>(
            [this](SetProgrammingModeResult& result) { return pollOpenAndSetProgrammingMode(result); });
    }

    util::Result<void> beginOpenAndAssignIndividualAddress(
        platform::NetworkInterface& network,
        const Options& options,
        IndividualAddress address,
        AssignIndividualAddressOptions procedureOptions = {},
        int openTimeoutMs = 1000,
        int connectTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, options, openTimeoutMs, connectTimeoutMs);
        }
        return beginOpenAndAssignIndividualAddress_(openResult, address, procedureOptions);
    }

    util::Result<util::OperationProgressState> pollOpenAndAssignIndividualAddress(
        AssignIndividualAddressResult& outResult)
    {
        return pollOpenAndAssignIndividualAddress_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<AssignIndividualAddressResult> openAndAssignIndividualAddress(
        platform::NetworkInterface& network,
        const Options& options,
        IndividualAddress address,
        AssignIndividualAddressOptions procedureOptions = {},
        int openTimeoutMs = 1000,
        int connectTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndAssignIndividualAddress(network,
                                                               options,
                                                               address,
                                                               procedureOptions,
                                                               openTimeoutMs,
                                                               connectTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<AssignIndividualAddressResult>(
            [this](AssignIndividualAddressResult& result) { return pollOpenAndAssignIndividualAddress(result); });
    }

    util::Result<void> beginOpenAndCommissionIndividualAddress(
        platform::NetworkInterface& network,
        const Options& options,
        IndividualAddress address,
        CommissionIndividualAddressOptions procedureOptions = {},
        int openTimeoutMs = 1000,
        int connectTimeoutMs = 1000)
    {
        auto openResult = util::Result<void>::ok();
        if (shouldBeginOpen_()) {
            openResult = beginOpen(network, options, openTimeoutMs, connectTimeoutMs);
        }
        return beginOpenAndCommissionIndividualAddress_(openResult, address, procedureOptions);
    }

    util::Result<util::OperationProgressState> pollOpenAndCommissionIndividualAddress(
        CommissionIndividualAddressResult& outResult)
    {
        return pollOpenAndCommissionIndividualAddress_([this]() { return pollOpen(); }, outResult);
    }

    util::Result<CommissionIndividualAddressResult> openAndCommissionIndividualAddress(
        platform::NetworkInterface& network,
        const Options& options,
        IndividualAddress address,
        CommissionIndividualAddressOptions procedureOptions = {},
        int openTimeoutMs = 1000,
        int connectTimeoutMs = 1000)
    {
        auto beginResult = beginOpenAndCommissionIndividualAddress(network,
                                                                   options,
                                                                   address,
                                                                   procedureOptions,
                                                                   openTimeoutMs,
                                                                   connectTimeoutMs);
        if (beginResult.isError()) return beginResult.error();

        return waitForOperationResult_<CommissionIndividualAddressResult>(
            [this](CommissionIndividualAddressResult& result) { return pollOpenAndCommissionIndividualAddress(result); });
    }
};

#endif

} // namespace netip
} // namespace knx
