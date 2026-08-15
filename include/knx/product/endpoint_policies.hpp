// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_policies.hpp
 * @brief Canonical endpoint policy helpers built on typed ports and EndpointRuntime.
 */

#pragma once

#include "knx/application/group_object.hpp"
#include "knx/product/endpoint_definition.hpp"
#include "knx/product/endpoint_runtime.hpp"
#include "knx/util/inplace_function.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace knx::product::endpoint {

namespace detail {

template <typename ValueType, typename ThresholdType>
bool defaultPolicyChangedFn(const ValueType& current,
                            const ValueType& last,
                            ThresholdType threshold)
{
    if constexpr (std::is_arithmetic_v<ValueType>) {
        const auto diff = current >= last ? (current - last) : (last - current);
        return static_cast<ThresholdType>(diff) >= threshold;
    } else if constexpr (std::equality_comparable<ValueType>) {
        (void)threshold;
        return current != last;
    } else {
        (void)current;
        (void)last;
        (void)threshold;
        return true;
    }
}

} // namespace detail

template <typename DefinitionT, auto LogicalId>
class PeriodicPublishPolicy {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_spec = port_spec_t<definition_type, LogicalId>;
    using value_type = port_value_t<definition_type, LogicalId>;
    using runtime_type = EndpointRuntime<DefinitionT>;
    using ValueFn = util::InplaceFunction<value_type(), 64>;

    static_assert(port_spec::transmit,
                  "PeriodicPublishPolicy requires a transmitting endpoint port.");

    PeriodicPublishPolicy(uint32_t intervalMs, ValueFn valueFn)
        : intervalMs_(intervalMs)
        , valueFn_(std::move(valueFn))
    {
    }

    util::Result<void> tick(uint32_t nowMs, runtime_type& runtime)
    {
        if (!initialized_) {
            lastPublishedMs_ = nowMs;
            initialized_ = true;
            return util::Result<void>::ok();
        }

        if ((nowMs - lastPublishedMs_) < intervalMs_) {
            return util::Result<void>::ok();
        }

        return publishNow(nowMs, runtime);
    }

    util::Result<void> publishNow(uint32_t nowMs, runtime_type& runtime)
    {
        const auto result = runtime.template publish<LogicalId>(valueFn_());
        if (result.isOk()) {
            lastPublishedMs_ = nowMs;
            initialized_ = true;
        }

        return result;
    }

    void setInterval(uint32_t intervalMs) { intervalMs_ = intervalMs; }
    uint32_t interval() const { return intervalMs_; }

private:
    uint32_t intervalMs_{0u};
    ValueFn valueFn_{};
    uint32_t lastPublishedMs_{0u};
    bool initialized_{false};
};

template <typename DefinitionT,
          auto LogicalId,
          typename ThresholdType = port_value_t<std::remove_cvref_t<DefinitionT>, LogicalId>>
class OnChangePolicy {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_spec = port_spec_t<definition_type, LogicalId>;
    using value_type = port_value_t<definition_type, LogicalId>;
    using runtime_type = EndpointRuntime<DefinitionT>;
    using ValueFn = util::InplaceFunction<value_type(), 64>;
    using ChangedFn = util::InplaceFunction<bool(const value_type&, const value_type&, ThresholdType), 64>;

    static_assert(port_spec::transmit,
                  "OnChangePolicy requires a transmitting endpoint port.");

    static bool defaultChangedFn(const value_type& current,
                                 const value_type& last,
                                 ThresholdType threshold)
    {
        return detail::defaultPolicyChangedFn(current, last, threshold);
    }

    OnChangePolicy(ThresholdType threshold,
                   ValueFn valueFn,
                   ChangedFn changedFn = defaultChangedFn)
        : threshold_(threshold)
        , valueFn_(std::move(valueFn))
        , changedFn_(std::move(changedFn))
    {
    }

    util::Result<void> tick(uint32_t nowMs, runtime_type& runtime)
    {
        (void)nowMs;

        const value_type current = valueFn_();
        if (!hasLastValue_ || changedFn_(current, lastValue_, threshold_)) {
            const auto result = runtime.template publish<LogicalId>(current);
            if (result.isOk()) {
                lastValue_ = current;
                hasLastValue_ = true;
            }
            return result;
        }

        return util::Result<void>::ok();
    }

    void reset() { hasLastValue_ = false; }
    void setThreshold(ThresholdType threshold) { threshold_ = threshold; }
    ThresholdType threshold() const { return threshold_; }

private:
    ThresholdType threshold_{};
    ValueFn valueFn_{};
    ChangedFn changedFn_{};
    bool hasLastValue_{false};
    value_type lastValue_{};
};

template <typename DefinitionT,
          auto LogicalId,
          typename ThresholdType = port_value_t<std::remove_cvref_t<DefinitionT>, LogicalId>>
class CombinedSensorPolicy {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_spec = port_spec_t<definition_type, LogicalId>;
    using value_type = port_value_t<definition_type, LogicalId>;
    using runtime_type = EndpointRuntime<DefinitionT>;
    using ValueFn = util::InplaceFunction<value_type(), 64>;
    using ChangedFn = util::InplaceFunction<bool(const value_type&, const value_type&, ThresholdType), 64>;

    static_assert(port_spec::transmit,
                  "CombinedSensorPolicy requires a transmitting endpoint port.");

