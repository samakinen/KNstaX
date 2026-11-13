// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_result.cpp
 * @brief Unit tests for Result<T> error handling wrapper
 */

#include "knx/util/result.hpp"
#include "unity.h"

#include <cstring>

using namespace knx::util;

void setUp(void) {}
void tearDown(void) {}

// Test basic Result<int> usage
void test_result_with_value(void) {
    Result<int> r(42);
    
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_FALSE(r.isError());
    TEST_ASSERT_EQUAL_INT(42, r.value());
    TEST_ASSERT_EQUAL_UINT8(ErrorCode::Success, r.error());
}

void test_result_with_error(void) {
    Result<int> r(ErrorCode::InvalidParameter);
    
    TEST_ASSERT_FALSE(r.isOk());
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(ErrorCode::InvalidParameter, r.error());
}

void test_result_value_or(void) {
    Result<int> success(42);
    Result<int> failure(ErrorCode::Timeout);
    
    TEST_ASSERT_EQUAL_INT(42, success.valueOr(0));
    TEST_ASSERT_EQUAL_INT(0, failure.valueOr(0));
}

void test_result_bool_conversion(void) {
    Result<int> success(100);
    Result<int> failure(ErrorCode::BufferTooSmall);
    
    TEST_ASSERT_TRUE(static_cast<bool>(success));
    TEST_ASSERT_FALSE(static_cast<bool>(failure));
}

void test_result_void_success(void) {
    Result<void> r = Result<void>::ok();
    
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_FALSE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(ErrorCode::Success, r.error());
}

void test_result_void_error(void) {
    Result<void> r = Result<void>::err(ErrorCode::NotInitialized);
    
    TEST_ASSERT_FALSE(r.isOk());
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(ErrorCode::NotInitialized, r.error());
}

void test_error_code_to_string(void) {
    TEST_ASSERT_EQUAL_STRING("Success", errorCodeToString(ErrorCode::Success));
    TEST_ASSERT_EQUAL_STRING("Invalid parameter", errorCodeToString(ErrorCode::InvalidParameter));
    TEST_ASSERT_EQUAL_STRING("Timeout", errorCodeToString(ErrorCode::Timeout));
    TEST_ASSERT_EQUAL_STRING("Queue full", errorCodeToString(ErrorCode::QueueFull));
    TEST_ASSERT_EQUAL_STRING("Not found", errorCodeToString(ErrorCode::NotFound));
    TEST_ASSERT_EQUAL_STRING("Storage full", errorCodeToString(ErrorCode::StorageFull));
    TEST_ASSERT_EQUAL_STRING("Storage error", errorCodeToString(ErrorCode::StorageError));
    TEST_ASSERT_EQUAL_STRING("Read only", errorCodeToString(ErrorCode::ReadOnly));
    TEST_ASSERT_EQUAL_STRING("Write only", errorCodeToString(ErrorCode::WriteOnly));
}

/// Every code below UnknownError has its own string, so a log line never has
/// to fall back to "Undefined error" for a code the stack actually returns.
void test_error_code_strings_are_distinct(void) {
    const auto last = static_cast<uint8_t>(ErrorCode::WriteOnly);
    for (uint8_t a = 0; a <= last; ++a) {
        const char* sa = errorCodeToString(static_cast<ErrorCode>(a));
        TEST_ASSERT_NOT_EQUAL(0, strcmp("Undefined error", sa));
        for (uint8_t b = static_cast<uint8_t>(a + 1u); b <= last; ++b) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(sa, errorCodeToString(static_cast<ErrorCode>(b))));
        }
    }
}

void test_result_copy_construction(void) {
    Result<int> original(99);
    Result<int> copy(original);
    
    TEST_ASSERT_TRUE(copy.isOk());
    TEST_ASSERT_EQUAL_INT(99, copy.value());
}

void test_result_move_construction(void) {
    Result<int> original(77);
    Result<int> moved(std::move(original));
    
    TEST_ASSERT_TRUE(moved.isOk());
    TEST_ASSERT_EQUAL_INT(77, moved.value());
}

void test_result_assignment(void) {
    Result<int> r1(50);
    Result<int> r2(ErrorCode::InvalidFrameSize);
    
    r2 = r1;
    TEST_ASSERT_TRUE(r2.isOk());
    TEST_ASSERT_EQUAL_INT(50, r2.value());
}

void test_result_expected_interop(void) {
    Result<int> success(std::expected<int, ErrorCode>(123));
    Result<int> failure(std::unexpected(ErrorCode::Timeout));

    TEST_ASSERT_TRUE(success.isOk());
    TEST_ASSERT_EQUAL_INT(123, success.expected().value());
    TEST_ASSERT_TRUE(failure.isError());
    TEST_ASSERT_EQUAL_UINT8(ErrorCode::Timeout, failure.expected().error());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_result_with_value);
    RUN_TEST(test_result_with_error);
    RUN_TEST(test_result_value_or);
    RUN_TEST(test_result_bool_conversion);
    RUN_TEST(test_result_void_success);
    RUN_TEST(test_result_void_error);
    RUN_TEST(test_error_code_to_string);
    RUN_TEST(test_error_code_strings_are_distinct);
    RUN_TEST(test_result_copy_construction);
    RUN_TEST(test_result_move_construction);
    RUN_TEST(test_result_assignment);
    RUN_TEST(test_result_expected_interop);
    
    return UNITY_END();
}
