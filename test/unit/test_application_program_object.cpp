// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_application_program_object.cpp
 * @brief Unit tests for ApplicationProgramObject
 */

#include "knx/objects/application_program_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include <unity.h>
#include <vector>
#include <span>

using namespace knx;
using namespace knx::objects;

static util::Result<void> readAppProgramProperty(const ApplicationProgramObject& obj,
                                                 application::PropertyID id,
                                                 uint16_t elementCount,
                                                 std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeAppProgramProperty(ApplicationProgramObject& obj,
                                                  application::PropertyID id,
                                                  std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

static ApplicationProgramObject* appProg = nullptr;

void setUp() {
    appProg = new ApplicationProgramObject();
}

void tearDown() {
    delete appProg;
    appProg = nullptr;
}

// === Basic Properties Tests ===

void test_AppProgram_DefaultConstruction() {
    for (uint8_t byte : appProg->getProgramVersionBlock()) {
        TEST_ASSERT_EQUAL_UINT8(0, byte);
    }
    TEST_ASSERT_EQUAL_UINT16(0, appProg->getApplicationId());
    TEST_ASSERT_EQUAL_UINT8(0, appProg->getApplicationVersion());
    TEST_ASSERT_EQUAL_UINT16(0, appProg->getApplicationNumber());
    TEST_ASSERT_EQUAL_UINT8(0, appProg->getApplicationArea());
    TEST_ASSERT_EQUAL_UINT16(0, appProg->getApplicationManufacturer());
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
    TEST_ASSERT_FALSE(appProg->isValid()); // Not valid until configured
}

void test_AppProgram_ProgramVersion() {
    // PID_PROGRAM_VERSION is a PDT_GENERIC_05 block:
    // manufacturer (2B) + application number (2B) + version (1B).
    const std::array<uint8_t, 5> block = {0x00, 0xFA, 0x00, 0x01, 42};
    appProg->setProgramVersionBlock(block);
    const auto readBack = appProg->getProgramVersionBlock();
    TEST_ASSERT_EQUAL_UINT32(5, readBack.size());
    for (size_t i = 0; i < block.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(block[i], readBack[i]);
    }
}

void test_AppProgram_ApplicationId() {
    appProg->setApplicationId(0x1234);
    TEST_ASSERT_EQUAL_UINT16(0x1234, appProg->getApplicationId());
}

void test_AppProgram_ApplicationVersion() {
    appProg->setApplicationVersion(5);
    TEST_ASSERT_EQUAL_UINT8(5, appProg->getApplicationVersion());
}

void test_AppProgram_ApplicationNumber() {
    appProg->setApplicationNumber(0xABCD);
    TEST_ASSERT_EQUAL_UINT16(0xABCD, appProg->getApplicationNumber());
}

void test_AppProgram_ApplicationArea() {
    appProg->setApplicationArea(7);
    TEST_ASSERT_EQUAL_UINT8(7, appProg->getApplicationArea());
}

void test_AppProgram_ApplicationManufacturer() {
    appProg->setApplicationManufacturer(0x5678);
    TEST_ASSERT_EQUAL_UINT16(0x5678, appProg->getApplicationManufacturer());
}

void test_AppProgram_ProgramName() {
    appProg->setProgramName("Test Application");
    TEST_ASSERT_EQUAL_STRING("Test Application", appProg->getProgramName().c_str());
}

void test_AppProgram_ProgramDescription() {
    appProg->setProgramDescription("This is a test program");
    TEST_ASSERT_EQUAL_STRING("This is a test program", appProg->getProgramDescription().c_str());
}

// === Program State & Control Tests ===

void test_AppProgram_InitialState() {
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
    TEST_ASSERT_FALSE(appProg->isActive());
}

void test_AppProgram_StartProgram() {
    TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Start).isOk());
        TEST_ASSERT_EQUAL(ProgramState::Active, appProg->getProgramState());
        TEST_ASSERT_TRUE(appProg->isActive());
    TEST_ASSERT_TRUE(appProg->isActive());
}

