// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/product/commissioned_product.hpp"

#include <filesystem>
#include <memory>
#include <string>

using namespace knx;
using namespace knx::product;

namespace {

enum class CommissionedSurfacePort : uint16_t {
    RelayCommand = 7,
    RelayState = 11,
};

enum class CommissionedSurfaceParameter : uint16_t {
    DefaultRelayState = 5,
};

constexpr auto kCommissionedSurfaceProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<CommissionedSurfacePort,
                               semantics::SwitchCommand<CommissionedSurfacePort::RelayCommand,
                                                        "relay_command">,
                               semantics::SwitchState<CommissionedSurfacePort::RelayState,
                                                      "relay_state">>(
            ProductIdentity{
                .productKey = "commissioned_surface",
                .productDisplayName = "Commissioned Surface",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::TP1,
                .applicationNumber = 17,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = knx::config::MAX_APDU_LENGTH,
            },
            PersistencePolicy{
                .namespacePrefix = "commissioned_surface",
                .schemaVersion = 3,
                .persistKnxState = true,
            }),
        makeParameterSchema(
            parameter<CommissionedSurfaceParameter::DefaultRelayState>("default_relay_state", false)));

std::unique_ptr<physical::Tp1MediumBackend> makeNullBackend()
{
    return std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend());
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_product_commissioned_surface_uses_public_commissioning_apis_by_default(void)
{
    platform::LinuxPlatform platform;
    bool relayState = false;
    bool faultSeen = false;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct)
            .onCommand<CommissionedSurfacePort::RelayCommand>([&](bool on) {
                relayState = on;
            })
            .provideState<CommissionedSurfacePort::RelayState>([&]() {
                return relayState;
            })
            .onFault([&](FaultInfo info) {
                faultSeen = (info.code == FaultCode::InternalError)
                            && info.detail != nullptr
                            && std::string(info.detail) == "commissioned";
            }),
        makeNullBackend());

    TEST_ASSERT_TRUE(result.isOk());

    auto runtime = std::move(result.value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedTransportPath::Tp1MediumBackend),
                          static_cast<int>(runtime->transportPath()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedSupportTier::ProductGrade),
                          static_cast<int>(runtime->supportTier()));
    TEST_ASSERT_TRUE(runtime->isProductGradePath());
    TEST_ASSERT_EQUAL_STRING("commissioned_surface",
                             runtime->knxPersistenceIdentity().instanceKey.c_str());
    TEST_ASSERT_EQUAL_STRING("commissioned_surface",
                             runtime->knxPersistenceIdentity().storageNamespace.c_str());
    TEST_ASSERT_EQUAL_UINT16(3u, runtime->knxPersistenceIdentity().schemaVersion);
    TEST_ASSERT_TRUE(isInitialIndividualAddress(runtime->individualAddress()));
    TEST_ASSERT_FALSE(runtime->parameters().hasCustomValue<CommissionedSurfaceParameter::DefaultRelayState>());

    TEST_ASSERT_TRUE(runtime->template applyCommissionedParameter<
                         CommissionedSurfaceParameter::DefaultRelayState>(true)
                         .isOk());
    TEST_ASSERT_TRUE(runtime->parameters().hasCustomValue<CommissionedSurfaceParameter::DefaultRelayState>());
    TEST_ASSERT_TRUE(runtime->parameters().get<CommissionedSurfaceParameter::DefaultRelayState>());
    TEST_ASSERT_TRUE(runtime->applyCommissionedGroupAddresses(
        groupAddressBinding(CommissionedSurfacePort::RelayState, GroupAddress(2, 1, 9)))
                         .isOk());
    const auto publish = runtime->publish(CommissionedSurfacePort::RelayState, true);
    TEST_ASSERT_TRUE(publish.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::OperationNotReady),
                          static_cast<int>(publish.error()));
    TEST_ASSERT_EQUAL_UINT(0u, runtime->pendingBusActionCount());
    TEST_ASSERT_NULL(runtime->pendingBusActionAt(0u));

    runtime->reportFault({FaultCode::InternalError, "commissioned"});
    TEST_ASSERT_TRUE(faultSeen);
}