    static bool defaultChangedFn(const value_type& current,
                                 const value_type& last,
                                 ThresholdType threshold)
    {
        return detail::defaultPolicyChangedFn(current, last, threshold);
    }

    CombinedSensorPolicy(uint32_t maxIntervalMs,
                         ThresholdType changeThreshold,
                         ValueFn valueFn,
                         ChangedFn changedFn = defaultChangedFn)
        : maxIntervalMs_(maxIntervalMs)
        , threshold_(changeThreshold)
        , valueFn_(std::move(valueFn))
        , changedFn_(std::move(changedFn))
    {
    }

    util::Result<void> tick(uint32_t nowMs, runtime_type& runtime)
    {
        const value_type current = valueFn_();

        if (!hasLastPublished_) {
            return publishStamped(nowMs, runtime, current);
        }

        const bool intervalElapsed = (nowMs - lastPublishedMs_) >= maxIntervalMs_;
        const bool valueChanged = changedFn_(current, lastPublishedValue_, threshold_);
        if (!intervalElapsed && !valueChanged) {
            return util::Result<void>::ok();
        }

        return publishStamped(nowMs, runtime, current);
    }

    util::Result<void> reset(uint32_t nowMs, runtime_type& runtime)
    {
        hasLastPublished_ = false;
        return publishStamped(nowMs, runtime, valueFn_());
    }

    void setInterval(uint32_t maxIntervalMs) { maxIntervalMs_ = maxIntervalMs; }
    uint32_t interval() const { return maxIntervalMs_; }
    void setThreshold(ThresholdType threshold) { threshold_ = threshold; }
    ThresholdType threshold() const { return threshold_; }

private:
    util::Result<void> publishStamped(uint32_t nowMs,
                                      runtime_type& runtime,
                                      const value_type& value)
    {
        const auto result = runtime.template publish<LogicalId>(value);
        if (result.isOk()) {
            lastPublishedValue_ = value;
            lastPublishedMs_ = nowMs;
            hasLastPublished_ = true;
        }

        return result;
    }

    uint32_t maxIntervalMs_{0u};
    ThresholdType threshold_{};
    ValueFn valueFn_{};
    ChangedFn changedFn_{};
    uint32_t lastPublishedMs_{0u};
    bool hasLastPublished_{false};
    value_type lastPublishedValue_{};
};

/**
 * @brief Assembles a GroupObjectTransmitPolicy from individually-arriving ETS
 *        parameter values and pushes it to a runtime port.
 *
 * The transmit-policy fields (send-on-change threshold, cyclic interval, min
 * interval) usually map to separate ETS parameters, each delivered through its
 * own `onParameterChanged` callback — one value at a time. This binder holds the
 * aggregate policy so each callback updates one field, and re-applies the whole
 * policy on every change via a caller-supplied apply function.
 *
 * The binder is owner-managed (typically a `static`/long-lived object) so the
 * `onParameterChanged` closures can capture it by reference safely. `onParameterChanged`
 * is bound before the runtime exists, so wire the apply function *after* start()
 * with `bindApply(...)` — that also flushes any values already delivered from
 * persistence during start.
 *
 * @thread_safety Owner-context only (drive from the same context as loop()).
 *
 * Example:
 * @code
 *   static endpoint::TransmitPolicyBinder tempPolicy;
 *   auto bindings = makeCommissionedBindings(kProduct)
 *       .onParameterChanged<Param::TempThresholdCenti>([&](uint16_t c) {
 *           tempPolicy.setChangeThreshold(c / 100.0);   // 0.01 °C units → °C
 *       })
 *       .onParameterChanged<Param::TempCyclicSec>([&](uint16_t s) {
 *           tempPolicy.setCyclicIntervalMs(endpoint::secondsToMs(s));
 *       });
 *   // ... start ...
 *   tempPolicy.bindApply([&](const application::GroupObjectTransmitPolicy& p) {
 *       (void)product.setTransmitPolicy<Port::Temperature>(p);
 *   });
 * @endcode
 */
class TransmitPolicyBinder {
public:
    using ApplyFn = util::InplaceFunction<void(const application::GroupObjectTransmitPolicy&), 64>;

    /// Attach the function that pushes the policy to a runtime port, and apply
    /// the current (possibly persistence-restored) policy immediately.
    void bindApply(ApplyFn apply) {
        apply_ = std::move(apply);
        applyNow();
    }

    void setOnChangeEnabled(bool enabled) { policy_.onChangeEnabled = enabled; applyNow(); }
    void setChangeThreshold(double threshold) {
        policy_.changeThreshold = threshold;
        policy_.onChangeEnabled = true;  // a threshold is only meaningful with on-change
        applyNow();
    }
    void setCyclicIntervalMs(uint32_t ms) { policy_.cyclicIntervalMs = ms; applyNow(); }
    void setMinIntervalMs(uint32_t ms) { policy_.minIntervalMs = ms; applyNow(); }

    void setPolicy(const application::GroupObjectTransmitPolicy& policy) { policy_ = policy; applyNow(); }
    const application::GroupObjectTransmitPolicy& policy() const noexcept { return policy_; }

private:
    void applyNow() {
        if (apply_) {
            apply_(policy_);
        }
    }

    application::GroupObjectTransmitPolicy policy_{};
    ApplyFn apply_{};
};

/// Convenience for ETS interval parameters, which are conventionally in seconds.
constexpr uint32_t secondsToMs(uint32_t seconds) noexcept { return seconds * 1000u; }

} // namespace knx::product::endpoint