void test_AppProgram_StopProgram() {
    TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Start).isOk());
    TEST_ASSERT_EQUAL(ProgramState::Active, appProg->getProgramState());
    TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Stop).isOk());
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
    TEST_ASSERT_FALSE(appProg->isActive());
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
    TEST_ASSERT_FALSE(appProg->isActive());
}

void test_AppProgram_ResetProgram() {
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Start).isOk());
        TEST_ASSERT_EQUAL(ProgramState::Active, appProg->getProgramState());
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Reset).isOk());
        TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
}

void test_AppProgram_InvalidStateTransitions() {
    // Cannot start when already active
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Start).isOk());
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Start).isError());
        // Cannot stop when inactive
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Reset).isOk());
        TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Stop).isError());
}

void test_AppProgram_ReloadProgram() {
    TEST_ASSERT_TRUE(appProg->executeProgramControl(ProgramControl::Reload).isOk());
    // State should be restored after reload
    TEST_ASSERT_EQUAL(ProgramState::Inactive, appProg->getProgramState());
}

// === Parameter Management Tests ===

void test_AppProgram_ParameterAddresses() {
    appProg->setParameterStart(MemoryAddress(0x1000));
    appProg->setParameterEnd(MemoryAddress(0x10FF));
    
    TEST_ASSERT_EQUAL_UINT16(0x1000, appProg->getParameterStart().raw);
    TEST_ASSERT_EQUAL_UINT16(0x10FF, appProg->getParameterEnd().raw);
    TEST_ASSERT_EQUAL_UINT16(256, appProg->getParameterSize());
    TEST_ASSERT_TRUE(appProg->hasParameters());
}

void test_AppProgram_NoParameters() {
    TEST_ASSERT_EQUAL_UINT16(0, appProg->getParameterSize());
    TEST_ASSERT_FALSE(appProg->hasParameters());
}

// The program data is the product's parameter image: a buffer the product
// binds, of a fixed size, that starts at the defaults.
void test_AppProgram_ProgramData_is_the_parameter_image() {
    TEST_ASSERT_EQUAL_INT(0, appProg->getProgramData().size());  // no parameters, no data
    TEST_ASSERT_FALSE(appProg->hasParameterImage());

    static const std::array<uint8_t, 4> defaults = {1, 2, 3, 4};
    std::array<uint8_t, 4> image{};
    appProg->bindParameterImage(ParameterImageBinding{
        .image = image,
        .defaults = defaults.data(),
        .fingerprint = 0xA1B2C3D4u,
        .withinBounds = [](std::span<const uint8_t> candidate) { return candidate[0] != 0xFFu; },
    });
    TEST_ASSERT_TRUE(appProg->hasParameterImage());
    TEST_ASSERT_EQUAL_MEMORY(defaults.data(), appProg->getProgramData().data(), 4);

    const std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC, 0xDD};
    appProg->setProgramData(std::span<const uint8_t>(data));
    TEST_ASSERT_EQUAL_INT(4, appProg->getProgramData().size());
    TEST_ASSERT_EQUAL_MEMORY(data.data(), image.data(), 4);

    // The stored record is the fingerprint and the image, and comes back.
    std::vector<uint8_t> record;
    appProg->appendParameterImageRecord(record);
    TEST_ASSERT_EQUAL_INT(8, record.size());
    TEST_ASSERT_EQUAL_HEX8(0xA1, record[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD4, record[3]);
    appProg->resetParameterImage();
    TEST_ASSERT_EQUAL_MEMORY(defaults.data(), image.data(), 4);
    appProg->restoreParameterImageRecord(record);
    TEST_ASSERT_EQUAL_MEMORY(data.data(), image.data(), 4);
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoaded, appProg->loadState());

    // A record of another layout is not taken into use.
    record[0] ^= 0x01u;
    appProg->restoreParameterImageRecord(record);
    TEST_ASSERT_EQUAL_MEMORY(defaults.data(), image.data(), 4);
    TEST_ASSERT_EQUAL_UINT8(loadstate::kUnloaded, appProg->loadState());

    // An image outside its bounds is refused as a whole.
    appProg->setLoadState(loadstate::kLoaded);
    image[0] = 0xFFu;
    TEST_ASSERT_FALSE(appProg->acceptParameterImage());
    TEST_ASSERT_EQUAL_MEMORY(defaults.data(), image.data(), 4);
    TEST_ASSERT_EQUAL_UINT8(loadstate::kError, appProg->loadState());

    // Not loaded: the record says so in one octet.
    record.clear();
    appProg->appendParameterImageRecord(record);
    TEST_ASSERT_EQUAL_INT(1, record.size());
}