void test_product_commissioned_surface_honors_storage_namespace_override(void)
{
    platform::LinuxPlatform platform;
    bool relayState = true;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct)
            .onCommand<CommissionedSurfacePort::RelayCommand>([&](bool on) {
                relayState = on;
            })
            .provideState<CommissionedSurfacePort::RelayState>([&]() {
                return relayState;
            }),
        CommissionedProductOptions{
            .persistence = {
                .instanceKey = "beta",
                .storageNamespace = "custom_commissioned_surface_beta",
            },
            .standaloneDemoIndividualAddress = IndividualAddress(1, 1, 43),
            .mediumBackend = makeNullBackend(),
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_TRUE(result.isOk());

    auto runtime = std::move(result.value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedTransportPath::Tp1MediumBackend),
                          static_cast<int>(runtime->transportPath()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedSupportTier::ProductGrade),
                          static_cast<int>(runtime->supportTier()));
    TEST_ASSERT_TRUE(runtime->isProductGradePath());
    TEST_ASSERT_EQUAL_STRING("beta", runtime->knxPersistenceIdentity().instanceKey.c_str());
    TEST_ASSERT_EQUAL_STRING("custom_commissioned_surface_beta",
                             runtime->knxPersistenceIdentity().storageNamespace.c_str());
    TEST_ASSERT_EQUAL_UINT16(IndividualAddress(1, 1, 43).raw, runtime->individualAddress().raw);
    TEST_ASSERT_TRUE(runtime->applyCommissionedGroupAddress(CommissionedSurfacePort::RelayState,
                                                           GroupAddress(2, 1, 10))
                         .isOk());
    TEST_ASSERT_TRUE(runtime->publish(CommissionedSurfacePort::RelayState, true).isOk());
    const std::string namespaceView(runtime->knxPersistenceNamespace());
    TEST_ASSERT_EQUAL_STRING("custom_commissioned_surface_beta", namespaceView.c_str());
}

void test_product_commissioned_surface_starts_via_tp1_mac_physical(void)
{
    platform::LinuxPlatform platform;

    auto physical = std::make_unique<physical::Tp1MacPhysical>(
        std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend()));

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        std::move(physical));

    // NullTp1MediumBackend always succeeds so Tp1MacPhysical init should succeed too.
    TEST_ASSERT_TRUE(result.isOk());

    auto runtime = std::move(result.value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedTransportPath::Tp1MacPhysical),
                          static_cast<int>(runtime->transportPath()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CommissionedSupportTier::AdvancedManual),
                          static_cast<int>(runtime->supportTier()));
    TEST_ASSERT_FALSE(runtime->isProductGradePath());
    TEST_ASSERT_TRUE(isInitialIndividualAddress(runtime->individualAddress()));
}

void test_product_commissioned_serial_number_comes_from_the_platform(void)
{
    // PID_SERIAL_NUMBER: manufacturer id, then the platform's unique number.
    platform::LinuxPlatform platform;
    const uint32_t unique = platform.uniqueSerialNumber();
    TEST_ASSERT_NOT_EQUAL(0u, unique);

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        makeNullBackend());
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    const auto serial = runtime->deviceObject().getSerialNumber();
    const uint8_t expected[6] = {
        0x00, 0xFA,
        static_cast<uint8_t>(unique >> 24), static_cast<uint8_t>(unique >> 16),
        static_cast<uint8_t>(unique >> 8), static_cast<uint8_t>(unique),
    };
    TEST_ASSERT_EQUAL_UINT32(6u, serial.size());
    TEST_ASSERT_EQUAL_MEMORY(expected, serial.data(), 6);
}

void test_product_commissioned_surface_forwards_owner_work_hint_and_notification(void)
{
    using runtime_type = CommissionedProductRuntime<decltype(kCommissionedSurfaceProduct)>;

    bool relayState = true;
    unsigned int workAvailableCount = 0u;

    auto endpointBindings = endpoint::makeEndpointBindings(kCommissionedSurfaceProduct.endpointDefinition)
                              .provideState<CommissionedSurfacePort::RelayState>([&]() {
                                  return relayState;
                              });

    runtime_type runtime(
        kCommissionedSurfaceProduct,
        std::move(endpointBindings),
        detail::ParameterCallbacks<typename runtime_type::parameter_schema_type>{},
        {},
        CommissionedProductOptions{
            .standaloneDemoIndividualAddress = IndividualAddress(1, 1, 44),
            .restoreKnxStateOnBoot = false,
        });

    runtime.setWorkAvailableCallback([&workAvailableCount]() {
        ++workAvailableCount;
    });

    auto hint = runtime.ownerWorkHint();
    TEST_ASSERT_FALSE(hint.hasImmediateWork());
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingDeferredWorkItems);

    TEST_ASSERT_TRUE(runtime.publish(CommissionedSurfacePort::RelayState, relayState).isOk());

    hint = runtime.ownerWorkHint();
    TEST_ASSERT_TRUE(hint.hasImmediateWork());
    TEST_ASSERT_TRUE(hint.shouldCallLoop());
    TEST_ASSERT_EQUAL_UINT(1u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingDeferredWorkItems);
    TEST_ASSERT_EQUAL_UINT(1u, workAvailableCount);
    TEST_ASSERT_EQUAL_UINT(1u, runtime.pendingBusActionCount());
}

