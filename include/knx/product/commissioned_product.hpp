// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file commissioned_product.hpp
 * @brief High-level developer surface for KNX endpoint products: bindings builder, parameter
 *        management, and transport-independent runtime start.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/objects/object_persistence.hpp"
#if KNX_FEATURE_NETIP
#include "knx/netip/ip_secure/mc_timer_store.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#endif
#if KNX_IP_TUNNELING
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/platform/platform.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/product/endpoint.hpp"
#include "knx/product/parameter_layout.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/log.hpp"
#if KNX_FEATURE_COUPLER
#include "knx/network/two_port_coupler.hpp"
#endif
#include "knx/util/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace knx::product {

// ============================================================================
// HOW TO USE THIS HEADER — firmware developer quick start
// ============================================================================
//
// Most firmware targets need exactly one call:
//
//   auto runtime = knx::product::startCommissionedProduct(
//       myProductDefinition,
//       std::move(options));
//
// That call returns a CommissionedProductHandle<> — a unique_ptr owning the
// CommissionedProductRuntime<> that drives the full KNX stack lifecycle.  The
// runtime is built directly on the heap because it holds the endpoint and
// parameter state inline and is far larger than an MCU task stack frame can
// carry.  See docs/reference/product_authoring_guide.md for the canonical
// step-by-step guide.
//
// Transport paths and their support tier:
//   • Tp1MediumBackend (CommissionedSupportTier::ProductGrade) — recommended
//     for all new TP1 designs.  Fully ETS-programmable and KNX-catalogued.
//   • IpTunnelingManaged / IpRoutingManaged (Functional) — stack-managed IP
//     paths; functional for deployment.
//   • Tp1MacPhysical / IpTunneling / IpRouting (AdvancedManual) — raw
//     physical pointer; for expert or legacy integration only.
//
// For test/demo use, see the `expert` namespace below.  It is NOT part of the
// normal firmware integration path.
// ============================================================================

using ProductIdentity = endpoint::ProductIdentity;
using PersistencePolicy = endpoint::PersistencePolicy;

/**
 * Explicit lifecycle states for a commissioned KNX product.
 *
 * Progression under normal operation:
 *   Uncommissioned → (ETS assigns IA) → Operational
 *   Any state       → (prog button)   → Commissioning
 *   Commissioning   → (IA assigned)   → Operational
 *
 * The firmware dev observes these states via onLifecycleChanged() and
 * CommissionedProductRuntime::lifecycleState(). Zero KNX protocol knowledge
 * is required.
 */
enum class DeviceLifecycleState : uint8_t {
    Uncommissioned, ///< No individual address assigned yet (device fresh from factory or reset).
    Commissioning,  ///< Programming mode active; ETS is writing parameters/addresses.
    Operational,    ///< Individual address assigned and KNX bus operational.
};

using endpoint::GroupAddressBinding;
using endpoint::groupAddressBinding;
using endpoint::makeEndpointDefinition;
namespace semantics = endpoint::semantics;

template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class CommissionedBindingsBuilder;

template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class CommissionedProductRuntime;

/// Owning handle to a started commissioned product.
///
/// `startCommissionedProduct()` hands back the runtime through this handle
/// rather than by value. The runtime carries the full endpoint and parameter
/// state inline and is tens of kilobytes for a product of any size — far too
/// large to sit in an MCU task's stack frame, let alone in the two frames a
/// by-value return would need it in at once. It is not movable either: its
/// callbacks capture `this`, so it is built directly in its final storage.
///
/// Nothing about the product's compile-time model changes: the runtime type is
/// still fully static in the product definition and binding capacity, and all
/// port, datapoint and parameter checking still happens at build time.
template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
using CommissionedProductHandle =
    std::unique_ptr<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>;

using LifecycleChangedCallback = util::InplaceFunction<void(DeviceLifecycleState), 32>;

namespace expert {

// ============================================================================
// EXPERT / TEST NAMESPACE — not for normal firmware integration
// ============================================================================
//
// Helpers in this namespace are intended exclusively for:
//   • Smoke tests and integration tests that need to exercise runtime
//     behavior without a live ETS session.
//   • Demo setups and developer boards that apply commissioning state
//     programmatically (e.g. hard-coded group addresses in a main.cpp).
//
// Do NOT call these from production firmware.  They bypass the KNX management
// model and do not persist state through the KNX stack.
// ============================================================================

template <typename ProductDefinitionT, size_t BindingCapacity, typename... Bindings>
auto bindDemoGroupAddresses(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                            Bindings&&... bindings) -> util::Result<void>;

template <typename ProductDefinitionT, size_t BindingCapacity>
bool loadParameterImage(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                        std::span<const uint8_t> image);

} // namespace expert

struct KnxPersistenceIdentity {
    // Advanced override for KNX-managed commissioned-state persistence.
    // Normal firmware should usually leave this empty and rely on the product
    // definition's persistence namespace.
    std::string_view instanceKey{};
    std::string_view storageNamespace{};
};

struct ResolvedKnxPersistenceIdentity {
    std::string instanceKey{};
    std::string storageNamespace{};
    // Derived from PersistencePolicy::schemaVersion and treated as the
    // compatibility boundary for stack-owned KNX commissioned state.
    uint16_t schemaVersion{1};
};

namespace detail {

#if KNX_FEATURE_NETIP && KNX_SECURE_ENABLED
/**
 * @brief The Secure Routing multicast timer, kept beside the commissioned state.
 *
 * One record under its own key in the storage namespace the stack's interface
 * objects use, opened through the same platform persistence. A second handle
 * on that namespace rather than a hook into the object manager: the record is
 * not a property, is written on its own schedule (at most once per hour of
 * timer time, 03_08_09 §2.2.4.2), and must not be rewritten with every
 * property download.
 *
 * Opened on first use, which is when the first backbone key is applied.
 */
class PersistentMcTimerStore final : public netip::ip_secure::McTimerStore {
public:
    explicit PersistentMcTimerStore(std::string storageNamespace)
        : _namespace(std::move(storageNamespace))
    {
    }

    std::optional<Record> load() override
    {
        if (!open()) {
            return std::nullopt;
        }
        std::vector<uint8_t> data;
        if (_persistence->load(kRecordKey, data).isError()) {
            return std::nullopt;
        }
        return decode(data);
    }

    util::Result<void> save(const Record& record) override
    {
        if (!open()) {
            return util::ErrorCode::NotInitialized;
        }
        const auto encoded = encode(record);
        return _persistence->save(kRecordKey, encoded);
    }

private:
    /// Short enough for an NVS key (15 characters), and not of the dXXXX
    /// form the interface-object records use.
    static constexpr const char* kRecordKey = "mc_timer";

    bool open()
    {
        if (_persistence) {
            return true;
        }
        auto persistence = objects::createPersistence();
        if (!persistence || persistence->init(_namespace).isError()) {
            return false;
        }
        _persistence = std::move(persistence);
        return true;
    }

    std::string _namespace;
    std::unique_ptr<objects::ObjectPersistence> _persistence;
};
#endif

template <typename ParametersT>
struct ParameterCallbacks;

template <typename ParametersT>
class ParameterState;

// ---------------------------------------------------------------------------
// Parameters: one image, read through a view (parameter_layout.hpp)
// ---------------------------------------------------------------------------

/// True for a product that declares parameters.
template <typename ParametersT>
inline constexpr bool kHasParameters = false;

template <const auto& Layout>
inline constexpr bool kHasParameters<params::LayoutParameters<Layout>> = true;

/// The value type of a parameter of a product. Defined only for a product
/// with parameters, and looked at only when a parameter is named.
template <typename ParametersT, auto ParameterId>
struct ParameterValueOf;

template <const auto& Layout, auto ParameterId>
struct ParameterValueOf<params::LayoutParameters<Layout>, ParameterId> {
    using type = params::value_type_in<typename params::LayoutParameters<Layout>::layout_type, ParameterId>;
};

/// A product without parameters has no callback and no image.
template <>
struct ParameterCallbacks<params::NoParameters> {};

template <>
class ParameterState<params::NoParameters> {
public:
    explicit ParameterState(ParameterCallbacks<params::NoParameters>&& /*callbacks*/ = {}) {}
};

template <const auto& Layout>
struct ParameterCallbacks<params::LayoutParameters<Layout>> {
    util::InplaceFunction<void(params::ViewFor<Layout>), 32> changed{};
};

/// Owns the product's parameter image: the one copy of the parameters.
template <const auto& Layout>
class ParameterState<params::LayoutParameters<Layout>> {
public:
    using parameters_type = params::LayoutParameters<Layout>;
    using layout_type = typename parameters_type::layout_type;
    using view_type = params::ViewFor<Layout>;

