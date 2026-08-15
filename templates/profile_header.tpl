#ifndef {{HEADER_GUARD}}
#define {{HEADER_GUARD}}

// Generated from .knxprod input.
// Product: {{PRODUCT_DISPLAY_NAME}}
// Profile key: {{PROFILE_KEY}}
// ManufacturerId: {{MANUFACTURER_ID}}

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

#include "knx/application/dpt.hpp"
#include "knx/product/export_descriptor.hpp"
#include "knx/product/fixed_datapoint_support.hpp"
#include "knx/product/internal/fixed_datapoint_profile_support.hpp"
#include "knx/product/linked_datapoint_pair.hpp"
#include "knx/types.hpp"

namespace knx::product::{{NAMESPACE}} {

enum class ObjectId : uint16_t {
{{OBJECT_ENUMS}}
};

using DatapointDescriptor = knx::product::BasicDatapointDescriptor<ObjectId>;

struct Identity {
    static constexpr const char* kProfileName = "{{PROFILE_KEY}}";
    static constexpr const char* kProductDisplayName = "{{PRODUCT_DISPLAY_NAME}}";
    static constexpr ManufacturerId kManufacturerId = ManufacturerId{ {{MANUFACTURER_ID}} };
    static constexpr knx::product::Medium kMedium = {{MEDIUM_LITERAL}};
};

using Policy = knx::product::internal::FixedDatapointPolicy;

struct MediumSelection {
    static constexpr knx::product::Medium kMedium = {{MEDIUM_LITERAL}};
};

using Capacities = knx::product::internal::BasicDatapointCapacities<
    {{GROUP_OBJECT_COUNT}},
    {{ADDRESS_TABLE_ENTRIES}},
    {{ASSOCIATION_ENTRIES}},
    {{AUTO_RESPONSE_QUEUE_CAPACITY}},
    {{SEND_OUTCOME_QUEUE_CAPACITY}}>;

struct ProductDefinition {
    static inline constexpr knx::product::internal::StaticProductDefinition<ObjectId, Capacities::kDatapointCount, 0> kDefinition{
        {
            Identity::kProfileName,
            Identity::kProductDisplayName,
            Identity::kManufacturerId,
            Identity::kMedium,
        },
        knx::product::internal::DeviceMetadata{
            Identity::kManufacturerId,
            0,
            254,
            6,
            3,
            0,
            Identity::kProfileName,
            Identity::kProductDisplayName,
        },
        knx::product::internal::ApplicationProgramMetadata{
            {{APPLICATION_NUMBER}},
            {{APPLICATION_VERSION}},
            0,
            Identity::kManufacturerId.raw,
            0,
            Identity::kProfileName,
        },
        knx::product::internal::makeProductFeatureSet(Policy::kPersistenceEnabled,
                                  Policy::kSecurityEnabled,
                                  Policy::kReadResponsesEnabled,
                                  Policy::kVerboseDiagnostics,
                                  Policy::kAutomaticResponseMode),
        knx::product::internal::makeProductCapacities(Capacities::kDatapointCount,
                                  Capacities::kGroupAddressCapacity,
                                  Capacities::kDatapointLinkCapacity,
                                  Capacities::kAutoResponseQueueCapacity,
                                  Capacities::kTransmissionOutcomeQueueCapacity),
        "{{PERSISTENCE_NAMESPACE}}",
        {},
        {{
{{OBJECT_DESCRIPTORS}}
        }},
    };
};

using DeclarativeProfile = knx::product::internal::BasicProductProfile<
    ProductDefinition,
    knx::product::IndependentDatapointPolicy<ObjectId>,
    {{PRIMARY_TAG_LITERAL}},
    {{SECONDARY_TAG_LITERAL}},
    ObjectId::{{PRIMARY_OBJECT_ENUM}},
    ObjectId::{{SECONDARY_OBJECT_ENUM}}>;

struct Profile : DeclarativeProfile {};

using DescriptorAccess = knx::product::internal::BasicDatapointProfileDescriptorAccess<Profile, ObjectId>;

static inline constexpr const char* kFunctionNames[{{GROUP_OBJECT_COUNT}}] = {
{{FUNCTION_NAMES}}
};

static constexpr knx::product::StaticExportDescriptor<ProductDefinition::kDefinition.kDatapointCount, {{PARAMETER_COUNT}}>
    kExportDescriptor{
        knx::product::ExportProductIdentity{
            ProductDefinition::kDefinition.identity.profileKey,
            ProductDefinition::kDefinition.identity.productDisplayName,
            ProductDefinition::kDefinition.identity.manufacturerId,
            ProductDefinition::kDefinition.identity.medium,
            knx::product::ExportApplicationProgramIdentity{ {{APPLICATION_NUMBER}}, {{APPLICATION_VERSION}} },
        },
        knx::product::ExportFeatureFlags{ProductDefinition::kDefinition.features.persistenceEnabled,
                                         ProductDefinition::kDefinition.features.securityEnabled,
                                         ProductDefinition::kDefinition.features.readResponsesEnabled,
                                         ProductDefinition::kDefinition.features.verboseDiagnostics},
        knx::product::makeExportCapacities(ProductDefinition::kDefinition.capacities.datapointCount,
                                           ProductDefinition::kDefinition.capacities.groupAddressCapacity,
                                           ProductDefinition::kDefinition.capacities.datapointLinkCapacity,
                                           ProductDefinition::kDefinition.capacities.autoResponseQueueCapacity,
                                           ProductDefinition::kDefinition.capacities.transmissionOutcomeQueueCapacity),
        {
    {{EXPORT_OBJECTS}}
        },
        {
    {{PARAMETER_DESCRIPTORS}}
        },
};

} // namespace knx::product::{{NAMESPACE}}

#endif // {{HEADER_GUARD}}