void test_product_commissioned_export_descriptor_includes_parameters(void)
{
    constexpr auto exportDesc = makeCommissionedExportDescriptor(kCommissionedSurfaceProduct);

    // Two communication objects defined in the fixture
    TEST_ASSERT_EQUAL_UINT(2u, exportDesc.kCommunicationObjectCount);

    // One parameter defined in the fixture
    TEST_ASSERT_EQUAL_UINT(1u, exportDesc.kParameterCount);

    const auto& param = exportDesc.parameters[0];
    TEST_ASSERT_EQUAL_STRING("default_relay_state", param.key.data());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExportParameterValueKind::Boolean),
                          static_cast<int>(param.valueKind));
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(CommissionedSurfaceParameter::DefaultRelayState),
                             param.id);
}

void test_product_commissioned_security_off_by_default(void)
{
    // Default options must leave security disabled — no accidental secure mode.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        makeNullBackend());

    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    TEST_ASSERT_FALSE(runtime->isSecurityEnabled());
    TEST_ASSERT_FALSE(runtime->hasEtsToolKey());
}

void test_product_commissioned_secure_options_wires_tool_key(void)
{
    // A factory Tool Key with Security Mode on at Factory Reset: both in
    // force on a fresh device.
    platform::LinuxPlatform platform;

    const std::array<uint8_t, 16> kToolKey = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    };

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        CommissionedProductOptions{
            .mediumBackend = makeNullBackend(),
            .secure = SecureCommissioningOptions{
                .factoryToolKey = kToolKey,
                .securityModeAtFactoryReset = true,
            },
        });

    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    TEST_ASSERT_TRUE(runtime->isSecurityEnabled());
    TEST_ASSERT_TRUE(runtime->hasEtsToolKey());
}

void test_product_commissioned_secure_options_enabled_no_key(void)
{
    // Security Mode without a factory Tool Key leaves the key unset — valid
    // for devices that receive keys via ETS later.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        CommissionedProductOptions{
            .mediumBackend = makeNullBackend(),
            .secure = SecureCommissioningOptions{
                .securityModeAtFactoryReset = true,
            },
        });

    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    TEST_ASSERT_TRUE(runtime->isSecurityEnabled());
    TEST_ASSERT_FALSE(runtime->hasEtsToolKey());
}

void test_product_commissioned_apply_ets_tool_key_post_start(void)
{
    // applyEtsToolKey() must enable security mode and store the key after start.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        makeNullBackend());

    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    TEST_ASSERT_FALSE(runtime->isSecurityEnabled());
    TEST_ASSERT_FALSE(runtime->hasEtsToolKey());

    const std::array<uint8_t, 16> kKey = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    };

    TEST_ASSERT_TRUE(runtime->applyEtsToolKey(kKey).isOk());
    TEST_ASSERT_TRUE(runtime->isSecurityEnabled());
    TEST_ASSERT_TRUE(runtime->hasEtsToolKey());
}

void test_product_commissioned_apply_ets_group_key_post_start(void)
{
    // applyEtsGroupKey() must not crash and return ok.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        makeNullBackend());

    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    const std::array<uint8_t, 16> kGroupKey = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    };

    const auto r = runtime->applyEtsGroupKey(GroupAddress(1, 2, 3), kGroupKey);
    TEST_ASSERT_TRUE(r.isOk());
}