    static_assert(layout_type::kSize >= 1u, "a parameter layout holds at least one parameter");
    static_assert(layout_type::kSize <= objects::tableseg::kApplicationCodeMaxSize,
                  "the parameter image does not fit the application code segment "
                  "(objects::tableseg::kApplicationCodeMaxSize)");

    explicit ParameterState(ParameterCallbacks<parameters_type>&& callbacks = {})
        : _image(params::kDefaultImageOf<Layout>)
        , _callbacks(std::move(callbacks))
    {
    }

    [[nodiscard]] view_type view() const noexcept
    {
        return view_type(std::span<const uint8_t, layout_type::kSize>(_image));
    }

    /// What the stack needs to let ETS write the image, to store it and to
    /// restore it.
    [[nodiscard]] objects::ParameterImageBinding binding() noexcept
    {
        return objects::ParameterImageBinding{
            .image = std::span<uint8_t>(_image),
            .defaults = params::kDefaultImageOf<Layout>.data(),
            .fingerprint = layout_type::kFingerprint,
            .withinBounds = &withinBounds,
        };
    }

    void notifyChanged()
    {
        if (_callbacks.changed) {
            _callbacks.changed(view());
        }
    }

    /// Write one parameter. A value outside the parameter's bounds, or an
    /// instance the block does not have, changes nothing.
    template <auto ParameterId>
    auto set(size_t instance, params::value_type_in<layout_type, ParameterId> value) -> util::Result<void>
    {
        using Value = params::value_type_in<layout_type, ParameterId>;
        if (instance >= layout_type::template kCount<decltype(ParameterId)>) {
            return util::ErrorCode::OutOfRange;
        }
        uint8_t* const at = _image.data() + params::offsetInImage<layout_type, ParameterId>(instance);
        std::array<uint8_t, params::kWidth<Value>> previous{};
        std::copy_n(at, previous.size(), previous.begin());
        params::store<layout_type, ParameterId>(std::span<uint8_t, layout_type::kSize>(_image), instance, value);
        if (!withinBounds(_image)) {
            std::copy_n(previous.begin(), previous.size(), at);
            return util::ErrorCode::OutOfRange;
        }
        return util::Result<void>::ok();
    }

private:
    static bool withinBounds(std::span<const uint8_t> image)
    {
        return params::imageWithinBounds(params::kBoundsOf<Layout>, image);
    }

    std::array<uint8_t, layout_type::kSize> _image;
    ParameterCallbacks<parameters_type> _callbacks{};
};

template <typename ProductDefinitionT>
ResolvedKnxPersistenceIdentity resolveKnxPersistenceIdentity(const ProductDefinitionT& definition,
                                                            const KnxPersistenceIdentity& requested)
{
    ResolvedKnxPersistenceIdentity resolved{};
    resolved.schemaVersion = definition.endpointDefinition.persistence.schemaVersion;
    const bool hasExplicitInstanceKey = !requested.instanceKey.empty();

    if (hasExplicitInstanceKey) {
        resolved.instanceKey = requested.instanceKey;
    } else if (!definition.endpointDefinition.identity.productKey.empty()) {
        resolved.instanceKey = std::string(definition.endpointDefinition.identity.productKey);
    } else {
        resolved.instanceKey = "commissioned_product";
    }

    if (!requested.storageNamespace.empty()) {
        resolved.storageNamespace = requested.storageNamespace;
        return resolved;
    }

    const auto prefix = definition.endpointDefinition.persistence.namespacePrefix;
    if (hasExplicitInstanceKey && !prefix.empty() && std::string_view(prefix) != resolved.instanceKey) {
        resolved.storageNamespace = std::string(prefix) + "_" + resolved.instanceKey;
        return resolved;
    }
    if (!prefix.empty()) {
        resolved.storageNamespace = std::string(prefix);
        return resolved;
    }

    resolved.storageNamespace = resolved.instanceKey;
    return resolved;
}

} // namespace detail

/**
 * @brief What a product is: its ports, and how its parameters are laid out
 *
 * The parameters are part of the type only. `ParametersT` is
 * params::NoParameters, or params::LayoutParameters<kParameters> naming the
 * product's parameter layout; nothing of the layout object is stored here.
 */
template <typename EndpointDefinitionT, typename ParametersT = params::NoParameters>
struct CommissionedProductDefinition {
    using endpoint_definition_type = std::remove_cvref_t<EndpointDefinitionT>;
    using parameters_type = std::remove_cvref_t<ParametersT>;
    using port_id_type = typename endpoint_definition_type::port_id_type;

    /// The value type of a parameter, e.g. ParameterValueType<In::DebounceMs>.
    template <auto ParameterId>
    using ParameterValueType = typename detail::ParameterValueOf<parameters_type, ParameterId>::type;

    endpoint_definition_type endpointDefinition{};
};

/// A product without ETS parameters.
template <typename EndpointDefinitionT>
constexpr auto makeCommissionedProduct(EndpointDefinitionT endpointDefinition)
    -> CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, params::NoParameters>
{
    return CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, params::NoParameters>{
        .endpointDefinition = std::move(endpointDefinition),
    };
}

/**
 * @brief A product whose ETS parameters are the layout named as template argument
 *
 * @code
 *   inline constexpr auto kParameters = params::parameterLayout(kDeviceBlock, params::repeat<8>(kInputBlock));
 *   inline constexpr auto kProduct = makeCommissionedProduct<kParameters>(makeEndpointDefinition<...>(...));
 * @endcode
 *
 * The runtime holds the parameter image (docs/design/parameter-model.md §3.4):
 * `parameters()` is a view over it, `onParametersChanged` tells the product
 * about a completed download.
 */
template <const auto& Layout, typename EndpointDefinitionT>
constexpr auto makeCommissionedProduct(EndpointDefinitionT endpointDefinition)
    -> CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, params::LayoutParameters<Layout>>
{
    return CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, params::LayoutParameters<Layout>>{
        .endpointDefinition = std::move(endpointDefinition),
    };
}

#if KNX_SECURE_ENABLED
/// The KNX Data Secure state this device has ex-factory.
///
/// Both fields are factory defaults (the rule on bau::BusAccessUnit::init and
/// DEVELOPMENT_GUIDELINES.md "Configuration before init() / start()"): they
/// are applied before the stack restores its persisted state, so a Tool Key
/// or Security Mode that ETS downloaded, or that the device stored on an
/// earlier start, takes precedence, and a Factory Reset returns to them.
///
/// They are independent. What a Data Secure device ships as is a factory Tool
/// Key with Security Mode off: ETS opens its first secure session with the
/// FDSK, installs its own Tool Key and turns the mode on at the end of a
/// secure download (03/05/01 §6.3.5), while a project that does not use
/// security commissions the device in plain. Defaults are off/empty, so a
/// non-secure product needs no change.
struct SecureCommissioningOptions {
    /// Factory Tool Key (FDSK, 16-byte AES-128), in force while no other Tool
    /// Key is stored. Lets ETS open the first secure management session.
    std::optional<std::array<uint8_t, 16>> factoryToolKey{};

    /// PID_SECURITY_MODE on a fresh device and after a Factory Reset. Leave
    /// it false for an ETS-commissioned product: the mode belongs to ETS.
    bool securityModeAtFactoryReset{false};
};
#endif  // KNX_SECURE_ENABLED

struct CommissionedProductOptions {
    KnxPersistenceIdentity persistence{};
    // Standalone smoke tests may set this to exercise normal process traffic
    // without ETS commissioning. Real products should leave it unset so KNstaX
    // keeps the KNX initial device address until a unique IA is assigned.
    std::optional<IndividualAddress> standaloneDemoIndividualAddress{};
    std::unique_ptr<physical::Tp1MediumBackend> mediumBackend;
    bool restoreKnxStateOnBoot{true};
    bool startInProgrammingMode{false};
#if KNX_SECURE_ENABLED
    /// Secure commissioning options.  Defaults are behavior-safe: security
    /// disabled, no keys applied.  Non-secure products require no change here.
    SecureCommissioningOptions secure{};
#endif
};

enum class CommissionedTransportPath : uint8_t {
    NotStarted = 0,
    Tp1MediumBackend,    ///< TP1 medium backend (product-grade, managed by stack)
    Tp1MacPhysical,      ///< TP1 MAC physical (advanced manual)
    IpTunneling,         ///< IP tunneling — raw physical pointer (advanced manual)
    IpRouting,           ///< IP routing — raw physical pointer (advanced manual)
    IpTunnelingManaged,  ///< IP tunneling via IpTunnelingOptions (functional, stack-managed)
    IpRoutingManaged,    ///< IP routing via IpRoutingOptions (functional, stack-managed)
    CustomStackPort,     ///< A stack port the caller assembled (other media, simulated bus)
#if KNX_FEATURE_COUPLER
    Tp1Coupler,          ///< Two TP1 ports with spec routing between them (product-grade)
#endif
};

