// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/device_management_commissioner.hpp"

#include <iostream>

int main()
{
    std::cout << "Device-management commissioning workflow example (conceptual)\n";

    // NOTE: This example is conceptual. A real call would pass a concrete
    // platform::NetworkInterface plus the gateway host/port.
    knx::netip::TunnelingDeviceManagementCommissioner commissioner;
    knx::platform::NetworkInterface* network = nullptr;
    const auto host = knx::IpAddress::fromOctets(192, 168, 1, 10);
    const auto port = knx::NetIpPort(3671);

    if (network == nullptr) {
        std::cout << "Provide a concrete NetworkInterface before running the commissioning flow.\n";
        return 0;
    }

    auto programmingModeResult = commissioner.openAndReadProgrammingMode(*network, host, port);
    if (programmingModeResult.isError()) {
        std::cout << "Unable to read programming mode through the commissioner.\n";
        return 1;
    }

    std::cout << "Programming mode before commissioning: "
              << (programmingModeResult.value() == knx::Toggle::Enable ? "enabled" : "disabled")
              << "\n";

    auto beginResult = commissioner.beginOpenAndCommissionIndividualAddress(
        *network,
        host,
        port,
        knx::IndividualAddress(0x1121));
    if (beginResult.isError()) {
        std::cout << "Session is not ready for the management procedure yet.\n";
        return 1;
    }

    while (true) {
        knx::netip::TunnelingDeviceManagementCommissioner::CommissionIndividualAddressResult result{};
        auto progress = commissioner.pollOpenAndCommissionIndividualAddress(result);
        if (progress.isError()) {
            std::cout << "Management procedure failed.\n";
            return 1;
        }
        if (progress.value() == knx::util::OperationProgressState::Success) {
            std::cout << "Commissioned device from 0x"
                      << std::hex << result.identityBefore.individualAddress.raw
                      << " to 0x" << result.identityAfter.individualAddress.raw
                      << std::dec << ".\n";
            break;
        }
        if (progress.value() == knx::util::OperationProgressState::Timeout) {
            std::cout << "Management procedure timed out.\n";
            return 1;
        }
    }

    std::cout << "The same commissioner also owns typed programming-mode and direct address-assignment procedures.\n";

    return 0;
}
