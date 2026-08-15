// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_logging.cpp
 * @brief Unit tests for logging system
 */

#include "unity.h"
#include "knx/util/log.hpp"
#include <cstring>
#include <vector>
#include <string>

using namespace knx;

// Test sink to capture log output
struct LogEntry {
    log::Level level;
    std::string tag;
    std::string message;
};

static std::vector<LogEntry> capturedLogs;

void testSink(log::Level level, const char* tag, const char* fmt, va_list args) {
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    
    LogEntry entry;
    entry.level = level;
    entry.tag = tag ? tag : "";
    entry.message = buffer;
    capturedLogs.push_back(entry);
}

void setUp(void) {
    capturedLogs.clear();
    log::setSink(testSink);
    log::setLevel(log::Level::Verbose);  // Allow all logs
    log::clearAllModuleLevels();
}

void tearDown(void) {
    log::setSink(nullptr);  // Reset to default
    log::setLevel(log::Level::Info);
    log::clearAllModuleLevels();
}

// ============================================================================
// Global Log Level Tests
// ============================================================================

void test_log_level_error_only() {
    log::setLevel(log::Level::Error);
    
    KNX_LOGE("TEST", "Error message");
    KNX_LOGW("TEST", "Warn message");
    KNX_LOGI("TEST", "Info message");
    
    TEST_ASSERT_EQUAL(1, capturedLogs.size());
    TEST_ASSERT_EQUAL(log::Level::Error, capturedLogs[0].level);
}

void test_log_level_warn() {
    log::setLevel(log::Level::Warn);
    
    KNX_LOGE("TEST", "Error message");
    KNX_LOGW("TEST", "Warn message");
    KNX_LOGI("TEST", "Info message");
    KNX_LOGD("TEST", "Debug message");
    
    TEST_ASSERT_EQUAL(2, capturedLogs.size());
    TEST_ASSERT_EQUAL(log::Level::Error, capturedLogs[0].level);
    TEST_ASSERT_EQUAL(log::Level::Warn, capturedLogs[1].level);
}

void test_log_level_info() {
    log::setLevel(log::Level::Info);
    
    KNX_LOGE("TEST", "Error message");
    KNX_LOGW("TEST", "Warn message");
    KNX_LOGI("TEST", "Info message");
    KNX_LOGD("TEST", "Debug message");
    KNX_LOGV("TEST", "Verbose message");
    
    TEST_ASSERT_EQUAL(3, capturedLogs.size());
    TEST_ASSERT_EQUAL(log::Level::Error, capturedLogs[0].level);
    TEST_ASSERT_EQUAL(log::Level::Warn, capturedLogs[1].level);
    TEST_ASSERT_EQUAL(log::Level::Info, capturedLogs[2].level);
}

void test_log_level_debug() {
    log::setLevel(log::Level::Debug);
    
    KNX_LOGE("TEST", "Error message");
    KNX_LOGW("TEST", "Warn message");
    KNX_LOGI("TEST", "Info message");
    KNX_LOGD("TEST", "Debug message");
    KNX_LOGV("TEST", "Verbose message");
    
    TEST_ASSERT_EQUAL(4, capturedLogs.size());
    TEST_ASSERT_EQUAL(log::Level::Debug, capturedLogs[3].level);
}

void test_log_level_verbose() {
    log::setLevel(log::Level::Verbose);
    
    KNX_LOGE("TEST", "Error message");
    KNX_LOGW("TEST", "Warn message");
    KNX_LOGI("TEST", "Info message");
    KNX_LOGD("TEST", "Debug message");
    KNX_LOGV("TEST", "Verbose message");
    
    TEST_ASSERT_EQUAL(5, capturedLogs.size());
    TEST_ASSERT_EQUAL(log::Level::Verbose, capturedLogs[4].level);
}

void test_log_get_level() {
    log::setLevel(log::Level::Debug);
    TEST_ASSERT_EQUAL(log::Level::Debug, log::getLevel());
    
    log::setLevel(log::Level::Error);
    TEST_ASSERT_EQUAL(log::Level::Error, log::getLevel());
}

// ============================================================================
// Per-Module Log Level Tests
// ============================================================================

void test_module_level_override_global() {
    log::setLevel(log::Level::Error);  // Global: only errors
    log::setModuleLevel("MODULE_A", log::Level::Debug);  // Module A: debug
    
    KNX_LOGI("MODULE_A", "Info from A");  // Should appear (module level)
    KNX_LOGI("MODULE_B", "Info from B");  // Should NOT appear (global level)
    
    TEST_ASSERT_EQUAL(1, capturedLogs.size());
    TEST_ASSERT_EQUAL_STRING("MODULE_A", capturedLogs[0].tag.c_str());
}