/// Support tier classification for the active transport path.
enum class CommissionedSupportTier : uint8_t {
    ProductGrade = 0, ///< Fully validated ETS-programmable production path.
    Functional,       ///< Stack-managed setup; functional for deployment, not yet ETS-catalogued.
    AdvancedManual,   ///< Raw physical pointer; requires manual wiring and expert knowledge.
};

constexpr CommissionedSupportTier supportTierForTransportPath(CommissionedTransportPath path)
{
    switch (path) {
        case CommissionedTransportPath::Tp1MediumBackend:
#if KNX_FEATURE_COUPLER
        case CommissionedTransportPath::Tp1Coupler:
#endif
            return CommissionedSupportTier::ProductGrade;
        case CommissionedTransportPath::IpTunnelingManaged:
        case CommissionedTransportPath::IpRoutingManaged:
            return CommissionedSupportTier::Functional;
        case CommissionedTransportPath::Tp1MacPhysical:
        case CommissionedTransportPath::IpTunneling:
        case CommissionedTransportPath::IpRouting:
        case CommissionedTransportPath::CustomStackPort:
        case CommissionedTransportPath::NotStarted:
            return CommissionedSupportTier::AdvancedManual;
    }

    return CommissionedSupportTier::AdvancedManual;
}

#if KNX_FEATURE_NETIP
#if KNX_IP_TUNNELING
/// Options for starting a commissioned product over KNXnet/IP tunneling.
///
/// The network interface is taken from the `platform::Platform` argument
/// passed to `startCommissionedProduct` — firmware does not need to extract
/// it manually.
struct IpTunnelingOptions {
    /// Required: KNXnet/IP gateway host address.
    IpAddress host{};
    /// KNXnet/IP gateway port (default 3671).
    NetIpPort port{NetIpPort(3671)};
#if KNX_SECURE_ENABLED
    /// Device-level KNX Data Secure options (same as for TP1).  Default is
    /// behavior-safe (security disabled, no keys).
    SecureCommissioningOptions secure{};
#endif
};
#endif  // KNX_IP_TUNNELING

/// Optional KNX/IP Secure Routing configuration (network-level AES encryption).
/// Distinct from device-level Data Secure (`SecureCommissioningOptions`).
struct IpRoutingSecureOptions {
#if KNX_SECURE_ENABLED
    /// Enable KNX/IP Secure Routing.  When false (default) plain multicast is used.
    bool enabled{false};
    /// 16-byte AES-128 group key for multicast encryption. Used only while
    /// ETS has not downloaded a backbone key of its own, which takes precedence.
    ///
    /// Nothing else is configured: the serial is the device's KNX serial number,
    /// the tag is random per frame and the sequence information is the
    /// multicast timer, which the product persists (03_08_09 §2.2.2).
    std::array<uint8_t, 16> groupKey{};
#endif
};

/// Options for starting a commissioned product over KNXnet/IP Routing (multicast).
struct IpRoutingOptions {
    /**
     * Required: the product's routing multicast group. Standard KNX default:
     * 224.0.23.12 (03_08_03 §2.5.17: the default "shall be equal to the
     * KNXnet/IP System Setup Multicast Address").
     *
     * A default, not the group in force: PID_ROUTING_MULTICAST_ADDRESS is
     * ETS's to set, and the value it last wrote is persisted and used from the
     * next start on. This group is used when none is stored — the first start,
     * and the one after a Factory Reset.
     */
    IpAddress multicastGroup{};

    /**
     * Use multicastGroup even when the device has a stored group.
     *
     * For a product with a configuration channel of its own, set once after
     * that channel changed the group, so the latest change wins whichever side
     * made it. The group is then stored as if ETS had written it.
     */
    bool multicastGroupOverridesStored{false};
    /// UDP port (default 3671).
    NetIpPort port{NetIpPort(3671)};
    /// Egress/join interface address.  Use IpAddress(0) for the system default.
    IpAddress interfaceAddress{};
    /// Multicast hop limit.  1 confines KNX traffic to the local subnet; the
    /// default of 16 matches what KNXnet/IP routers ship with.
    uint8_t timeToLive{16};

    /**
     * Deliver this device's own multicast back to its own socket.
     *
     * Off, because a device has nothing to learn from its own echo and a good
     * deal to lose. The looped-back copy re-enters the stack as an ordinary
     * inbound frame, so a Data Secure device sees its own responses arriving
     * from its own individual address with sequence numbers it has already
     * spent, records each as a security failure against itself, and reports
     * that count to ETS.
     *
     * Worth having as a switch rather than a constant: two endpoints sharing
     * one host only reach each other when the sender loops back, which is how
     * the host tests put two devices on one machine.
     */
    Toggle multicastLoopback{Toggle::Disable};

    /**
     * Answer KNXnet/IP discovery and self-description on the routing socket.
     *
     * On by default, because Core is mandatory for a KNXnet/IP device
     * (03/08/01 Table 2) and a device that stays silent is one ETS cannot list
     * even while it is sitting on the multicast group. Everything the response
     * carries — individual address, serial number, programming mode, medium —
     * is read from the running device when a request arrives, so the answer is
     * never a snapshot from boot.
     */
    bool announceOnNetwork{true};

    /**
     * The name ETS shows for this device in a discovery list.
     *
     * Up to 30 octets of ISO 8859-1. Empty falls back to the product's order
     * number, which identifies the model but not the unit — set this to
     * whatever the installation calls this particular device.
     */
    std::string friendlyName{};

    /// Optional KNX/IP Secure Routing settings (network-level, not device-level).
    IpRoutingSecureOptions secureRouting{};
#if KNX_SECURE_ENABLED
    /// Device-level KNX Data Secure options.  Default is behavior-safe.
    SecureCommissioningOptions secure{};
#endif
};
#endif  // KNX_FEATURE_NETIP

#if KNX_FEATURE_COUPLER
/**
 * @brief Options for starting a commissioned product as a KNX coupler.
 *
 * The device is a normal commissioned product — same endpoints, parameters,
 * ETS identity and persistence — that additionally routes between two TP1
 * ports according to 03/03/03 §2.4.2.4.
 *
 * The coupler's role (line coupler or backbone coupler) is derived from the
 * individual address ETS assigns: x.y.0 makes it a line coupler for line x.y,
 * x.0.0 a backbone coupler for area x. Until it has one it behaves as a
 * repeater, which is what keeps an uncommissioned coupler from severing the
 * installation it was just plugged into.
 */
struct CouplerOptions {
    /// Required: the upstream side. Main line for a line coupler, backbone for
    /// a backbone coupler.
    std::unique_ptr<physical::Tp1MediumBackend> primary;

    /// Required: the downstream subnetwork.
    std::unique_ptr<physical::Tp1MediumBackend> secondary;

    /**
     * Publish the Router Object (interface object type 6).
     *
     * On by default: a coupler that does not publish one cannot receive an ETS
     * filter-table download, so it would have to route everything. Turning it
     * off is only sensible for a device that routes by a policy the firmware
     * sets itself and that ETS should not try to configure.
     */
    bool publishRouterObject{true};

    /// PID_MEDIUM for the secondary side. 0 = TP1, 1 = PL110, 2 = RF, 5 = IP.
    uint8_t subMedium{0};

#if KNX_SECURE_ENABLED
    /// Device-level KNX Data Secure options.
    SecureCommissioningOptions secure{};
#endif
};
#endif  // KNX_FEATURE_COUPLER

template <typename ProductDefinitionT, size_t BindingCapacity>
class CommissionedBindingsBuilder {
public:
    using product_definition_type = std::remove_cvref_t<ProductDefinitionT>;
    using endpoint_definition_type = typename product_definition_type::endpoint_definition_type;
    using parameters_type = typename product_definition_type::parameters_type;

    explicit CommissionedBindingsBuilder(endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity> endpointBindings)
        : _endpointBindings(std::move(endpointBindings))
    {
    }

    // Every registration method comes in an lvalue- and an rvalue-qualified
    // form. The rvalue form keeps a chain that started from a temporary —
    // `makeCommissionedBindings(kProduct).onCommand<...>(...)...` — an rvalue
    // all the way to startCommissionedProduct(), which takes the builder by
    // rvalue reference so it is never copied into the caller's stack frame.
    // The builder is several kilobytes for a product of any size, so that copy
    // is the difference between fitting the service task's stack and not.