void test_product_commissioned_secure_options_yield_to_the_stored_tool_key(void)
{
    // SecureCommissioningOptions are the device's factory defaults: what ETS
    // downloaded and the device stored comes back over them at every start.
    // Applied after start(), as they used to be, they put the device back on
    // its factory key and Security Mode behind ETS's back on every boot.
    const std::array<uint8_t, 16> kFactoryKey = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    };
    const std::array<uint8_t, 16> kEtsKey = {
        0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
        0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C,
    };
    const std::string kNamespace = "secure_defaults_surface";
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir(kNamespace), ec);

    {
        // A device ETS commissioned with Security Mode left off and its own
        // Tool Key, as the store holds it after the download.
        platform::LinuxPlatform platform;
        bau::BusAccessUnit device(platform, makeNullBackend());
        TEST_ASSERT_TRUE(device.init(IndividualAddress(1, 1, 45), kNamespace, 3).isOk());
        TEST_ASSERT_TRUE(device.writeProperty(
            InterfaceObjectType::security(), InterfaceObjectInstance(1),
            static_cast<application::PropertyID>(objects::SecurityProperty::ToolKey), 1,
            std::span<const uint8_t>(kEtsKey)).isOk());
        device.flushPendingPersistence();
        device.close();
    }

    platform::LinuxPlatform platform;
    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        CommissionedProductOptions{
            .persistence = {.storageNamespace = kNamespace},
            .mediumBackend = makeNullBackend(),
            .secure = SecureCommissioningOptions{
                .factoryToolKey = kFactoryKey,
                .securityModeAtFactoryReset = true,
            },
        });
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    TEST_ASSERT_TRUE(runtime->toolKeyInForceIs(kEtsKey));
    TEST_ASSERT_FALSE(runtime->toolKeyInForceIs(kFactoryKey));
    TEST_ASSERT_FALSE(runtime->isSecurityEnabled());

    runtime.reset();
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir(kNamespace), ec);
}

// What a Data Secure device ships as: the FDSK installed, Security Mode off
// until ETS turns it on. Three moments of its life, with the same options at
// every start: fresh, after an ETS secure download, after a Factory Reset.
namespace {

constexpr std::array<uint8_t, 16> kFdsk = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
};
constexpr std::array<uint8_t, 16> kDownloadedToolKey = {
    0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
    0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C,
};
const std::string kFdskNamespace = "secure_fdsk_surface";

auto startFdskProduct(platform::LinuxPlatform& platform)
{
    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        CommissionedProductOptions{
            .persistence = {.storageNamespace = kFdskNamespace},
            .mediumBackend = makeNullBackend(),
            .secure = SecureCommissioningOptions{.factoryToolKey = kFdsk},
        });
    TEST_ASSERT_TRUE(result.isOk());
    return std::move(result.value());
}

void eraseFdskStore()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir(kFdskNamespace), ec);
}

} // namespace

void test_product_fdsk_fresh_device_has_the_fdsk_and_mode_off(void)
{
    eraseFdskStore();
    {
        platform::LinuxPlatform platform;
        auto runtime = startFdskProduct(platform);
        TEST_ASSERT_TRUE(runtime->toolKeyInForceIs(kFdsk));
        TEST_ASSERT_FALSE(runtime->isSecurityEnabled());
    }
    eraseFdskStore();
}

void test_product_fdsk_yields_to_the_ets_download_across_restarts(void)
{
    eraseFdskStore();
    {
        platform::LinuxPlatform platform;
        auto runtime = startFdskProduct(platform);
        TEST_ASSERT_TRUE(runtime->toolKeyInForceIs(kFdsk));
    }
    {
        // The end of an ETS secure download, as it lands in the store: ETS's
        // own Tool Key and Security Mode on.
        platform::LinuxPlatform platform;
        bau::BusAccessUnit device(platform, makeNullBackend());
        TEST_ASSERT_TRUE(device.init(IndividualAddress(1, 1, 46), kFdskNamespace, 3).isOk());
        device.securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        TEST_ASSERT_TRUE(device.writeProperty(
            InterfaceObjectType::security(), InterfaceObjectInstance(1),
            static_cast<application::PropertyID>(objects::SecurityProperty::ToolKey), 1,
            std::span<const uint8_t>(kDownloadedToolKey)).isOk());
        device.flushPendingPersistence();
        device.close();
    }
    for (int boot = 0; boot < 2; ++boot) {
        platform::LinuxPlatform platform;
        auto runtime = startFdskProduct(platform);
        TEST_ASSERT_TRUE(runtime->toolKeyInForceIs(kDownloadedToolKey));
        TEST_ASSERT_FALSE(runtime->toolKeyInForceIs(kFdsk));
        TEST_ASSERT_TRUE(runtime->isSecurityEnabled());
    }
    eraseFdskStore();
}