// === Property Access Tests ===

void test_AppProgram_PropertyAccess_ProgramVersion() {
    // PDT_GENERIC_05: the whole 5-octet block is written/read as one element.
    std::vector<uint8_t> value = {0x00, 0xFA, 0x00, 0x01, 0x2A};
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ProgramVersion),
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT8(0x2A, appProg->getProgramVersionBlock()[4]);

    value.clear();
    TEST_ASSERT_TRUE(readAppProgramProperty(*appProg,
                                            static_cast<application::PropertyID>(AppProgramProperty::ProgramVersion),
                                            1,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(5, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFA, value[1]);
    TEST_ASSERT_EQUAL_UINT8(0x2A, value[4]);
}

void test_AppProgram_PropertyAccess_ApplicationId() {
    std::vector<uint8_t> value = {0x12, 0x34};
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ApplicationId),
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(0x1234, appProg->getApplicationId());
    
    value.clear();
    TEST_ASSERT_TRUE(readAppProgramProperty(*appProg,
                                            static_cast<application::PropertyID>(AppProgramProperty::ApplicationId),
                                            1,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x12, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, value[1]);
}

void test_AppProgram_PropertyAccess_ProgramName() {
    std::string name = "TestProg";
    std::vector<uint8_t> value(name.begin(), name.end());
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ProgramName),
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL_STRING("TestProg", appProg->getProgramName().c_str());
    const uint16_t expectedLength = static_cast<uint16_t>(value.size());
    value.clear();
    TEST_ASSERT_TRUE(readAppProgramProperty(*appProg,
                                            static_cast<application::PropertyID>(AppProgramProperty::ProgramName),
                                            expectedLength,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(8, value.size());
}

void test_AppProgram_PropertyAccess_ProgramState() {
    std::vector<uint8_t> value = {static_cast<uint8_t>(ProgramState::Active)};
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ProgramState),
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL(ProgramState::Active, appProg->getProgramState());
    
    value.clear();
    TEST_ASSERT_TRUE(readAppProgramProperty(*appProg,
                                            static_cast<application::PropertyID>(AppProgramProperty::ProgramState),
                                            1,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProgramState::Active), value[0]);
}

void test_AppProgram_PropertyAccess_ProgramControl() {
    // Program control is write-only
    std::vector<uint8_t> value = {static_cast<uint8_t>(ProgramControl::Start)};
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ProgramControl),
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL(ProgramState::Active, appProg->getProgramState());
}

void test_AppProgram_PropertyAccess_ParameterAddresses() {
    std::vector<uint8_t> startValue = {0x10, 0x00};
    std::vector<uint8_t> endValue = {0x10, 0xFF};
    
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ParameterStart),
                                             startValue)
                         .isOk());
    TEST_ASSERT_TRUE(writeAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(AppProgramProperty::ParameterEnd),
                                             endValue)
                         .isOk());
    
    TEST_ASSERT_EQUAL_UINT16(0x1000, appProg->getParameterStart().raw);
    TEST_ASSERT_EQUAL_UINT16(0x10FF, appProg->getParameterEnd().raw);
    TEST_ASSERT_EQUAL_UINT16(256, appProg->getParameterSize());
}