    /**
     * Register a command handler invoked from CommissionedProductRuntime::loop().
     * Keep the callback non-blocking and treat it as owner-context code.
     */
    template <auto LogicalId, typename Callback>
    auto& onCommand(Callback&& callback) &
    {
        _endpointBindings.template onCommand<LogicalId>(std::forward<Callback>(callback));
        return *this;
    }

    template <auto LogicalId, typename Callback>
    auto onCommand(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onCommand<LogicalId>(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a writable-state handler invoked from CommissionedProductRuntime::loop().
     */
    template <auto LogicalId, typename Callback>
    auto& onStateWrite(Callback&& callback) &
    {
        _endpointBindings.template onStateWrite<LogicalId>(std::forward<Callback>(callback));
        return *this;
    }

    template <auto LogicalId, typename Callback>
    auto onStateWrite(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onStateWrite<LogicalId>(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a state provider read from CommissionedProductRuntime::loop().
     */
    template <auto LogicalId, typename Provider>
    auto& provideState(Provider&& provider) &
    {
        _endpointBindings.template provideState<LogicalId>(std::forward<Provider>(provider));
        return *this;
    }

    template <auto LogicalId, typename Provider>
    auto provideState(Provider&& provider) && -> CommissionedBindingsBuilder&&
    {
        provideState<LogicalId>(std::forward<Provider>(provider));
        return std::move(*this);
    }

    auto& onProgrammingModeChanged(endpoint::ProgModeCallback callback) &
    {
        _endpointBindings.onProgrammingModeChanged(std::move(callback));
        return *this;
    }

    auto onProgrammingModeChanged(endpoint::ProgModeCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onProgrammingModeChanged(std::move(callback));
        return std::move(*this);
    }

    auto& onFault(endpoint::FaultCallback callback) &
    {
        _endpointBindings.onFault(std::move(callback));
        return *this;
    }

    auto onFault(endpoint::FaultCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onFault(std::move(callback));
        return std::move(*this);
    }

    /**
     * Register the one parameter callback of a product built with
     * makeCommissionedProduct<kParameters>(). It gets a view over the new
     * values after ETS completed a parameter download and after
     * setParameter(); the product reloads what it keeps decoded. It is not
     * called at start: read parameters() once startCommissionedProduct()
     * returned.
     *
     * @code
     *   bindings.onParametersChanged([](params::ViewFor<kParameters> p) {
     *       g_debounceMs = p.block<Dev>().get<Dev::DebounceMs>();
     *   });
     * @endcode
     */
    template <typename Callback>
    auto& onParametersChanged(Callback&& callback) &
    {
        _parameterCallbacks.changed = std::forward<Callback>(callback);
        return *this;
    }

    template <typename Callback>
    auto onParametersChanged(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onParametersChanged(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a callback that fires every time the device lifecycle state changes.
     *
     * Called at most once per loop() invocation, only on actual state transitions:
     *   Uncommissioned → Commissioning → Operational (and back)
     *
     * Example:
     * @code
     *   bindings.onLifecycleChanged([](DeviceLifecycleState s) {
     *       if (s == DeviceLifecycleState::Operational) {
     *           indicator_led_set(LED_SOLID_GREEN);
     *       }
     *   });
     * @endcode
     */
    auto& onLifecycleChanged(LifecycleChangedCallback callback) &
    {
        _lifecycleCallback = std::move(callback);
        return *this;
    }

    auto onLifecycleChanged(LifecycleChangedCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onLifecycleChanged(std::move(callback));
        return std::move(*this);
    }

private:
    endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity> _endpointBindings;
    detail::ParameterCallbacks<parameters_type> _parameterCallbacks{};
    LifecycleChangedCallback _lifecycleCallback;

    template <typename DefinitionT, size_t Cap>
    friend class CommissionedProductRuntime;

    template <typename DefinitionT, size_t Cap>
    friend auto makeCommissionedBindings(const DefinitionT& definition)
        -> CommissionedBindingsBuilder<DefinitionT, Cap>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         CommissionedProductOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<bau::BusAccessStackPort> stackPort,
                                         CommissionedProductOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

#if KNX_SECURE_ENABLED
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::Tp1MacPhysical> physical,
                                         SecureCommissioningOptions secure)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#else
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::Tp1MacPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif

#if KNX_FEATURE_COUPLER
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         CouplerOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif

#if KNX_IP_TUNNELING
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::IpTunnelingPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         IpTunnelingOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::IpRoutingPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         IpRoutingOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif  // KNX_FEATURE_NETIP
};

template <size_t BindingCapacity = kDefaultBindingCapacity, typename ProductDefinitionT>
auto makeCommissionedBindings(const ProductDefinitionT& definition)
    -> CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>
{
    return CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>(
        endpoint::makeEndpointBindings<BindingCapacity>(definition.endpointDefinition));
}

template <typename ProductDefinitionT, size_t BindingCapacity>
class CommissionedProductRuntime {
public:
    using product_definition_type = std::remove_cvref_t<ProductDefinitionT>;
    using endpoint_definition_type = typename product_definition_type::endpoint_definition_type;
    using parameters_type = typename product_definition_type::parameters_type;
    using port_id_type = typename product_definition_type::port_id_type;
    using endpoint_runtime_type = endpoint::EndpointRuntime<endpoint_definition_type, BindingCapacity>;
    using OwnerWorkHint = typename endpoint_runtime_type::OwnerWorkHint;
    using WorkAvailableCallback = typename endpoint_runtime_type::WorkAvailableCallback;

    // The bindings and the parameter callbacks are taken by rvalue reference,
    // not by value: both are sized by the product's port and parameter counts
    // and run to several kilobytes each, so a by-value parameter would have the
    // caller materialise a full copy in its own stack frame purely to hand it
    // straight into a member. They are consumed here either way.
    CommissionedProductRuntime(const product_definition_type& definition,
                               endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity>&& endpointBindings,
                               detail::ParameterCallbacks<parameters_type>&& parameterCallbacks,
                               LifecycleChangedCallback lifecycleCallback,
                               CommissionedProductOptions options)
        : _definition(definition)
        , _knxPersistenceIdentity(detail::resolveKnxPersistenceIdentity(definition, options.persistence))
        , _parametersState(std::move(parameterCallbacks))
        , _lifecycleCallback(std::move(lifecycleCallback))
        , _runtime(
              _definition.endpointDefinition,
              std::move(endpointBindings),
              endpoint::EndpointInstanceConfig{
                  .defaultIndividualAddress =
                      options.standaloneDemoIndividualAddress.value_or(initialIndividualAddress()),
                  .persistenceNamespace = _knxPersistenceIdentity.storageNamespace,
                  .persistenceSchemaVersion = _knxPersistenceIdentity.schemaVersion,
                  .restoreKnxStateOnBoot = options.restoreKnxStateOnBoot,
              })
        , _startInProgrammingMode(options.startInProgrammingMode)
#if KNX_SECURE_ENABLED
        , _secureOptions(options.secure)
#endif
    {
        if constexpr (detail::kHasParameters<parameters_type>) {
            _runtime.bindParameterImage(_parametersState.binding());
        }
    }

    // Not copyable or movable: start() hands `this` to the parameter, endpoint
    // and IP routing callbacks (setLatencySettingsSource among them), so the
    // runtime stays where it was built. startCommissionedProduct() builds it on
    // the heap for that reason.
    CommissionedProductRuntime(const CommissionedProductRuntime&) = delete;
    CommissionedProductRuntime& operator=(const CommissionedProductRuntime&) = delete;
    CommissionedProductRuntime(CommissionedProductRuntime&&) = delete;
    CommissionedProductRuntime& operator=(CommissionedProductRuntime&&) = delete;

    auto start(platform::Platform& platform,
               std::unique_ptr<physical::Tp1MediumBackend> mediumBackend) -> util::Result<void>
    {
        if (!mediumBackend) {
            return util::ErrorCode::InvalidParameter;
        }

        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(mediumBackend));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1MediumBackend;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

    /**
     * @brief Start on a stack port the caller assembled.
     *
     * For a medium the overloads above do not cover, and for tests that run
     * the product against a simulated bus.
     */
    auto start(platform::Platform& platform,
               std::unique_ptr<bau::BusAccessStackPort> stackPort) -> util::Result<void>
    {
        if (!stackPort) {
            return util::ErrorCode::InvalidParameter;
        }

        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(stackPort));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::CustomStackPort;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

#if KNX_FEATURE_COUPLER
    /**
     * @brief Start as a KNX coupler over two TP1 ports.
     *
     * Everything the single-port path does still happens — endpoints,
     * parameters, persistence, ETS commissioning — and forwarding between the
     * two ports is added on top.
     */
    auto start(platform::Platform& platform, CouplerOptions options) -> util::Result<void>
    {
        if (!options.primary || !options.secondary) {
            return util::ErrorCode::InvalidParameter;
        }

        auto stackPort = bau::createTp1CouplerStackPort(
            platform,
            std::make_unique<physical::Tp1MacPhysical>(std::move(options.primary)),
            std::make_unique<physical::Tp1MacPhysical>(std::move(options.secondary)));

#if KNX_SECURE_ENABLED
        _secureOptions = options.secure;
#endif
        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(stackPort));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1Coupler;

        if (options.publishRouterObject) {
            // The filter table and routing policy are left null on purpose:
            // configureRouterRole() takes them from the coupler the stack port
            // already built, so there is only ever one of each.
            bau::BusAccessUnit::RouterRoleConfig routerConfig;
            routerConfig.subMedium = options.subMedium;
            auto routerResult = _runtime.configureRouterRole(routerConfig);
            if (routerResult.isError()) {
                return routerResult.error();
            }
        }

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_FEATURE_COUPLER
#if KNX_FEATURE_TP1

    auto start(platform::Platform& platform,
               std::unique_ptr<physical::Tp1MacPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }


        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1MacPhysical;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_FEATURE_TP1

#if KNX_IP_TUNNELING
    auto start(platform::Platform& platform,
               std::unique_ptr<physical::IpTunnelingPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }

        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpTunneling;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
    auto start(platform::Platform& platform,
               std::unique_ptr<physical::IpRoutingPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }

        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpRouting;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

    // -----------------------------------------------------------------------
    // Managed IP transport overloads
    //
    // These overloads accept option structs and handle physical-layer assembly
    // internally, keeping firmware code free of physical_factory.hpp details.
    // The network interface is taken from the supplied Platform.
    // -----------------------------------------------------------------------

#if KNX_IP_TUNNELING
    auto start(platform::Platform& platform, IpTunnelingOptions options) -> util::Result<void>
    {
        if (options.host.raw == 0) {
            return util::ErrorCode::InvalidParameter;
        }

        auto* network = platform.network();
        if (!network) {
            return util::ErrorCode::ResourceUnavailable;
        }

        auto physical = std::make_unique<physical::IpTunnelingPhysical>();
        physical->setNetworkInterface(network);
        physical->setGateway(options.host, options.port);

#if KNX_SECURE_ENABLED
        _secureOptions = options.secure;
#endif
        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpTunnelingManaged;

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_IP_TUNNELING

    /// Install the description a Core search response is built from.
    ///
    /// Every field is read when the request arrives rather than captured now:
    /// the individual address changes at commissioning, programming mode
    /// changes at the press of a button, and both decide whether ETS finds this
    /// device at all.
    void configureCoreDiscovery(physical::IpRoutingPhysical& physical,
                                platform::NetworkInterface& network,
                                const IpRoutingOptions& options)
    {
        static constexpr netip::ServiceFamilySupport kFamilies[] = {
            {netip::service_family::kCore, 2},
            {netip::service_family::kRouting, 2},
        };

        std::string friendlyName = options.friendlyName;

        // The physical owns the responder this provider lives in, so the
        // reference cannot outlive it.
        physical.setCoreDiscovery(
            [this, &network, &physical, friendlyName]() {
                netip::CoreDeviceDescription description{};
                description.medium = static_cast<uint8_t>(MediumType::IP);
                description.programmingMode = _runtime.isProgrammingModeActive();
                description.individualAddress = _runtime.individualAddress();
                // The group the device is on now, which after a start with a
                // stored group is not the one in the options.
                description.routingMulticastAddress = physical.multicastGroup();
                description.maxLocalApduLength = _runtime.identity().maxApduLength;
                // The same mask version setDeviceMedium() puts in the Device
                // Descriptor, from the same table — ETS cross-checks the two.
                description.deviceDescriptorType0 =
                    application::systemBMaskVersion(MediumType::IP);

                network.macAddress(description.macAddress);

                const auto serial = _runtime.deviceObject().getSerialNumber();
                const size_t serialLength = std::min(serial.size(), description.serialNumber.size());
                std::copy_n(serial.begin(), serialLength, description.serialNumber.begin());

                // A name identifies the unit, the order number only the model.
                // Falling back to the model still beats an empty field, which
                // ETS shows as a blank row.
                const std::string_view name =
                    friendlyName.empty() ? std::string_view(_runtime.identity().orderNumber)
                                         : std::string_view(friendlyName);
                const size_t nameLength = std::min(name.size(), description.friendlyName.size());
                std::copy_n(name.begin(), nameLength, description.friendlyName.begin());

                return description;
            },
            std::span<const netip::ServiceFamilySupport>(kFamilies));
    }

    auto start(platform::Platform& platform, IpRoutingOptions options) -> util::Result<void>
    {
        if (options.multicastGroup.raw == 0) {
            return util::ErrorCode::InvalidParameter;
        }

        auto* network = platform.network();
        if (!network) {
            return util::ErrorCode::ResourceUnavailable;
        }

        auto physical = std::make_unique<physical::IpRoutingPhysical>();
        physical->setNetworkInterface(network);
        physical->setMulticast(options.multicastGroup, options.port, options.interfaceAddress);
        physical->setMulticastSocketOptions(options.timeToLive, options.multicastLoopback);

        if (options.announceOnNetwork) {
            configureCoreDiscovery(*physical, *network, options);
        }

        // The platform RNG for the ROUTING_BUSY back-off and, under Secure
        // Routing, for message tags and TIMER_NOTIFY delays.
        physical->setRandomSource([&platform](std::span<uint8_t> out) { platform.randomBytes(out); });

#if KNX_SECURE_ENABLED
        _mcTimerStore = std::make_unique<detail::PersistentMcTimerStore>(_knxPersistenceIdentity.storageNamespace);
        physical->setMcTimerStore(_mcTimerStore.get());
        physical->setLatencySettingsSource([this]() { return secureRoutingLatencySettings(); });
#endif

        // Kept across the move: the stack port owns the physical for as long
        // as the runtime lives, and the backbone-key observer below has to
        // reach it after start().
        auto* routing = physical.get();

#if KNX_SECURE_ENABLED
        _secureOptions = options.secure;
#endif
        applySecureOptions();  // factory defaults: before init(), restore wins
        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpRoutingManaged;

        // The socket opened on the product's group, before the stored
        // KNXnet/IP Parameter Object came back. A group ETS downloaded before
        // the last restart is in there now, and 03_08_03 §2.5.17 makes it the
        // one in force from this start on. Settled before anything is sent:
        // all sending happens from the stack task's loop(), which has not run.
        {
            const IpAddress group = _runtime.adoptRoutingMulticastAddress(
                options.multicastGroup, options.multicastGroupOverridesStored);
            if (group != options.multicastGroup) {
                const auto moved = routing->setMulticastGroup(group);
                char text[16] = {0};
                group.toString(text);
                if (moved.isError()) {
                    // Still the group sends go to; the join completes on the
                    // next membership refresh, as at an open() without an
                    // interface.
                    KNX_LOGW("KNX.Product", "Routing group %s not joined yet: %s", text,
                             util::errorCodeToString(moved.error()));
                } else {
                    KNX_LOGI("KNX.Product", "Routing on the stored group %s", text);
                }
            }
        }

        // A group written while running is stored and becomes active after the
        // next restart (§2.5.17) — moving now would strand the management
        // connection ETS is writing it over. What a new group does change at
        // once is the multicast timer: E11, answered with A7 (03_08_09
        // §2.2.2.3.2.7).
        _runtime.setRoutingMulticastAddressCallback([routing](IpAddress) {
#if KNX_SECURE_ENABLED
            routing->restartMulticastTimerSync();
#else
            (void)routing;
#endif
        });

#if KNX_SECURE_ENABLED
        // ETS enables KNX/IP Secure Routing by downloading the backbone key as
        // an ordinary property write, and expects it to take effect at once.
        // Wired here rather than left to the firmware because there is exactly
        // one correct thing to do with that key, and a device that stores it
        // and keeps multicasting in the clear looks secured in the project and
        // is not on the wire.
        _runtime.setBackboneKeyCallback([routing](std::span<const uint8_t> key) {
            (void)routing->applySecureRouting(key, physical::IpRoutingPhysical::KeySource::Download);
        });

        // The nonce carries the KNX serial number. It is known once the runtime
        // has started, and has to be in place before any key is applied.
        {
            std::array<uint8_t, 6> serial{};
            const auto deviceSerial = _runtime.deviceObject().getSerialNumber();
            std::copy_n(deviceSerial.begin(), std::min(serial.size(), deviceSerial.size()), serial.begin());
            routing->setSecureRoutingSerial(serial);
        }
        seedSecureRoutingLatencyDefaults();

        // A backbone key ETS downloaded before the last restart came back with
        // the KNXnet/IP Parameter Object while the runtime started, but through
        // the object store rather than a property write, so the callback above
        // never saw it. Without this a secured device reboots into plain
        // multicast and stays there until ETS downloads again.
        const auto storedKey = _runtime.referenceObjectPropertyValue(
            InterfaceObjectType::knxNetIpParameter(), kPidBackboneKey);
        const bool storedKeySet = std::any_of(storedKey.begin(), storedKey.end(),
                                              [](uint8_t b) { return b != 0; });
        util::Result<void> restored = util::Result<void>::ok();
        if (storedKeySet) {
            restored = routing->applySecureRouting(storedKey, physical::IpRoutingPhysical::KeySource::Restored);
        } else if (options.secureRouting.enabled) {
            restored = routing->applySecureRouting(options.secureRouting.groupKey,
                                                   physical::IpRoutingPhysical::KeySource::Restored);
        }
        if (restored.isError()) {
            // The device stays on plain multicast, which its secured peers
            // ignore; say so rather than leave a silent half-commissioned node.
            KNX_LOGW("KNX.Product", "Backbone key not restored, routing stays plain: %s",
                     util::errorCodeToString(restored.error()));
        }
#else
        (void)routing;
#endif

        wireParameters();

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_FEATURE_NETIP

#if KNX_FEATURE_NETIP
    /**
     * @brief Publish this device's live IP identity to the KNXnet/IP Parameter Object.
     *
     * ETS reads a KNX IP device's address, multicast group, MAC and friendly
     * name out of interface object 11. The stack registers the object; only the
     * firmware knows the values, and not until the network interface is up.
     * Call this once it is, and again on every address change.
     */
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config)
    {
        return _runtime.applyKnxNetIpConfiguration(config);
    }

    /**
     * @brief Re-join the routing multicast group after an interface change.
     *
     * The membership is an interface-local thing: it is established when the
     * routing socket opens, and it does not follow the interface afterwards.
     * A device that started before its network was up never joined at all, and
     * one whose lease moved it into a different subnet holds a membership on a
     * route that no longer carries the group. Both are silent — the socket is
     * open, sends succeed, and nothing arrives.
     *
     * So the firmware, which is the only party that learns the interface
     * changed, says so here. Call it whenever the interface gains or changes an
     * address; it is idempotent, and cheap enough that calling it on every such
     * event is the intended use.
     *
     * Returns OperationNotSupported on a product that is not running KNXnet/IP
     * routing, and NotInitialized before start().
     */
    util::Result<void> refreshIpMulticastMembership()
    {
        return _runtime.refreshMulticastMembership();
    }

    /**
     * @brief The routing multicast group in PID_ROUTING_MULTICAST_ADDRESS.
     *
     * The group the device is routing on, unless ETS (by property write or
     * A_DomainAddressSerialNumber_Write) has set another since start, which
     * becomes the group in force at the next restart (03_08_03 §2.5.17).
     * 0.0.0.0 on a product not running KNXnet/IP routing.
     */
    [[nodiscard]] IpAddress routingMulticastAddress() const
    {
        return _runtime.routingMulticastAddress();
    }
#endif

    /**
     * @brief Counters for the port this product is running on.
     *
     * For the question a capture taken on the network cannot answer: when the
     * device stops responding, did the telegram reach it at all? On a KNXnet/IP
     * routing port, `mediumDatagramsReceived` standing still while a client is
     * plainly transmitting means the group is no longer being delivered here,
     * and nothing above the socket can fix that.
     *
     * Zero-filled before start() and on a port that keeps no such counter.
     */
    bau::LinkDiagnostics linkDiagnostics() const
    {
        return _runtime.linkDiagnostics();
    }

#if KNX_FEATURE_COUPLER
    /**
     * @brief The coupler, when the product was started with `CouplerOptions`.
     *
     * nullptr for an ordinary end device. Use it to reach the filter table, the
     * routing policy and the observability callbacks.
     */
    network::TwoPortCoupler* coupler() { return _runtime.coupler(); }

    /**
     * @brief Apply a downloaded coupler configuration to the live routing policy.
     *
     * `PID_MAIN_LCCONFIG` and its siblings are ordinary properties: ETS writes
     * them into the Router Object, and they change nothing until this is
     * called. The filter table is separate — it applies live through
     * `PID_ROUTETABLE_CONTROL`. Call this after a download completes.
     */
    util::Result<void> syncRouterRoutingConfig() { return _runtime.syncRouterRoutingConfig(); }
#endif  // KNX_FEATURE_COUPLER

    /**
     * Progress the runtime and deliver queued firmware callbacks.
     *
     * Call from a single owner context. `onCommand`, `onStateWrite`,
     * `onProgrammingModeChanged`, and `onLifecycleChanged` callbacks run from
     * this method.
     */
    void loop()
    {
        _runtime.loop();

        if (_lifecycleCallback) {
            const auto current = lifecycleState();
            if (current != _lastLifecycleState) {
                _lastLifecycleState = current;
                _lifecycleCallback(current);
            }
        }
    }

    OwnerWorkHint ownerWorkHint() const
    {
        return _runtime.ownerWorkHint();
    }

    void setWorkAvailableCallback(WorkAvailableCallback callback)
    {
        _runtime.setWorkAvailableCallback(std::move(callback));
    }

    /**
     * Returns the current lifecycle state derived from the KNX stack.
     *
     * Equivalent manual check:
     *   isProgrammingModeActive() → Commissioning
     *   isOperationalIndividualAddress(individualAddress()) → Operational
     *   otherwise → Uncommissioned
     */
    DeviceLifecycleState lifecycleState() const
    {
        if (isProgrammingModeActive()) {
            return DeviceLifecycleState::Commissioning;
        }
        if (isOperationalIndividualAddress(individualAddress())) {
            return DeviceLifecycleState::Operational;
        }
        return DeviceLifecycleState::Uncommissioned;
    }

    template <auto LogicalId, typename ValueT>
    auto publish(ValueT&& value) -> util::Result<void>
    {
        return _runtime.template publish<LogicalId>(std::forward<ValueT>(value));
    }

    template <typename ValueT>
    auto publish(port_id_type logicalId, ValueT&& value) -> util::Result<void>
    {
        return _runtime.publish(logicalId, std::forward<ValueT>(value));
    }

    /// True when the ETS project linked a group address to this port. Publishing
    /// on an unlinked port succeeds without emitting a telegram, so query this
    /// when you want to report the unused datapoints of a project.
    bool isPortLinked(port_id_type logicalId) const
    {
        return _runtime.isPortLinked(logicalId);
    }

    template <auto LogicalId>
    bool isPortLinked() const
    {
        return _runtime.template isPortLinked<LogicalId>();
    }

    // ── Outbound transmit shaping (see knx/application/group_object.hpp) ──────
    // Provide a monotonic millisecond clock; required for cyclic sends, per-
    // object min-interval floors, and the telegram rate limiter to take effect.
    void setTimeSource(bau::BusAccessUnit::TimeSourceFn timeSource)
    {
        _runtime.setTimeSource(std::move(timeSource));
    }

    /// Global rate limit for this device's own unsolicited group sends.
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config)
    {
        _runtime.setTelegramRateLimit(config);
    }

    /// Set/read a port's send-on-change / cyclic / min-interval policy. Typically
    /// driven from ETS parameters: build the policy from parameters() after start and again
    /// after onParametersChanged.
    template <auto LogicalId>
    auto setTransmitPolicy(const application::GroupObjectTransmitPolicy& policy) -> util::Result<void>
    {
        return _runtime.setTransmitPolicy(static_cast<port_id_type>(LogicalId), policy);
    }

    auto setTransmitPolicy(port_id_type logicalId,
                           const application::GroupObjectTransmitPolicy& policy) -> util::Result<void>
    {
        return _runtime.setTransmitPolicy(logicalId, policy);
    }

    template <auto LogicalId>
    auto transmitPolicy() const -> util::Result<application::GroupObjectTransmitPolicy>
    {
        return _runtime.transmitPolicy(static_cast<port_id_type>(LogicalId));
    }

    // -----------------------------------------------------------------------
    // Commissioning import helpers
    //
    // These methods are provided for two explicit scenarios:
    //   1. Importing commissioned state from an external source into a running
    //      device (e.g. an ETS-compatible commissioning tool that writes state
    //      through a non-KNX path such as a cloud API or BLE provisioning).
    //   2. Smoke tests and demo setups that need to exercise runtime behavior
    //      without a connected ETS instance.
    //
    // They are NOT needed for normal ETS-managed products. When a real ETS
    // session programs the device over the KNX bus, the stack writes group
    // addresses into the address table and association table through the KNX
    // management protocol and restores them from persistence on reboot.
    // Firmware does not call these methods at all in that flow.
    //
    // Parameters are changed from the device with setParameter(), below.
    // -----------------------------------------------------------------------

    template <auto LogicalId>
    auto applyCommissionedGroupAddress(GroupAddress address) -> util::Result<void>
    {
        return _runtime.template bindGroupAddress<LogicalId>(address);
    }

    auto applyCommissionedGroupAddress(port_id_type logicalId, GroupAddress address) -> util::Result<void>
    {
        return _runtime.bindGroupAddress(logicalId, address);
    }

    template <typename... Bindings>
    auto applyCommissionedGroupAddresses(Bindings&&... bindings) -> util::Result<void>
    {
        return endpoint::bindGroupAddresses(_runtime, std::forward<Bindings>(bindings)...);
    }

    /// Device object access for host-specific identity (e.g. serial number
    /// from a factory-programmed MAC).
    auto& deviceObject() { return _runtime.deviceObject(); }

    /// The parameters: a typed view over the image. Reading one is a load at
    /// a constant offset.
    auto parameters() const
        requires detail::kHasParameters<parameters_type>
    {
        return _parametersState.view();
    }

    /**
     * Change one parameter from the device itself (a local menu, a learned
     * value). The image is stored with the next coalesced save, which needs
     * a clock (setTimeSource()), and onParametersChanged fires. A value
     * outside the parameter's declared bounds is refused.
     *
     * `instance` is the 0-based instance of a repeated block.
     */
    template <auto ParameterId>
    auto setParameter(size_t instance, typename product_definition_type::template ParameterValueType<ParameterId> value)
        -> util::Result<void>
    {
        const auto result = _parametersState.template set<ParameterId>(instance, value);
        if (!result.isError()) {
            _runtime.markParameterImageChanged();
            _parametersState.notifyChanged();
        }
        return result;
    }

    template <auto ParameterId>
    auto setParameter(typename product_definition_type::template ParameterValueType<ParameterId> value)
        -> util::Result<void>
    {
        return setParameter<ParameterId>(size_t{0}, value);
    }

    /// False when the parameters are not a completed download: during an ETS
    /// download, after one that was refused or interrupted, after ETS unloaded
    /// the application, and after a firmware update that changed the parameter
    /// layout. The parameters then read as their defaults, or as half of a
    /// download. A device that was never downloaded reads its defaults and is
    /// loaded, like the tables.
    bool parametersLoaded() const
    {
        return _runtime.applicationProgramLoaded();
    }

    auto individualAddress() const -> IndividualAddress
    {
        return _runtime.individualAddress();
    }

    void reportFault(FaultInfo info) const
    {
        _runtime.reportFault(info);
    }

    size_t pendingBusActionCount() const
    {
        return _runtime.pendingActionCount();
    }

    const endpoint::PendingBusAction<port_id_type>* pendingBusActionAt(size_t index) const
    {
        return _runtime.pendingActionAt(index);
    }

    /// Number of group objects registered after start().  Always equals the
    /// number of ports in the product definition (compile-time fixed).
    size_t registeredGroupObjectCount() const
    {
        return _runtime.registeredGroupObjectCount();
    }

    static constexpr size_t kPortCount = product_definition_type::endpoint_definition_type::kPortCount;

    CommissionedTransportPath transportPath() const
    {
        return _transportPath;
    }

    CommissionedSupportTier supportTier() const
    {
        return supportTierForTransportPath(_transportPath);
    }

    bool isProductGradePath() const
    {
        return supportTier() == CommissionedSupportTier::ProductGrade;
    }
#if KNX_SECURE_ENABLED

    // -----------------------------------------------------------------------
    // KNX Data Secure helpers (Phase C key provisioning API)
    //
    // These convenience methods remove the need for firmware code to know
    // about SecurityInterfaceObject internals.  They act on the started
    // device and return OperationNotReady before start(); the values a device
    // starts with are SecureCommissioningOptions.
    // -----------------------------------------------------------------------

    /// Apply an ETS tool key (16-byte AES-128) to this device.
    ///
    /// The tool key is stored in the Security Interface Object and allows ETS
    /// to open a secure management session.  If KNX Data Secure mode is not
    /// yet enabled, it is enabled automatically.
    ///
    /// Must be called after start().
    util::Result<void> applyEtsToolKey(const std::array<uint8_t, 16>& key)
    {
        const auto enabled = _runtime.enableSecurityMode();
        if (enabled.isError()) {
            return enabled;
        }
        return _runtime.applyToolKey(key);
    }

    /// Apply a group key for a specific group address (16-byte AES-128).
    ///
    /// The key is stored in the Security Interface Object and is used to
    /// authenticate and encrypt group communication for that address.
    ///
    /// Must be called after start().
    util::Result<void> applyEtsGroupKey(GroupAddress address, const std::array<uint8_t, 16>& key)
    {
        return _runtime.applyGroupKey(address, key);
    }

    /// Install the factory Tool Key (FDSK) without asserting Security Mode.
    ///
    /// This is what a device should do at power-up. PID_SECURITY_MODE belongs
    /// to ETS: a device leaves the factory with it off, is commissioned over
    /// plain management by a project that does not use security, and has the
    /// mode switched on by ETS at the end of a secure download. The property
    /// is persisted, so that decision survives a restart -- and firmware that
    /// re-asserts the mode on every boot overwrites it, leaving a device no
    /// unsecured project can ever commission.
    ///
    /// The key is still installed, so ETS can open a secured exchange whenever
    /// it chooses to. Capability without enforcement; see
    /// SecureApplicationLayer::securityUsable().
    ///
    /// Must be called after start().
    util::Result<void> installFactoryToolKey(const std::array<uint8_t, 16>& key)
    {
        return _runtime.applyToolKey(key);
    }

    /// Turn on KNX Data Secure without touching the Tool Key.
    ///
    /// Needed by firmware that provisions a factory key once and must not
    /// re-apply it on later boots: ETS replaces the Tool Key during secure
    /// commissioning, and that key is restored from persistence.
    ///
    /// Must be called after start().
    util::Result<void> enableSecurityMode()
    {
        return _runtime.enableSecurityMode();
    }

    /// Returns true if KNX Data Secure mode is currently enabled on this device.
    bool isSecurityEnabled() const
    {
        return _runtime.isSecurityEnabled();
    }

    /// Returns true if an ETS tool key has been applied to this device.
    bool hasEtsToolKey() const
    {
        return _runtime.hasToolKey();
    }

    /// Returns true if the Tool Key in force is @p key.
    ///
    /// Use this, not hasEtsToolKey(), to decide whether a device certificate
    /// built from @p key is still worth printing: hasEtsToolKey() is true for
    /// any key at all, including one ETS installed over the factory key.
    bool toolKeyInForceIs(const std::array<uint8_t, 16>& key) const
    {
        return _runtime.toolKeyMatches(key);
    }
#endif  // KNX_SECURE_ENABLED

    const auto& knxPersistenceIdentity() const
    {
        return _knxPersistenceIdentity;
    }

    std::string_view knxPersistenceNamespace() const
    {
        return _knxPersistenceIdentity.storageNamespace;
    }

    bool isProgrammingModeActive() const
    {
        return _runtime.isProgrammingModeActive();
    }

    void toggleProgrammingMode()
    {
        _runtime.toggleProgrammingMode();
    }

    void requestProgrammingMode(bool active)
    {
        if (_runtime.isProgrammingModeActive() != active) {
            _runtime.toggleProgrammingMode();
        }
    }

    const auto& definition() const
    {
        return _definition;
    }

    /// Compiled endpoint view: runtime communication-object descriptors plus the
    /// export descriptors that carry each port's key and display name. Useful
    /// for diagnostics such as reporting which ports a project left unlinked.
    /// Built on demand and returned by value (it is as large as the port table's
    /// descriptors, which the runtime does not keep in RAM); `const auto&`
    /// bound to the result keeps it alive for the scope.
    auto compiledEndpoint() const
    {
        return _runtime.compiledDefinition();
    }

private:
    template <typename DefinitionT, size_t Cap, typename... Bindings>
    friend auto expert::bindDemoGroupAddresses(CommissionedProductRuntime<DefinitionT, Cap>& runtime,
                                               Bindings&&... bindings) -> util::Result<void>;

    template <typename DefinitionT, size_t Cap>
    friend bool expert::loadParameterImage(CommissionedProductRuntime<DefinitionT, Cap>& runtime,
                                           std::span<const uint8_t> image);

    template <typename... Bindings>
    auto bindCommissionedGroupAddressesForExpertUse(Bindings&&... bindings) -> util::Result<void>
    {
        return endpoint::bindGroupAddresses(_runtime, std::forward<Bindings>(bindings)...);
    }

    /// Replace the whole image the way a completed ETS download does.
    bool loadParameterImageForExpertUse(std::span<const uint8_t> image)
    {
        return _runtime.loadParameterImageForExpertUse(image);
    }

    void wireParameters()
    {
        if constexpr (detail::kHasParameters<parameters_type>) {
            _runtime.onParameterImageChanged([this]() { _parametersState.notifyChanged(); });
        }
    }

    product_definition_type _definition;
    ResolvedKnxPersistenceIdentity _knxPersistenceIdentity;
    detail::ParameterState<parameters_type> _parametersState;
    LifecycleChangedCallback _lifecycleCallback;
    DeviceLifecycleState _lastLifecycleState{DeviceLifecycleState::Uncommissioned};
    endpoint_runtime_type _runtime;
    bool _startInProgrammingMode{false};
    CommissionedTransportPath _transportPath{CommissionedTransportPath::NotStarted};
#if KNX_SECURE_ENABLED
    SecureCommissioningOptions _secureOptions{};
#endif
#if KNX_FEATURE_NETIP && KNX_SECURE_ENABLED
    /// Held by pointer in the routing physical, hence on the heap.
    std::unique_ptr<detail::PersistentMcTimerStore> _mcTimerStore;

    // KNXnet/IP Parameter Object, 03_08_09 §2.3.1.
    static constexpr application::PropertyID kPidBackboneKey = static_cast<application::PropertyID>(91);
    static constexpr application::PropertyID kPidMulticastLatencyTolerance = static_cast<application::PropertyID>(95);
    static constexpr application::PropertyID kPidSyncLatencyFraction = static_cast<application::PropertyID>(96);

    /// The ex-factory values (§2.3.1.6.3, §2.3.1.7.3) where nothing is stored,
    /// so that what ETS reads is what the device uses.
    void seedSecureRoutingLatencyDefaults()
    {
        const physical::IpRoutingPhysical::LatencySettings defaults{};
        if (_runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                  kPidMulticastLatencyTolerance).empty()) {
            const std::array<uint8_t, 2> value{static_cast<uint8_t>(defaults.latencyToleranceMs >> 8),
                                               static_cast<uint8_t>(defaults.latencyToleranceMs & 0xFFu)};
            (void)_runtime.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                      kPidMulticastLatencyTolerance, value);
        }
        if (_runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                  kPidSyncLatencyFraction).empty()) {
            const std::array<uint8_t, 1> value{defaults.syncLatencyFraction};
            (void)_runtime.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                      kPidSyncLatencyFraction, value);
        }
    }

    /// Read on the routing receive task once a second; see
    /// IpRoutingPhysical::setLatencySettingsSource.
    physical::IpRoutingPhysical::LatencySettings secureRoutingLatencySettings() const
    {
        physical::IpRoutingPhysical::LatencySettings settings{};
        const auto tolerance = _runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                                     kPidMulticastLatencyTolerance);
        if (tolerance.size() == 2) {
            const uint16_t value = static_cast<uint16_t>((tolerance[0] << 8) | tolerance[1]);
            // Zero would discard every frame not from the future; read it as unset.
            if (value != 0) {
                settings.latencyToleranceMs = value;
            }
        }
        const auto fraction = _runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                                    kPidSyncLatencyFraction);
        if (fraction.size() == 1) {
            settings.syncLatencyFraction = fraction[0];
        }
        return settings;
    }
