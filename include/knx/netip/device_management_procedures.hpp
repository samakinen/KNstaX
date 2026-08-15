// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/application/property.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/device_management_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace knx {
namespace netip {

template <typename Workflow>
class DeviceManagementProcedures {
public:
    using WorkflowType = Workflow;
    using PlanPhase = typename Workflow::PlanPhase;
    using PlanStep = typename Workflow::PlanStep;
    using PropertyReadPlanStep = typename Workflow::PropertyReadPlanStep;
    using PropertyWritePlanStep = typename Workflow::PropertyWritePlanStep;
    using ConfigurationExchangePlanStep = typename Workflow::ConfigurationExchangePlanStep;

    enum class ProcedureType : uint8_t {
        None = 0,
        ReadDeviceIdentity,
        ReadProgrammingMode,
        SetProgrammingMode,
        AssignIndividualAddress,
        CommissionIndividualAddress,
    };

    enum class ProgrammingModeDisposition : uint8_t {
        RestoreOriginal = 0,
        Enable,
        Disable,
    };

    struct DeviceIdentity {
        ManufacturerId manufacturerId{};
        IndividualAddress individualAddress{};
        Toggle programmingMode{Toggle::Disable};
    };

    struct SetProgrammingModeResult {
        Toggle requestedMode{Toggle::Disable};
        Toggle programmingModeBefore{Toggle::Disable};
        Toggle programmingModeAfter{Toggle::Disable};
        bool changed{false};
    };

    struct AssignIndividualAddressOptions {
        ProgrammingModeDisposition programmingModeDisposition{ProgrammingModeDisposition::RestoreOriginal};
        int timeoutMs{1000};
    };

    struct AssignIndividualAddressResult {
        IndividualAddress assignedAddress{};
        Toggle programmingModeBefore{Toggle::Disable};
        Toggle programmingModeAfter{Toggle::Disable};
    };

    struct CommissionIndividualAddressOptions {
        Toggle finalProgrammingMode{Toggle::Disable};
        bool verifyAfterWrite{true};
        int timeoutMs{1000};
    };

    struct CommissionIndividualAddressResult {
        DeviceIdentity identityBefore{};
        DeviceIdentity identityAfter{};
        bool addressChanged{false};
        bool programmingModeChanged{false};
    };

    struct ProcedureStatus {
        bool active{false};
        ProcedureType type{ProcedureType::None};
        PlanPhase phase{PlanPhase::Idle};
        util::ErrorCode terminalError{util::ErrorCode::Success};
    };

    explicit DeviceManagementProcedures(Workflow& workflow) noexcept
        : workflow_(&workflow)
    {
    }

    Workflow& workflow() noexcept { return *workflow_; }
    const Workflow& workflow() const noexcept { return *workflow_; }

    ProcedureType activeProcedure() const noexcept { return status_.type; }
    ProcedureStatus procedureStatus() const noexcept { return status_; }

    void cancel() noexcept
    {
        if (status_.active) {
            workflow_->cancelPlan();
            workflow_->cancelOperation();
            status_.active = false;
            status_.phase = PlanPhase::Cancelled;
            status_.terminalError = util::ErrorCode::Success;
        }
        operation_ = {};
    }

