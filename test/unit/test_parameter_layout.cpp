// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/parameter_layout.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace knx::product;

namespace {

enum class Function : uint8_t { Disabled = 0, Switch = 1, Dimming = 3 };
enum class Action : uint8_t { Nothing, On, Off, Toggle };

inline constexpr auto kFunctionOptions = params::options<Function>(
    {{Function::Disabled, "Disabled"}, {Function::Switch, "Switch"}, {Function::Dimming, "Dimming"}});
inline constexpr auto kActionOptions = params::options<Action>(
    {{Action::Nothing, "Nothing"}, {Action::On, "On"}, {Action::Off, "Off"}, {Action::Toggle, "Toggle"}});

enum class Dev : uint8_t { HeartbeatMinutes, Offset };
enum class In : uint8_t { Function = 4, DebounceMs = 9, Rising = 2, LongPressMs = 7, Inverted = 1 };
enum class Out : uint8_t { OnDelaySeconds, Trim };

inline constexpr auto kDeviceBlock = params::parameterBlock<Dev>(
    {.key = "device", .name = "Device"},
    params::param<Dev::HeartbeatMinutes, uint8_t>({.name = "In operation", .def = 0, .max = 60, .unit = "min"}),
    params::param<Dev::Offset, Dpt9Float>({.name = "Offset", .def = -1.5f, .unit = "K"}));

inline constexpr auto kInputBlock = params::parameterBlock<In>(
    {.key = "input", .name = "Input {n}"},
    params::param<In::Function, Function>({.name = "Function", .def = Function::Disabled,
                                           .options = kFunctionOptions}),
    params::param<In::DebounceMs, uint8_t>({.name = "Debounce time", .def = 30, .min = 10, .max = 200,
                                            .unit = "ms"}),
    params::param<In::Rising, Action>({.name = "On closing", .def = Action::Toggle, .options = kActionOptions,
                                       .visible = params::when<In::Function>(Function::Switch)}),
    params::param<In::LongPressMs, uint16_t>({.name = "Long press after", .def = 500, .min = 300, .max = 3000,
                                              .unit = "ms",
                                              .visible = params::whenAny<In::Function>(Function::Switch,
                                                                                       Function::Dimming)}),
    params::param<In::Inverted, bool>({.name = "Normally closed"}));

inline constexpr auto kOutputBlock = params::parameterBlock<Out>(
    {.key = "output", .name = "Output {n}"},
    params::param<Out::OnDelaySeconds, uint16_t>({.name = "Switch-on delay", .max = 3600, .unit = "s"}),
    params::param<Out::Trim, int16_t>({.name = "Trim", .def = -5, .min = -100, .max = 100}));

inline constexpr auto kLayout =
    params::parameterLayout(kDeviceBlock, params::repeat<8>(kInputBlock), params::repeat<3>(kOutputBlock));

using Layout = std::remove_cvref_t<decltype(kLayout)>;
using InputBlock = std::remove_cvref_t<decltype(kInputBlock)>;

// The layout is decided at compile time.
static_assert(InputBlock::kStride == 6u);
static_assert(InputBlock::kOffset<In::Function> == 0u);
static_assert(InputBlock::kOffset<In::DebounceMs> == 1u);
static_assert(InputBlock::kOffset<In::Rising> == 2u);
static_assert(InputBlock::kOffset<In::LongPressMs> == 3u);
static_assert(InputBlock::kOffset<In::Inverted> == 5u);
static_assert(Layout::kBase<Dev> == 0u);
static_assert(Layout::kBase<In> == 3u);
static_assert(Layout::kBase<Out> == 3u + 8u * 6u);
static_assert(Layout::kCount<In> == 8u);
static_assert(Layout::kCount<Dev> == 1u);
static_assert(Layout::kSize == 3u + 8u * 6u + 3u * 4u);
static_assert(Layout::kDeclaredParameters == 9u);
static_assert(std::same_as<InputBlock::value_type_of<In::Function>, Function>);
static_assert(std::same_as<InputBlock::value_type_of<In::LongPressMs>, uint16_t>);

// The fingerprint follows types, ids, order and repeat counts, not names.
inline constexpr auto kRenamed = params::parameterLayout(
    params::parameterBlock<Out>({.key = "x", .name = "Other name"},
                                params::param<Out::OnDelaySeconds, uint16_t>({.name = "Renamed"}),
                                params::param<Out::Trim, int16_t>({.name = "Renamed too"})));
inline constexpr auto kSameShape = params::parameterLayout(kOutputBlock);
inline constexpr auto kSwapped = params::parameterLayout(
    params::parameterBlock<Out>({}, params::param<Out::Trim, int16_t>({}),
                                params::param<Out::OnDelaySeconds, uint16_t>({})));
inline constexpr auto kRetyped = params::parameterLayout(
    params::parameterBlock<Out>({}, params::param<Out::OnDelaySeconds, uint16_t>({}),
                                params::param<Out::Trim, uint16_t>({})));
inline constexpr auto kTwice = params::parameterLayout(params::repeat<2>(kOutputBlock));
static_assert(kRenamed.kFingerprint == kSameShape.kFingerprint);
static_assert(kSwapped.kFingerprint != kSameShape.kFingerprint);
static_assert(kRetyped.kFingerprint != kSameShape.kFingerprint);
static_assert(kTwice.kFingerprint != kSameShape.kFingerprint);

inline constexpr auto& kDefaults = params::kDefaultImageOf<kLayout>;
inline constexpr auto& kBounds = params::kBoundsOf<kLayout>;
static_assert(kDefaults.size() == Layout::kSize);
static_assert(kDefaults[0] == 0u);
// -1.5 as DPT 9: mantissa -150, exponent 0.
static_assert(kDefaults[1] == 0x87u && kDefaults[2] == 0x6Au);
static_assert(kDefaults[Layout::kBase<In> + 1u] == 30u);
static_assert(kDefaults[Layout::kBase<In> + 7u * 6u + 3u] == 0x01u
              && kDefaults[Layout::kBase<In> + 7u * 6u + 4u] == 0xF4u);

std::array<uint8_t, Layout::kSize> defaults()
{
    return kDefaults;
}

void put16(std::array<uint8_t, Layout::kSize>& image, size_t at, uint16_t value)
{
    image[at] = static_cast<uint8_t>(value >> 8u);
    image[at + 1u] = static_cast<uint8_t>(value);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_view_reads_defaults_of_every_type(void)
{
    const auto image = defaults();
    const params::ViewFor<kLayout> view(image);

    TEST_ASSERT_EQUAL_UINT8(0, view.block<Dev>().get<Dev::HeartbeatMinutes>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.5f, view.block<Dev>().get<Dev::Offset>());
    for (size_t channel = 0; channel < view.count<In>(); ++channel) {
        const auto in = view.block<In>(channel);
        TEST_ASSERT_TRUE(in.get<In::Function>() == Function::Disabled);
        TEST_ASSERT_EQUAL_UINT8(30, in.get<In::DebounceMs>());
        TEST_ASSERT_TRUE(in.get<In::Rising>() == Action::Toggle);
        TEST_ASSERT_EQUAL_UINT16(500, in.get<In::LongPressMs>());
        TEST_ASSERT_FALSE(in.get<In::Inverted>());
    }
    TEST_ASSERT_EQUAL_INT16(-5, view.block<Out>(2).get<Out::Trim>());
    TEST_ASSERT_EQUAL_UINT16(0, view.block<Out>(2).get<Out::OnDelaySeconds>());
}

void test_view_reads_the_instance_it_was_asked_for(void)
{
    auto image = defaults();
    const size_t base = Layout::kBase<In>;
    image[base + 5u * 6u + 0u] = static_cast<uint8_t>(Function::Dimming);
    put16(image, base + 5u * 6u + 3u, 1234);
    put16(image, Layout::kBase<Out> + 1u * 4u + 2u, static_cast<uint16_t>(int16_t{-77}));

    const params::ViewFor<kLayout> view(image);
    TEST_ASSERT_TRUE(view.block<In>(5).get<In::Function>() == Function::Dimming);
    TEST_ASSERT_EQUAL_UINT16(1234, view.block<In>(5).get<In::LongPressMs>());
    TEST_ASSERT_TRUE(view.block<In>(4).get<In::Function>() == Function::Disabled);
    TEST_ASSERT_TRUE(view.block<In>(6).get<In::Function>() == Function::Disabled);
    TEST_ASSERT_EQUAL_INT16(-77, view.block<Out>(1).get<Out::Trim>());
    TEST_ASSERT_EQUAL_INT16(-5, view.block<Out>(0).get<Out::Trim>());
}

void test_an_instance_past_the_end_reads_the_last_one(void)
{
    auto image = defaults();
    put16(image, Layout::kBase<Out> + 2u * 4u, 3000);
    const params::ViewFor<kLayout> view(image);
    TEST_ASSERT_EQUAL_UINT16(3000, view.block<Out>(3).get<Out::OnDelaySeconds>());
    TEST_ASSERT_EQUAL_UINT16(3000, view.block<Out>(1000).get<Out::OnDelaySeconds>());
}

void test_default_image_is_within_bounds(void)
{
    TEST_ASSERT_TRUE(params::imageWithinBounds(kBounds, defaults()));
}

void test_bounds_reject_a_value_outside_its_range(void)
{
    const size_t in3 = Layout::kBase<In> + 3u * 6u;

    auto image = defaults();
    image[in3 + 1u] = 9; // debounce below 10
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));
    image[in3 + 1u] = 10;
    TEST_ASSERT_TRUE(params::imageWithinBounds(kBounds, image));
    image[in3 + 1u] = 200;
    TEST_ASSERT_TRUE(params::imageWithinBounds(kBounds, image));
    image[in3 + 1u] = 201;
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));

    image = defaults();
    image[in3 + 0u] = 4; // past the last function option
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));

    image = defaults();
    image[in3 + 5u] = 2; // a bool is 0 or 1
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));

    image = defaults();
    put16(image, in3 + 3u, 3001);
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));

    image = defaults();
    put16(image, Layout::kBase<Out> + 2u, static_cast<uint16_t>(int16_t{-101}));
    TEST_ASSERT_FALSE(params::imageWithinBounds(kBounds, image));
    put16(image, Layout::kBase<Out> + 2u, static_cast<uint16_t>(int16_t{-100}));
    TEST_ASSERT_TRUE(params::imageWithinBounds(kBounds, image));

    image = defaults();
    put16(image, 1u, 0x7FFFu); // floats are not range-checked
    TEST_ASSERT_TRUE(params::imageWithinBounds(kBounds, image));
}