#endif

    // SecureCommissioningOptions describe the device ex-factory, so they go
    // in before init() as factory defaults: what ETS downloaded (Security
    // Mode, its own Tool Key) and the device stored comes back over them at
    // every start. Applying them after start(), as this used to, put the
    // device back on its factory Tool Key behind ETS's back at every boot.
    void applySecureOptions()
    {
#if KNX_SECURE_ENABLED
        (void)_runtime.setFactorySecurityDefaults(_secureOptions.securityModeAtFactoryReset,
                                                  _secureOptions.factoryToolKey);
#endif
    }
};

template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              CommissionedProductOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{
            .persistence = options.persistence,
            .standaloneDemoIndividualAddress = options.standaloneDemoIndividualAddress,
            .mediumBackend = nullptr,
            .restoreKnxStateOnBoot = options.restoreKnxStateOnBoot,
            .startInProgrammingMode = options.startInProgrammingMode,
#if KNX_SECURE_ENABLED
            .secure = options.secure,
#endif
        });

    auto startResult = runtime->start(platform, std::move(options.mediumBackend));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    return startCommissionedProduct(
        platform,
        definition,
        std::move(bindings),
        CommissionedProductOptions{
            .mediumBackend = std::move(mediumBackend),
        });
}

#if KNX_FEATURE_COUPLER
/**
 * @brief Overload: start the product as a KNX coupler over two TP1 ports.
 *
 * The result is an ordinary commissioned product that also routes. Endpoints,
 * parameters, ETS export and persistence work exactly as for a single-port
 * device; the Router Object and the filter table are published and bound to
 * the forwarding path automatically.
 *
 * @code
 * auto runtime = startCommissionedProduct(
 *     platform, kCouplerProduct, std::move(bindings),
 *     CouplerOptions{
 *         .primary   = createTp1Backend(mainLinePins),
 *         .secondary = createTp1Backend(subLinePins),
 *     });
 * @endcode
 */
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              CouplerOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{
            .mediumBackend = nullptr,
#if KNX_SECURE_ENABLED
            .secure = options.secure,
#endif
        });

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_FEATURE_COUPLER