void test_module_level_multiple_modules() {
    log::setLevel(log::Level::Error);
    log::setModuleLevel("BAU", log::Level::Debug);
    log::setModuleLevel("DataLink", log::Level::Info);
    log::setModuleLevel("Physical", log::Level::Warn);
    
    KNX_LOGD("BAU", "Debug from BAU");          // ✓ (Debug allowed)
    KNX_LOGD("DataLink", "Debug from DataLink"); // ✗ (only Info+)
    KNX_LOGI("DataLink", "Info from DataLink");  // ✓ (Info allowed)
    KNX_LOGI("Physical", "Info from Physical");  // ✗ (only Warn+)
    KNX_LOGW("Physical", "Warn from Physical");  // ✓ (Warn allowed)
    KNX_LOGI("Network", "Info from Network");    // ✗ (global Error only)
    
    TEST_ASSERT_EQUAL(3, capturedLogs.size());
    TEST_ASSERT_EQUAL_STRING("BAU", capturedLogs[0].tag.c_str());
    TEST_ASSERT_EQUAL_STRING("DataLink", capturedLogs[1].tag.c_str());
    TEST_ASSERT_EQUAL_STRING("Physical", capturedLogs[2].tag.c_str());
}

void test_module_level_get() {
    log::setLevel(log::Level::Info);
    log::setModuleLevel("TEST_MODULE", log::Level::Verbose);
    
    TEST_ASSERT_EQUAL(log::Level::Verbose, log::getModuleLevel("TEST_MODULE"));
    TEST_ASSERT_EQUAL(log::Level::Info, log::getModuleLevel("OTHER_MODULE"));
}

void test_module_level_clear() {
    log::setLevel(log::Level::Error);
    log::setModuleLevel("TEST", log::Level::Debug);
    
    KNX_LOGD("TEST", "Before clear");
    TEST_ASSERT_EQUAL(1, capturedLogs.size());
    
    capturedLogs.clear();
    log::clearModuleLevel("TEST");
    
    KNX_LOGD("TEST", "After clear");  // Should use global level (Error)
    TEST_ASSERT_EQUAL(0, capturedLogs.size());
}

void test_module_level_clear_all() {
    log::setLevel(log::Level::Error);
    log::setModuleLevel("MODULE_A", log::Level::Debug);
    log::setModuleLevel("MODULE_B", log::Level::Info);
    log::setModuleLevel("MODULE_C", log::Level::Verbose);
    
    log::clearAllModuleLevels();
    
    // All should revert to global Error level
    KNX_LOGI("MODULE_A", "Info A");
    KNX_LOGI("MODULE_B", "Info B");
    KNX_LOGI("MODULE_C", "Info C");
    
    TEST_ASSERT_EQUAL(0, capturedLogs.size());
}

// ============================================================================
// Log Tag and Message Tests
// ============================================================================

void test_log_captures_tag() {
    log::setLevel(log::Level::Info);
    KNX_LOGI("MY_TAG", "Test message");
    
    TEST_ASSERT_EQUAL(1, capturedLogs.size());
    TEST_ASSERT_EQUAL_STRING("MY_TAG", capturedLogs[0].tag.c_str());
}

void test_log_captures_message() {
    log::setLevel(log::Level::Info);
    KNX_LOGI("TAG", "Test message with value: %d", 42);
    
    TEST_ASSERT_EQUAL(1, capturedLogs.size());
    TEST_ASSERT_EQUAL_STRING("Test message with value: 42", capturedLogs[0].message.c_str());
}

void test_log_multiple_messages() {
    log::setLevel(log::Level::Info);
    
    KNX_LOGE("TAG1", "Error 1");
    KNX_LOGW("TAG2", "Warning 2");
    KNX_LOGI("TAG3", "Info 3");
    
    TEST_ASSERT_EQUAL(3, capturedLogs.size());
    TEST_ASSERT_EQUAL_STRING("Error 1", capturedLogs[0].message.c_str());
    TEST_ASSERT_EQUAL_STRING("Warning 2", capturedLogs[1].message.c_str());
    TEST_ASSERT_EQUAL_STRING("Info 3", capturedLogs[2].message.c_str());
}

// ============================================================================
// Edge Cases
// ============================================================================

void test_module_level_null_tag() {
    log::setModuleLevel(nullptr, log::Level::Debug);  // Should not crash
    log::clearModuleLevel(nullptr);  // Should not crash
    
    // Use global level
    log::setLevel(log::Level::Error);
    KNX_LOGI("TAG", "Info");
    TEST_ASSERT_EQUAL(0, capturedLogs.size());
}

void test_module_level_empty_tag() {
    log::setLevel(log::Level::Info);
    log::setModuleLevel("", log::Level::Debug);  // Should be ignored
    
    // Should use global level since empty tag is ignored
    TEST_ASSERT_EQUAL(log::Level::Info, log::getModuleLevel(""));
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    // Global log level tests
    RUN_TEST(test_log_level_error_only);
    RUN_TEST(test_log_level_warn);
    RUN_TEST(test_log_level_info);
    RUN_TEST(test_log_level_debug);
    RUN_TEST(test_log_level_verbose);
    RUN_TEST(test_log_get_level);
    
    // Per-module log level tests
    RUN_TEST(test_module_level_override_global);
    RUN_TEST(test_module_level_multiple_modules);
    RUN_TEST(test_module_level_get);
    RUN_TEST(test_module_level_clear);
    RUN_TEST(test_module_level_clear_all);
    
    // Tag and message tests
    RUN_TEST(test_log_captures_tag);
    RUN_TEST(test_log_captures_message);
    RUN_TEST(test_log_multiple_messages);
    
    // Edge cases
    RUN_TEST(test_module_level_null_tag);
    RUN_TEST(test_module_level_empty_tag);
    
    return UNITY_END();
}