void test_product_fdsk_returns_after_a_factory_reset(void)
{
    eraseFdskStore();
    {
        platform::LinuxPlatform platform;
        bau::BusAccessUnit device(platform, makeNullBackend());
        TEST_ASSERT_TRUE(device.init(IndividualAddress(1, 1, 46), kFdskNamespace, 3).isOk());
        device.securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        TEST_ASSERT_TRUE(device.writeProperty(
            InterfaceObjectType::security(), InterfaceObjectInstance(1),
            static_cast<application::PropertyID>(objects::SecurityProperty::ToolKey), 1,
            std::span<const uint8_t>(kDownloadedToolKey)).isOk());
        device.flushPendingPersistence();
        device.close();
    }
    // A local Factory Reset erases the stored state (Habinari's
    // control_service_factory_reset); a KNX Master Reset 02h leaves the
    // factory state stored instead, covered in test_ets_connected_session.
    eraseFdskStore();
    {
        platform::LinuxPlatform platform;
        auto runtime = startFdskProduct(platform);
        TEST_ASSERT_TRUE(runtime->toolKeyInForceIs(kFdsk));
        TEST_ASSERT_FALSE(runtime->isSecurityEnabled());
    }
    eraseFdskStore();
}

void test_product_commissioned_security_helpers_refuse_before_start(void)
{
    // They act on the started device. Before start() they used to return ok
    // having done nothing; the values a device starts with are
    // SecureCommissioningOptions.
    using runtime_type = CommissionedProductRuntime<decltype(kCommissionedSurfaceProduct)>;
    auto endpointBindings = endpoint::makeEndpointBindings(kCommissionedSurfaceProduct.endpointDefinition);
    runtime_type runtime(
        kCommissionedSurfaceProduct,
        std::move(endpointBindings),
        detail::ParameterCallbacks<typename runtime_type::parameter_schema_type>{},
        {},
        CommissionedProductOptions{});

    const std::array<uint8_t, 16> kKey{};
    const auto notReady = static_cast<int>(util::ErrorCode::OperationNotReady);
    TEST_ASSERT_EQUAL_INT(notReady, static_cast<int>(runtime.applyEtsToolKey(kKey).error()));
    TEST_ASSERT_EQUAL_INT(notReady, static_cast<int>(runtime.installFactoryToolKey(kKey).error()));
    TEST_ASSERT_EQUAL_INT(notReady, static_cast<int>(runtime.enableSecurityMode().error()));
    TEST_ASSERT_EQUAL_INT(notReady,
                          static_cast<int>(runtime.applyEtsGroupKey(GroupAddress(1, 2, 3), kKey).error()));
}

// ──────────────────────────────────────────────────────────────────────────────
// IP managed transport path tests
// ──────────────────────────────────────────────────────────────────────────────

#if KNX_IP_TUNNELING
void test_product_commissioned_ip_tunneling_zero_host_returns_invalid_parameter(void)
{
    // IpTunnelingOptions with a zero host must be rejected before any network
    // activity — safe to run without an actual gateway.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        IpTunnelingOptions{
            .host = IpAddress(0),
        });

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidParameter),
                          static_cast<int>(result.error()));
}
#endif  // KNX_IP_TUNNELING

void test_product_commissioned_ip_routing_zero_multicast_returns_invalid_parameter(void)
{
    // IpRoutingOptions with a zero multicast group must be rejected before
    // any network activity.
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        IpRoutingOptions{
            .multicastGroup = IpAddress(0),
        });

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidParameter),
                          static_cast<int>(result.error()));
}

#if KNX_IP_TUNNELING
void test_product_commissioned_ip_tunneling_managed_sets_functional_tier(void)
{
    // When IpTunnelingOptions passes validation (non-zero host), the runtime
    // starts with IpTunnelingManaged path and Functional support tier.
    // The BAU may fail to connect on the test host; that is acceptable —
    // this test only verifies the pre-connection validation and error path.
    platform::LinuxPlatform platform;

    // Use a non-routable address (TEST-NET-1, RFC 5737) so the socket open
    // attempt fails quickly without blocking or modifying live network state.
    const IpAddress kTestGateway = IpAddress::fromString("192.0.2.1");

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        IpTunnelingOptions{
            .host = kTestGateway,
            .port = NetIpPort(3671),
        });

    // The result is either ok (if the platform socket layer succeeds without
    // blocking) or an error other than InvalidParameter (validation passed).
    if (result.isError()) {
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(util::ErrorCode::InvalidParameter),
                              static_cast<int>(result.error()));
    } else {
        auto runtime = std::move(result.value());
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CommissionedTransportPath::IpTunnelingManaged),
            static_cast<int>(runtime->transportPath()));
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CommissionedSupportTier::Functional),
            static_cast<int>(runtime->supportTier()));
        TEST_ASSERT_FALSE(runtime->isProductGradePath());
    }
}
#endif  // KNX_IP_TUNNELING