/// Overload: start on a stack port the caller assembled (another medium, or a
/// simulated bus in a test). `options.mediumBackend` is not used.
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<bau::BusAccessStackPort> stackPort,
                              CommissionedProductOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    options.mediumBackend = nullptr;
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        std::move(options));

    auto startResult = runtime->start(platform, std::move(stackPort));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

#if KNX_SECURE_ENABLED
/// Overload: assemble with a TP1 MAC physical layer (the bit-bang backend),
/// with the device's ex-factory Data Secure state.
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::Tp1MacPhysical> physical,
                              SecureCommissioningOptions secure)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    CommissionedProductOptions options{};
    options.secure = std::move(secure);
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        std::move(options));

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_SECURE_ENABLED

/// Overload: assemble with a TP1 MAC physical layer, no Data Secure defaults.
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::Tp1MacPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
#if KNX_SECURE_ENABLED
    return startCommissionedProduct(platform, definition, std::move(bindings), std::move(physical),
                                    SecureCommissioningOptions{});
#else
    CommissionedProductOptions options{};
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        std::move(options));

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
#endif
}

#if KNX_IP_TUNNELING
/// Overload: assemble with an IP tunneling physical layer (KNXnet/IP tunneling).
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::IpTunnelingPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
/// Overload: assemble with an IP routing physical layer (KNXnet/IP routing).
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::IpRoutingPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

