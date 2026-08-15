// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/device_management_codec.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace netip {

struct DeviceManagementConnectionProvider {
    using AcquireFn = util::Result<device_management::ConnectionHeader> (*)(void* context);

    void* context{nullptr};
    AcquireFn acquire{nullptr};

    constexpr bool isBound() const noexcept
    {
        return context != nullptr && acquire != nullptr;
    }

    util::Result<device_management::ConnectionHeader> acquireHeader() const
    {
        if (!isBound()) return util::ErrorCode::NotInitialized;
        return acquire(context);
    }

    template <typename Session,
              util::Result<device_management::ConnectionHeader> (Session::*AcquireMethod)()>
    static constexpr DeviceManagementConnectionProvider from(Session& session) noexcept
    {
        return DeviceManagementConnectionProvider{&session, &invoke<Session, AcquireMethod>};
    }

private:
    template <typename Session,
              util::Result<device_management::ConnectionHeader> (Session::*AcquireMethod)()>
    static util::Result<device_management::ConnectionHeader> invoke(void* context)
    {
        if (!context) return util::ErrorCode::InvalidParameter;
        return (static_cast<Session*>(context)->*AcquireMethod)();
    }
};

} // namespace netip
} // namespace knx