    util::Result<void> beginReadDeviceIdentity(int timeoutMs = 1000)
    {
        auto ready = validateProcedureStart_();
        if (ready.isError()) return ready.error();

        operation_.template emplace<ReadIdentityOperation>();
        auto& read = std::get<ReadIdentityOperation>(operation_);
        configureReadIdentityPlan_(read, timeoutMs);

        auto beginResult = workflow_->beginPlan(read.planStepsSpan());
        if (beginResult.isError()) {
            return failBegin_(ProcedureType::ReadDeviceIdentity, beginResult.error());
        }

        status_ = ProcedureStatus{true, ProcedureType::ReadDeviceIdentity, PlanPhase::Running, util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollReadDeviceIdentity(DeviceIdentity& outIdentity)
    {
        if (!status_.active || status_.type != ProcedureType::ReadDeviceIdentity) {
            return util::ErrorCode::OperationNotReady;
        }

        auto progress = workflow_->pollPlan();
        if (progress.isError()) {
            return failProgress_(ProcedureType::ReadDeviceIdentity, progress.error());
        }
        if (progress.value() != util::OperationProgressState::Success) {
            syncStatusFromWorkflow_(ProcedureType::ReadDeviceIdentity);
            return progress.value();
        }

        auto identityResult = buildDeviceIdentity_(std::get<ReadIdentityOperation>(operation_));
        if (identityResult.isError()) {
            return failProgress_(ProcedureType::ReadDeviceIdentity, identityResult.error());
        }

        outIdentity = identityResult.value();
        status_ = ProcedureStatus{false, ProcedureType::ReadDeviceIdentity, PlanPhase::Success, util::ErrorCode::Success};
        operation_ = {};
        return util::OperationProgressState::Success;
    }

    util::Result<DeviceIdentity> readDeviceIdentity(int timeoutMs = 1000)
    {
        auto beginResult = beginReadDeviceIdentity(timeoutMs);
        if (beginResult.isError()) return beginResult.error();
        return waitForReadDeviceIdentity_();
    }

    util::Result<void> beginReadProgrammingMode(int timeoutMs = 1000)
    {
        auto ready = validateProcedureStart_();
        if (ready.isError()) return ready.error();

        operation_.template emplace<ReadProgrammingModeOperation>();
        auto& read = std::get<ReadProgrammingModeOperation>(operation_);
        read.timeoutMs = timeoutMs;

        auto beginResult = workflow_->beginReadProperty(programmingModePropertyTarget_(), read.responseBuffer, timeoutMs);
        if (beginResult.isError()) {
            return failBegin_(ProcedureType::ReadProgrammingMode, beginResult.error());
        }

        status_ = ProcedureStatus{true, ProcedureType::ReadProgrammingMode, PlanPhase::Running, util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollReadProgrammingMode(Toggle& outProgrammingMode)
    {
        if (!status_.active || status_.type != ProcedureType::ReadProgrammingMode) {
            return util::ErrorCode::OperationNotReady;
        }

        auto& read = std::get<ReadProgrammingModeOperation>(operation_);
        auto progress = workflow_->pollReadProperty(read.confirmation);
        if (progress.isError()) {
            return failProgress_(ProcedureType::ReadProgrammingMode, progress.error());
        }
        if (progress.value() != util::OperationProgressState::Success) {
            updateStatusFromDirectProgress_(ProcedureType::ReadProgrammingMode, progress.value());
            return progress.value();
        }

        auto modeResult = decodeProgMode_(read.confirmation.data);
        if (modeResult.isError()) {
            return failProgress_(ProcedureType::ReadProgrammingMode, modeResult.error());
        }

        outProgrammingMode = modeResult.value();
        status_ = ProcedureStatus{false, ProcedureType::ReadProgrammingMode, PlanPhase::Success, util::ErrorCode::Success};
        operation_ = {};
        return util::OperationProgressState::Success;
    }

    util::Result<Toggle> readProgrammingMode(int timeoutMs = 1000)
    {
        auto beginResult = beginReadProgrammingMode(timeoutMs);
        if (beginResult.isError()) return beginResult.error();

        while (true) {
            Toggle programmingMode{Toggle::Disable};
            auto progress = pollReadProgrammingMode(programmingMode);
            if (progress.isError()) return progress.error();
            switch (progress.value()) {
                case util::OperationProgressState::Success:
                    return programmingMode;
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

    util::Result<void> beginSetProgrammingMode(Toggle mode, int timeoutMs = 1000)
    {
        auto ready = validateProcedureStart_();
        if (ready.isError()) return ready.error();

        operation_.template emplace<SetProgrammingModeOperation>();
        auto& set = std::get<SetProgrammingModeOperation>(operation_);
        set.targetMode = mode;
        set.timeoutMs = timeoutMs;
        set.readResponseBuffer.fill(0);

        auto beginResult = workflow_->beginReadProperty(programmingModePropertyTarget_(), set.readResponseBuffer, timeoutMs);
        if (beginResult.isError()) {
            return failBegin_(ProcedureType::SetProgrammingMode, beginResult.error());
        }

        status_ = ProcedureStatus{true, ProcedureType::SetProgrammingMode, PlanPhase::Running, util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollSetProgrammingMode(SetProgrammingModeResult& outResult)
    {
        if (!status_.active || status_.type != ProcedureType::SetProgrammingMode) {
            return util::ErrorCode::OperationNotReady;
        }

        auto& set = std::get<SetProgrammingModeOperation>(operation_);
        switch (set.phase) {
            case SetProgrammingModeOperation::Phase::ReadingCurrent: {
                auto progress = workflow_->pollReadProperty(set.readConfirmation);
                if (progress.isError()) {
                    return failProgress_(ProcedureType::SetProgrammingMode, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    updateStatusFromDirectProgress_(ProcedureType::SetProgrammingMode, progress.value());
                    return progress.value();
                }

                auto modeResult = decodeProgMode_(set.readConfirmation.data);
                if (modeResult.isError()) {
                    return failProgress_(ProcedureType::SetProgrammingMode, modeResult.error());
                }

                set.modeBefore = modeResult.value();
                set.modeAfter = set.modeBefore;
                if (set.modeBefore == set.targetMode) {
                    outResult = SetProgrammingModeResult{set.targetMode, set.modeBefore, set.modeAfter, false};
                    status_ = ProcedureStatus{false, ProcedureType::SetProgrammingMode, PlanPhase::Success, util::ErrorCode::Success};
                    operation_ = {};
                    return util::OperationProgressState::Success;
                }

                set.writePayload[0] = encodeToggleValue_(set.targetMode);
                auto beginWrite = workflow_->beginWriteProperty(programmingModePropertyTarget_(),
                                                                set.writePayload,
                                                                set.writeResponseBuffer,
                                                                set.timeoutMs);
                if (beginWrite.isError()) {
                    return failProgress_(ProcedureType::SetProgrammingMode, beginWrite.error());
                }

                set.phase = SetProgrammingModeOperation::Phase::WritingRequested;
                updateStatusFromDirectProgress_(ProcedureType::SetProgrammingMode, util::OperationProgressState::Pending);
                return util::OperationProgressState::Pending;
            }
            case SetProgrammingModeOperation::Phase::WritingRequested: {
                auto progress = workflow_->pollWriteProperty(set.writeConfirmation);
                if (progress.isError()) {
                    return failProgress_(ProcedureType::SetProgrammingMode, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    updateStatusFromDirectProgress_(ProcedureType::SetProgrammingMode, progress.value());
                    return progress.value();
                }

                set.modeAfter = set.targetMode;
                outResult = SetProgrammingModeResult{set.targetMode, set.modeBefore, set.modeAfter, true};
                status_ = ProcedureStatus{false, ProcedureType::SetProgrammingMode, PlanPhase::Success, util::ErrorCode::Success};
                operation_ = {};
                return util::OperationProgressState::Success;
            }
        }

        return util::ErrorCode::OperationFailed;
    }

    util::Result<SetProgrammingModeResult> setProgrammingMode(Toggle mode, int timeoutMs = 1000)
    {
        auto beginResult = beginSetProgrammingMode(mode, timeoutMs);
        if (beginResult.isError()) return beginResult.error();

        while (true) {
            SetProgrammingModeResult result{};
            auto progress = pollSetProgrammingMode(result);
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

    util::Result<void> beginAssignIndividualAddress(IndividualAddress address,
                                                    AssignIndividualAddressOptions options = {})
    {
        auto ready = validateProcedureStart_();
        if (ready.isError()) return ready.error();
        if (!address.isValid()) return util::ErrorCode::InvalidParameter;

        operation_.template emplace<AssignAddressOperation>();
        auto& assign = std::get<AssignAddressOperation>(operation_);
        assign.targetAddress = address;
        assign.options = options;
        assign.phase = AssignAddressOperation::Phase::ReadingProgMode;

        auto beginResult = workflow_->beginReadProperty(programmingModePropertyTarget_(),
                                                        assign.readProgModeBuffer,
                                                        options.timeoutMs);
        if (beginResult.isError()) {
            return failBegin_(ProcedureType::AssignIndividualAddress, beginResult.error());
        }

        status_ = ProcedureStatus{true, ProcedureType::AssignIndividualAddress, PlanPhase::Running, util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollAssignIndividualAddress(AssignIndividualAddressResult& outResult)
    {
        if (!status_.active || status_.type != ProcedureType::AssignIndividualAddress) {
            return util::ErrorCode::OperationNotReady;
        }

        auto& assign = std::get<AssignAddressOperation>(operation_);
        return pollAssignAddressOperation_(ProcedureType::AssignIndividualAddress, assign, outResult);
    }

    util::Result<AssignIndividualAddressResult> assignIndividualAddress(IndividualAddress address,
                                                                        AssignIndividualAddressOptions options = {})
    {
        auto beginResult = beginAssignIndividualAddress(address, options);
        if (beginResult.isError()) return beginResult.error();

        while (true) {
            AssignIndividualAddressResult result{};
            auto progress = pollAssignIndividualAddress(result);
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

    util::Result<void> beginCommissionIndividualAddress(IndividualAddress address,
                                                        CommissionIndividualAddressOptions options = {})
    {
        auto ready = validateProcedureStart_();
        if (ready.isError()) return ready.error();
        if (!address.isValid()) return util::ErrorCode::InvalidParameter;

        operation_.template emplace<CommissionAddressOperation>();
        auto& commission = std::get<CommissionAddressOperation>(operation_);
        commission.targetAddress = address;
        commission.options = options;
        configureReadIdentityPlan_(commission.readBefore, options.timeoutMs);

        auto beginResult = workflow_->beginPlan(commission.readBefore.planStepsSpan());
        if (beginResult.isError()) {
            return failBegin_(ProcedureType::CommissionIndividualAddress, beginResult.error());
        }

        status_ = ProcedureStatus{true, ProcedureType::CommissionIndividualAddress, PlanPhase::Running, util::ErrorCode::Success};
        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> pollCommissionIndividualAddress(CommissionIndividualAddressResult& outResult)
    {
        if (!status_.active || status_.type != ProcedureType::CommissionIndividualAddress) {
            return util::ErrorCode::OperationNotReady;
        }

        auto& commission = std::get<CommissionAddressOperation>(operation_);
        switch (commission.phase) {
            case CommissionAddressOperation::Phase::ReadingIdentityBefore: {
                auto progress = workflow_->pollPlan();
                if (progress.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    syncStatusFromWorkflow_(ProcedureType::CommissionIndividualAddress);
                    return progress.value();
                }

                auto identityResult = buildDeviceIdentity_(commission.readBefore);
                if (identityResult.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, identityResult.error());
                }

                commission.identityBefore = identityResult.value();
                if (commission.identityBefore.individualAddress == commission.targetAddress &&
                    commission.identityBefore.programmingMode == commission.options.finalProgrammingMode) {
                    commission.identityAfter = commission.identityBefore;
                    outResult = buildCommissionResult_(commission);
                    status_ = ProcedureStatus{false, ProcedureType::CommissionIndividualAddress, PlanPhase::Success, util::ErrorCode::Success};
                    operation_ = {};
                    return util::OperationProgressState::Success;
                }

                commission.assign.targetAddress = commission.targetAddress;
                commission.assign.options.timeoutMs = commission.options.timeoutMs;
                commission.assign.options.programmingModeDisposition = programmingModeDispositionFor_(commission.options.finalProgrammingMode);
                commission.assign.programmingModeBefore = commission.identityBefore.programmingMode;
                commission.assign.phase = AssignAddressOperation::Phase::ExecutingPlan;
                buildAssignAddressPlan_(commission.assign);

                auto beginPlan = workflow_->beginPlan(commission.assign.planStepsSpan());
                if (beginPlan.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, beginPlan.error());
                }

                commission.phase = CommissionAddressOperation::Phase::ExecutingAddressPlan;
                syncStatusFromWorkflow_(ProcedureType::CommissionIndividualAddress);
                return util::OperationProgressState::Pending;
            }
            case CommissionAddressOperation::Phase::ExecutingAddressPlan: {
                auto progress = workflow_->pollPlan();
                if (progress.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    syncStatusFromWorkflow_(ProcedureType::CommissionIndividualAddress);
                    return progress.value();
                }

                commission.identityAfter = commission.identityBefore;
                commission.identityAfter.individualAddress = commission.targetAddress;
                commission.identityAfter.programmingMode = commission.assign.programmingModeAfter;

                if (!commission.options.verifyAfterWrite) {
                    outResult = buildCommissionResult_(commission);
                    status_ = ProcedureStatus{false, ProcedureType::CommissionIndividualAddress, PlanPhase::Success, util::ErrorCode::Success};
                    operation_ = {};
                    return util::OperationProgressState::Success;
                }

                configureReadIdentityPlan_(commission.readAfter, commission.options.timeoutMs);
                auto beginVerify = workflow_->beginPlan(commission.readAfter.planStepsSpan());
                if (beginVerify.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, beginVerify.error());
                }

                commission.phase = CommissionAddressOperation::Phase::ReadingIdentityAfter;
                syncStatusFromWorkflow_(ProcedureType::CommissionIndividualAddress);
                return util::OperationProgressState::Pending;
            }
            case CommissionAddressOperation::Phase::ReadingIdentityAfter: {
                auto progress = workflow_->pollPlan();
                if (progress.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    syncStatusFromWorkflow_(ProcedureType::CommissionIndividualAddress);
                    return progress.value();
                }

                auto identityResult = buildDeviceIdentity_(commission.readAfter);
                if (identityResult.isError()) {
                    return failProgress_(ProcedureType::CommissionIndividualAddress, identityResult.error());
                }

                commission.identityAfter = identityResult.value();
                outResult = buildCommissionResult_(commission);
                status_ = ProcedureStatus{false, ProcedureType::CommissionIndividualAddress, PlanPhase::Success, util::ErrorCode::Success};
                operation_ = {};
                return util::OperationProgressState::Success;
            }
        }

        return util::ErrorCode::OperationFailed;
    }

    util::Result<CommissionIndividualAddressResult> commissionIndividualAddress(
        IndividualAddress address,
        CommissionIndividualAddressOptions options = {})
    {
        auto beginResult = beginCommissionIndividualAddress(address, options);
        if (beginResult.isError()) return beginResult.error();

        while (true) {
            CommissionIndividualAddressResult result{};
            auto progress = pollCommissionIndividualAddress(result);
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

    static constexpr device_management::PropertyAccessTarget makeDevicePropertyTarget(application::PropertyID propertyId) noexcept
    {
        return device_management::PropertyAccessTarget{
            InterfaceObjectType::knxNetIpParameter(),
            InterfaceObjectInstance(0x01),
            propertyId,
            1,
            1,
        };
    }

private:
    struct ReadIdentityOperation {
        std::array<uint8_t, 16> manufacturerBuffer{};
        std::array<uint8_t, 16> subnetBuffer{};
        std::array<uint8_t, 16> deviceBuffer{};
        std::array<uint8_t, 16> progModeBuffer{};
        device_management::PropertyReadConfirmationView manufacturerConfirmation{};
        device_management::PropertyReadConfirmationView subnetConfirmation{};
        device_management::PropertyReadConfirmationView deviceConfirmation{};
        device_management::PropertyReadConfirmationView progModeConfirmation{};
        std::array<PlanStep, 4> planSteps{};

        std::span<PlanStep> planStepsSpan() noexcept { return planSteps; }
    };

    struct ReadProgrammingModeOperation {
        int timeoutMs{1000};
        std::array<uint8_t, 16> responseBuffer{};
        device_management::PropertyReadConfirmationView confirmation{};
    };

    struct SetProgrammingModeOperation {
        enum class Phase : uint8_t {
            ReadingCurrent = 0,
            WritingRequested,
        };

        Phase phase{Phase::ReadingCurrent};
        Toggle targetMode{Toggle::Disable};
        Toggle modeBefore{Toggle::Disable};
        Toggle modeAfter{Toggle::Disable};
        int timeoutMs{1000};
        std::array<uint8_t, 16> readResponseBuffer{};
        device_management::PropertyReadConfirmationView readConfirmation{};
        std::array<uint8_t, 1> writePayload{};
        std::array<uint8_t, 16> writeResponseBuffer{};
        device_management::PropertyWriteConfirmation writeConfirmation{};
    };

    struct AssignAddressOperation {
        enum class Phase : uint8_t {
            ReadingProgMode = 0,
            ExecutingPlan,
        };

        Phase phase{Phase::ReadingProgMode};
        IndividualAddress targetAddress{};
        AssignIndividualAddressOptions options{};
        Toggle programmingModeBefore{Toggle::Disable};
        Toggle programmingModeAfter{Toggle::Disable};

        std::array<uint8_t, 16> readProgModeBuffer{};
        device_management::PropertyReadConfirmationView readProgModeConfirmation{};

        std::array<uint8_t, 1> progModeEnableData{encodeToggleValue_(Toggle::Enable)};
        std::array<uint8_t, 1> progModeDisableData{encodeToggleValue_(Toggle::Disable)};
        std::array<uint8_t, 1> subnetData{};
        std::array<uint8_t, 1> deviceData{};

        std::array<uint8_t, 16> progModeEnableResponseBuffer{};
        std::array<uint8_t, 16> subnetResponseBuffer{};
        std::array<uint8_t, 16> deviceResponseBuffer{};
        std::array<uint8_t, 16> progModeDisableResponseBuffer{};

        device_management::PropertyWriteConfirmation progModeEnableConfirmation{};
        device_management::PropertyWriteConfirmation subnetConfirmation{};
        device_management::PropertyWriteConfirmation deviceConfirmation{};
        device_management::PropertyWriteConfirmation progModeDisableConfirmation{};

        std::array<PlanStep, 4> planSteps{};
        size_t planStepCount{0};

        std::span<PlanStep> planStepsSpan() noexcept
        {
            return std::span<PlanStep>(planSteps.data(), planStepCount);
        }
    };

    struct CommissionAddressOperation {
        enum class Phase : uint8_t {
            ReadingIdentityBefore = 0,
            ExecutingAddressPlan,
            ReadingIdentityAfter,
        };

        Phase phase{Phase::ReadingIdentityBefore};
        IndividualAddress targetAddress{};
        CommissionIndividualAddressOptions options{};
        DeviceIdentity identityBefore{};
        DeviceIdentity identityAfter{};
        ReadIdentityOperation readBefore{};
        AssignAddressOperation assign{};
        ReadIdentityOperation readAfter{};
    };

    using Operation = std::variant<std::monostate,
                                   ReadIdentityOperation,
                                   ReadProgrammingModeOperation,
                                   SetProgrammingModeOperation,
                                   AssignAddressOperation,
                                   CommissionAddressOperation>;

    static constexpr device_management::PropertyAccessTarget manufacturerPropertyTarget_() noexcept
    {
        return makeDevicePropertyTarget(application::PropertyID::ManufacturerId);
    }

    static constexpr device_management::PropertyAccessTarget subnetPropertyTarget_() noexcept
    {
        return makeDevicePropertyTarget(application::PropertyID::SubnetAddress);
    }

    static constexpr device_management::PropertyAccessTarget devicePropertyTarget_() noexcept
    {
        return makeDevicePropertyTarget(application::PropertyID::DeviceAddress);
    }

    static constexpr device_management::PropertyAccessTarget programmingModePropertyTarget_() noexcept
    {
        return makeDevicePropertyTarget(application::PropertyID::ProgMode);
    }

    static constexpr uint8_t encodeToggleValue_(Toggle toggle) noexcept
    {
        return toggle == Toggle::Enable ? 0x01 : 0x00;
    }

    static constexpr Toggle decodeToggleValue_(uint8_t value) noexcept
    {
        return value == 0x00 ? Toggle::Disable : Toggle::Enable;
    }

    static constexpr uint8_t subnetByte_(IndividualAddress address) noexcept
    {
        return static_cast<uint8_t>((address.raw >> 8) & 0xFF);
    }

    static constexpr uint8_t deviceByte_(IndividualAddress address) noexcept
    {
        return static_cast<uint8_t>(address.raw & 0xFF);
    }

    static constexpr IndividualAddress makeIndividualAddress_(uint8_t subnet, uint8_t device) noexcept
    {
        return IndividualAddress(static_cast<uint16_t>((static_cast<uint16_t>(subnet) << 8) | device));
    }

    static constexpr ProgrammingModeDisposition programmingModeDispositionFor_(Toggle finalProgrammingMode) noexcept
    {
        return finalProgrammingMode == Toggle::Enable
                   ? ProgrammingModeDisposition::Enable
                   : ProgrammingModeDisposition::Disable;
    }

    static constexpr Toggle resolveProgrammingModeAfter_(Toggle programmingModeBefore,
                                                         ProgrammingModeDisposition disposition) noexcept
    {
        switch (disposition) {
            case ProgrammingModeDisposition::RestoreOriginal:
                return programmingModeBefore;
            case ProgrammingModeDisposition::Enable:
                return Toggle::Enable;
            case ProgrammingModeDisposition::Disable:
                return Toggle::Disable;
        }

        return programmingModeBefore;
    }

    void sleepForNextPoll_()
    {
        detail::delayForNextPoll(workflow_->timingPlatform());
    }

    static util::Result<uint8_t> decodeSingleByte_(std::span<const uint8_t> data)
    {
        if (data.size() != 1) return util::ErrorCode::DecodeFailed;
        return data[0];
    }

    static util::Result<ManufacturerId> decodeManufacturerId_(std::span<const uint8_t> data)
    {
        if (data.size() != 2) return util::ErrorCode::DecodeFailed;
        return ManufacturerId(static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]));
    }

    static util::Result<Toggle> decodeProgMode_(std::span<const uint8_t> data)
    {
        auto byte = decodeSingleByte_(data);
        if (byte.isError()) return byte.error();
        return decodeToggleValue_(byte.value());
    }

    static util::Result<DeviceIdentity> buildDeviceIdentity_(const ReadIdentityOperation& read)
    {
        auto manufacturer = decodeManufacturerId_(read.manufacturerConfirmation.data);
        if (manufacturer.isError()) return manufacturer.error();
        auto subnet = decodeSingleByte_(read.subnetConfirmation.data);
        if (subnet.isError()) return subnet.error();
        auto device = decodeSingleByte_(read.deviceConfirmation.data);
        if (device.isError()) return device.error();
        auto progMode = decodeProgMode_(read.progModeConfirmation.data);
        if (progMode.isError()) return progMode.error();

        return DeviceIdentity{
            manufacturer.value(),
            makeIndividualAddress_(subnet.value(), device.value()),
            progMode.value(),
        };
    }

    static CommissionIndividualAddressResult buildCommissionResult_(const CommissionAddressOperation& commission) noexcept
    {
        return CommissionIndividualAddressResult{
            commission.identityBefore,
            commission.identityAfter,
            commission.identityBefore.individualAddress != commission.identityAfter.individualAddress,
            commission.identityBefore.programmingMode != commission.identityAfter.programmingMode,
        };
    }

    util::Result<void> validateProcedureStart_() const noexcept
    {
        if (!workflow_->isOpen()) return util::ErrorCode::NotInitialized;
        if (status_.active || workflow_->isPlanPending() || workflow_->isOperationPending()) {
            return util::ErrorCode::Busy;
        }
        return util::Result<void>::ok();
    }

    util::Result<void> failBegin_(ProcedureType type, util::ErrorCode error)
    {
        status_ = ProcedureStatus{false, type, PlanPhase::Failed, error};
        operation_ = {};
        return error;
    }

    util::Result<util::OperationProgressState> failProgress_(ProcedureType type, util::ErrorCode error)
    {
        status_ = ProcedureStatus{false, type, PlanPhase::Failed, error};
        operation_ = {};
        return error;
    }

    util::Result<DeviceIdentity> waitForReadDeviceIdentity_()
    {
        while (true) {
            DeviceIdentity identity{};
            auto progress = pollReadDeviceIdentity(identity);
            if (progress.isError()) return progress.error();
            switch (progress.value()) {
                case util::OperationProgressState::Success:
                    return identity;
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

    util::Result<util::OperationProgressState> pollAssignAddressOperation_(ProcedureType type,
                                                                           AssignAddressOperation& assign,
                                                                           AssignIndividualAddressResult& outResult)
    {
        switch (assign.phase) {
            case AssignAddressOperation::Phase::ReadingProgMode: {
                auto progress = workflow_->pollReadProperty(assign.readProgModeConfirmation);
                if (progress.isError()) {
                    return failProgress_(type, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    updateStatusFromDirectProgress_(type, progress.value());
                    return progress.value();
                }

                auto progModeResult = decodeProgMode_(assign.readProgModeConfirmation.data);
                if (progModeResult.isError()) {
                    return failProgress_(type, progModeResult.error());
                }

                assign.programmingModeBefore = progModeResult.value();
                assign.phase = AssignAddressOperation::Phase::ExecutingPlan;
                buildAssignAddressPlan_(assign);

                auto beginPlan = workflow_->beginPlan(assign.planStepsSpan());
                if (beginPlan.isError()) {
                    return failProgress_(type, beginPlan.error());
                }

                syncStatusFromWorkflow_(type);
                return util::OperationProgressState::Pending;
            }
            case AssignAddressOperation::Phase::ExecutingPlan: {
                auto progress = workflow_->pollPlan();
                if (progress.isError()) {
                    return failProgress_(type, progress.error());
                }
                if (progress.value() != util::OperationProgressState::Success) {
                    syncStatusFromWorkflow_(type);
                    return progress.value();
                }

                outResult.assignedAddress = assign.targetAddress;
                outResult.programmingModeBefore = assign.programmingModeBefore;
                outResult.programmingModeAfter = assign.programmingModeAfter;
                status_ = ProcedureStatus{false, type, PlanPhase::Success, util::ErrorCode::Success};
                operation_ = {};
                return util::OperationProgressState::Success;
            }
        }

        return util::ErrorCode::OperationFailed;
    }

    void syncStatusFromWorkflow_(ProcedureType type) noexcept
    {
        const auto planStatus = workflow_->planStatus();
        status_.active = planStatus.active;
        status_.type = type;
        status_.phase = planStatus.phase;
        status_.terminalError = planStatus.terminalError;
    }

    void updateStatusFromDirectProgress_(ProcedureType type, util::OperationProgressState progress) noexcept
    {
        status_.type = type;
        switch (progress) {
            case util::OperationProgressState::Pending:
            case util::OperationProgressState::Busy:
                status_.active = true;
                status_.phase = PlanPhase::Running;
                status_.terminalError = util::ErrorCode::Success;
                break;
            case util::OperationProgressState::Timeout:
                status_.active = false;
                status_.phase = PlanPhase::Timeout;
                status_.terminalError = util::ErrorCode::Timeout;
                break;
            case util::OperationProgressState::TransmissionFailed:
                status_.active = false;
                status_.phase = PlanPhase::TransmissionFailed;
                status_.terminalError = util::ErrorCode::TransmissionFailed;
                break;
            case util::OperationProgressState::Success:
                status_.active = false;
                status_.phase = PlanPhase::Success;
                status_.terminalError = util::ErrorCode::Success;
                break;
        }
    }

    void configureReadIdentityPlan_(ReadIdentityOperation& read, int timeoutMs) noexcept
    {
        read.planSteps[0] = PlanStep{PropertyReadPlanStep{
            manufacturerPropertyTarget_(),
            read.manufacturerBuffer,
            &read.manufacturerConfirmation,
            timeoutMs,
        }};
        read.planSteps[1] = PlanStep{PropertyReadPlanStep{
            subnetPropertyTarget_(),
            read.subnetBuffer,
            &read.subnetConfirmation,
            timeoutMs,
        }};
        read.planSteps[2] = PlanStep{PropertyReadPlanStep{
            devicePropertyTarget_(),
            read.deviceBuffer,
            &read.deviceConfirmation,
            timeoutMs,
        }};
        read.planSteps[3] = PlanStep{PropertyReadPlanStep{
            programmingModePropertyTarget_(),
            read.progModeBuffer,
            &read.progModeConfirmation,
            timeoutMs,
        }};
    }

    void buildAssignAddressPlan_(AssignAddressOperation& assign) noexcept
    {
        assign.planStepCount = 0;
        assign.subnetData[0] = subnetByte_(assign.targetAddress);
        assign.deviceData[0] = deviceByte_(assign.targetAddress);

        const Toggle desiredFinalProgrammingMode =
            resolveProgrammingModeAfter_(assign.programmingModeBefore, assign.options.programmingModeDisposition);

        if (assign.programmingModeBefore == Toggle::Disable) {
            assign.planSteps[assign.planStepCount++] = PlanStep{PropertyWritePlanStep{
                programmingModePropertyTarget_(),
                assign.progModeEnableData,
                assign.progModeEnableResponseBuffer,
                &assign.progModeEnableConfirmation,
                assign.options.timeoutMs,
            }};
        }

        assign.planSteps[assign.planStepCount++] = PlanStep{PropertyWritePlanStep{
            subnetPropertyTarget_(),
            assign.subnetData,
            assign.subnetResponseBuffer,
            &assign.subnetConfirmation,
            assign.options.timeoutMs,
        }};
        assign.planSteps[assign.planStepCount++] = PlanStep{PropertyWritePlanStep{
            devicePropertyTarget_(),
            assign.deviceData,
            assign.deviceResponseBuffer,
            &assign.deviceConfirmation,
            assign.options.timeoutMs,
        }};

        assign.programmingModeAfter = desiredFinalProgrammingMode;
        if (desiredFinalProgrammingMode == Toggle::Disable) {
            assign.planSteps[assign.planStepCount++] = PlanStep{PropertyWritePlanStep{
                programmingModePropertyTarget_(),
                assign.progModeDisableData,
                assign.progModeDisableResponseBuffer,
                &assign.progModeDisableConfirmation,
                assign.options.timeoutMs,
            }};
        }
    }

    Workflow* workflow_{nullptr};
    ProcedureStatus status_{};
    Operation operation_{};
};

template <typename Workflow>
DeviceManagementProcedures<Workflow> makeDeviceManagementProcedures(Workflow& workflow) noexcept
{
    return DeviceManagementProcedures<Workflow>(workflow);
}

using TunnelingDeviceManagementProcedures = DeviceManagementProcedures<TunnelingDeviceManagementSession>;

#if KNX_SECURE_ENABLED
using SecureTunnelingDeviceManagementProcedures = DeviceManagementProcedures<SecureTunnelingDeviceManagementSession>;
#endif

} // namespace netip
} // namespace knx