#if KNX_IP_TUNNELING
/// Overload: assemble with a KNXnet/IP tunneling managed options struct.
///
/// The platform's `NetworkInterface` is used automatically. Set `options.host`
/// to the KNXnet/IP gateway IP address before calling.
///
/// Transport path: `CommissionedTransportPath::IpTunnelingManaged`
/// Support tier:   `CommissionedSupportTier::Functional`
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              IpTunnelingOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_IP_TUNNELING

/// Overload: assemble with a KNXnet/IP routing managed options struct.
///
/// The platform's `NetworkInterface` is used automatically. Set
/// `options.multicastGroup` to the desired KNX multicast group
/// (standard: 224.0.23.12) before calling.
///
/// Transport path: `CommissionedTransportPath::IpRoutingManaged`
/// Support tier:   `CommissionedSupportTier::Functional`
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              IpRoutingOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and it cannot be moved: its callbacks
    // capture `this`.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_FEATURE_NETIP

/// The communication objects and identity of a product, for the exporter.
/// Its parameters are exported as blocks by parameter_layout_export.hpp;
/// exportCommissionedProductToJson() in commissioned_product_export.hpp
/// writes both.
template <typename EndpointDefinitionT, typename ParametersT>
constexpr auto makeCommissionedExportDescriptor(
    const CommissionedProductDefinition<EndpointDefinitionT, ParametersT>& definition)
{
    return compileEndpointDefinition(definition.endpointDefinition).exportDescriptor;
}

} // namespace knx::product