void test_product_commissioned_ip_routing_managed_sets_functional_tier(void)
{
    // When IpRoutingOptions passes validation (non-zero multicast group), the
    // runtime starts with IpRoutingManaged path and Functional support tier.
    platform::LinuxPlatform platform;

    // Use a valid KNX multicast group.  The routing socket may fail to join
    // on the test host; that is acceptable.
    const IpAddress kKnxMulticast = IpAddress::fromString("224.0.23.12");

    auto result = startCommissionedProduct(
        platform,
        kCommissionedSurfaceProduct,
        makeCommissionedBindings(kCommissionedSurfaceProduct),
        IpRoutingOptions{
            .multicastGroup = kKnxMulticast,
            .port = NetIpPort(3671),
        });

    if (result.isError()) {
        TEST_ASSERT_NOT_EQUAL(static_cast<int>(util::ErrorCode::InvalidParameter),
                              static_cast<int>(result.error()));
    } else {
        auto runtime = std::move(result.value());
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CommissionedTransportPath::IpRoutingManaged),
            static_cast<int>(runtime->transportPath()));
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(CommissionedSupportTier::Functional),
            static_cast<int>(runtime->supportTier()));
        TEST_ASSERT_FALSE(runtime->isProductGradePath());
    }
}

void test_product_commissioned_support_tier_values(void)
{
    // Verify tier classification for all transport path enum values.
    using TP = CommissionedTransportPath;
    using ST = CommissionedSupportTier;

    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::ProductGrade),
                          static_cast<int>(supportTierForTransportPath(TP::Tp1MediumBackend)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::Functional),
                          static_cast<int>(supportTierForTransportPath(TP::IpTunnelingManaged)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::Functional),
                          static_cast<int>(supportTierForTransportPath(TP::IpRoutingManaged)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::AdvancedManual),
                          static_cast<int>(supportTierForTransportPath(TP::Tp1MacPhysical)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::AdvancedManual),
                          static_cast<int>(supportTierForTransportPath(TP::IpTunneling)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::AdvancedManual),
                          static_cast<int>(supportTierForTransportPath(TP::IpRouting)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ST::AdvancedManual),
                          static_cast<int>(supportTierForTransportPath(TP::NotStarted)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_commissioned_surface_uses_public_commissioning_apis_by_default);
    RUN_TEST(test_product_commissioned_surface_honors_storage_namespace_override);
    RUN_TEST(test_product_commissioned_surface_starts_via_tp1_mac_physical);
    RUN_TEST(test_product_commissioned_serial_number_comes_from_the_platform);
    RUN_TEST(test_product_commissioned_surface_forwards_owner_work_hint_and_notification);
    RUN_TEST(test_product_commissioned_export_descriptor_includes_parameters);
    RUN_TEST(test_product_commissioned_security_off_by_default);
    RUN_TEST(test_product_commissioned_secure_options_wires_tool_key);
    RUN_TEST(test_product_commissioned_secure_options_enabled_no_key);
    RUN_TEST(test_product_commissioned_apply_ets_tool_key_post_start);
    RUN_TEST(test_product_commissioned_apply_ets_group_key_post_start);
    RUN_TEST(test_product_commissioned_secure_options_yield_to_the_stored_tool_key);
    RUN_TEST(test_product_fdsk_fresh_device_has_the_fdsk_and_mode_off);
    RUN_TEST(test_product_fdsk_yields_to_the_ets_download_across_restarts);
    RUN_TEST(test_product_fdsk_returns_after_a_factory_reset);
    RUN_TEST(test_product_commissioned_security_helpers_refuse_before_start);
    RUN_TEST(test_product_commissioned_support_tier_values);
#if KNX_IP_TUNNELING
    RUN_TEST(test_product_commissioned_ip_tunneling_zero_host_returns_invalid_parameter);
#endif
    RUN_TEST(test_product_commissioned_ip_routing_zero_multicast_returns_invalid_parameter);
#if KNX_IP_TUNNELING
    RUN_TEST(test_product_commissioned_ip_tunneling_managed_sets_functional_tier);
#endif
    RUN_TEST(test_product_commissioned_ip_routing_managed_sets_functional_tier);
    return UNITY_END();
}