void test_bounds_reject_an_image_that_is_too_short(void)
{
    const auto image = defaults();
    TEST_ASSERT_FALSE(
        params::imageWithinBounds(kBounds, std::span<const uint8_t>(image).first(Layout::kSize - 1u)));
}

void test_dpt9_round_trips_through_the_image(void)
{
    for (const float value : {0.0f, 0.01f, -0.01f, 20.47f, 21.0f, -30.0f, 670.0f, -273.0f, 1000.0f}) {
        std::array<uint8_t, 2> octets{};
        params::detail::encode(Dpt9Float{value}, octets.data());
        const float back = params::detail::decode<Dpt9Float>(octets.data());
        // Above the 0.01 range the step doubles with every exponent.
        TEST_ASSERT_FLOAT_WITHIN(value > 20.47f || value < -20.48f ? 0.64f : 0.006f, value, back);
    }
}

void test_export_walk_reports_offsets_and_conditions(void)
{
    std::vector<params::ParameterInfo> seen;
    params::forEachParameter(kLayout, [&](const params::ParameterInfo& info) { seen.push_back(info); });

    TEST_ASSERT_EQUAL_UINT(Layout::kDeclaredParameters, seen.size());

    const auto& function = seen[2];
    TEST_ASSERT_EQUAL_STRING("input", std::string(function.blockKey).c_str());
    TEST_ASSERT_EQUAL_STRING("Function", std::string(function.name).c_str());
    TEST_ASSERT_EQUAL_UINT(3, function.blockBase);
    TEST_ASSERT_EQUAL_UINT(6, function.blockStride);
    TEST_ASSERT_EQUAL_UINT(8, function.blockCount);
    TEST_ASSERT_EQUAL_UINT16(4, function.id);
    TEST_ASSERT_TRUE(function.kind == ExportParameterValueKind::Enum);
    TEST_ASSERT_EQUAL_UINT(3, function.options.size());
    TEST_ASSERT_TRUE(function.visible.op == params::ConditionOp::Always);

    const auto& longPress = seen[5];
    TEST_ASSERT_EQUAL_UINT(3, longPress.offset);
    TEST_ASSERT_EQUAL_UINT(2, longPress.width);
    TEST_ASSERT_EQUAL_DOUBLE(500.0, longPress.defaultValue);
    TEST_ASSERT_TRUE(longPress.visible.op == params::ConditionOp::AnyOf);
    // The condition points at "Function" of the same block.
    TEST_ASSERT_TRUE(longPress.visible.block == function.block);
    TEST_ASSERT_EQUAL_UINT16(function.id, longPress.visible.parameterId);
    TEST_ASSERT_EQUAL_UINT8(2, longPress.visible.valueCount);
    TEST_ASSERT_EQUAL_INT32(1, longPress.visible.values[0]);
    TEST_ASSERT_EQUAL_INT32(3, longPress.visible.values[1]);

    // Two parameters can share one option list: the exporter emits one type.
    TEST_ASSERT_TRUE(seen[4].options.data() == kActionOptions.data());

    // Offsets in the walk are the ones the view reads at.
    TEST_ASSERT_EQUAL_UINT(Layout::kBase<Out>, seen[7].blockBase);
    TEST_ASSERT_EQUAL_UINT(2, seen[8].offset);
    TEST_ASSERT_EQUAL_DOUBLE(-5.0, seen[8].defaultValue);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_view_reads_defaults_of_every_type);
    RUN_TEST(test_view_reads_the_instance_it_was_asked_for);
    RUN_TEST(test_an_instance_past_the_end_reads_the_last_one);
    RUN_TEST(test_default_image_is_within_bounds);
    RUN_TEST(test_bounds_reject_a_value_outside_its_range);
    RUN_TEST(test_bounds_reject_an_image_that_is_too_short);
    RUN_TEST(test_dpt9_round_trips_through_the_image);
    RUN_TEST(test_export_walk_reports_offsets_and_conditions);
    return UNITY_END();
}