void test_AppProgram_PropertyAccess_ObjectType() {
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAppProgramProperty(*appProg,
                                            static_cast<application::PropertyID>(AppProgramProperty::ObjectType),
                                            1,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, value[1]); // Interface Object Type 3
}

void test_AppProgram_PropertyAccess_InvalidProperty() {
    std::vector<uint8_t> value;
    TEST_ASSERT_FALSE(readAppProgramProperty(*appProg,
                                             static_cast<application::PropertyID>(static_cast<AppProgramProperty>(255)),
                                             1,
                                             value)
                          .isOk());
    value = {0x01};
    TEST_ASSERT_FALSE(writeAppProgramProperty(*appProg,
                                              static_cast<application::PropertyID>(static_cast<AppProgramProperty>(255)),
                                              value)
                          .isOk());
}

// === Validation Tests ===

void test_AppProgram_IsValid_Default() {
    TEST_ASSERT_FALSE(appProg->isValid()); // Not valid until configured
}

void test_AppProgram_IsValid_PartialConfig() {
    appProg->setApplicationId(0x1234);
    TEST_ASSERT_FALSE(appProg->isValid()); // Still missing manufacturer and version
    
    appProg->setApplicationManufacturer(0x5678);
    TEST_ASSERT_FALSE(appProg->isValid()); // Still missing version
}

void test_AppProgram_IsValid_FullConfig() {
    appProg->setApplicationId(0x1234);
    appProg->setApplicationManufacturer(0x5678);
    const std::array<uint8_t, 5> block = {0x56, 0x78, 0x12, 0x34, 1};
    appProg->setProgramVersionBlock(block);
    TEST_ASSERT_TRUE(appProg->isValid());
}

// === Main Test Runner ===

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    // Basic properties
    RUN_TEST(test_AppProgram_DefaultConstruction);
    RUN_TEST(test_AppProgram_ProgramVersion);
    RUN_TEST(test_AppProgram_ApplicationId);
    RUN_TEST(test_AppProgram_ApplicationVersion);
    RUN_TEST(test_AppProgram_ApplicationNumber);
    RUN_TEST(test_AppProgram_ApplicationArea);
    RUN_TEST(test_AppProgram_ApplicationManufacturer);
    RUN_TEST(test_AppProgram_ProgramName);
    RUN_TEST(test_AppProgram_ProgramDescription);
    
    // Program state & control
    RUN_TEST(test_AppProgram_InitialState);
    RUN_TEST(test_AppProgram_StartProgram);
    RUN_TEST(test_AppProgram_StopProgram);
    RUN_TEST(test_AppProgram_ResetProgram);
    RUN_TEST(test_AppProgram_InvalidStateTransitions);
    RUN_TEST(test_AppProgram_ReloadProgram);
    
    // Parameter management
    RUN_TEST(test_AppProgram_ParameterAddresses);
    RUN_TEST(test_AppProgram_NoParameters);
    RUN_TEST(test_AppProgram_ProgramData_is_the_parameter_image);
    
    // Property access
    RUN_TEST(test_AppProgram_PropertyAccess_ProgramVersion);
    RUN_TEST(test_AppProgram_PropertyAccess_ApplicationId);
    RUN_TEST(test_AppProgram_PropertyAccess_ProgramName);
    RUN_TEST(test_AppProgram_PropertyAccess_ProgramState);
    RUN_TEST(test_AppProgram_PropertyAccess_ProgramControl);
    RUN_TEST(test_AppProgram_PropertyAccess_ParameterAddresses);
    RUN_TEST(test_AppProgram_PropertyAccess_ObjectType);
    RUN_TEST(test_AppProgram_PropertyAccess_InvalidProperty);
    
    // Validation
    RUN_TEST(test_AppProgram_IsValid_Default);
    RUN_TEST(test_AppProgram_IsValid_PartialConfig);
    RUN_TEST(test_AppProgram_IsValid_FullConfig);
    
    return UNITY_END();
}
