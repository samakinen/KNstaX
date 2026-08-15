// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file unity.h
 * @brief Minimal Unity Test Framework Mock
 * 
 * This is a minimal implementation of the Unity framework for Linux builds.
 * For actual ESP-IDF builds, the real Unity framework from ESP-IDF is used.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <cmath>

// ============================================================================
// Test Framework State
// ============================================================================

static int _unity_tests_run = 0;
static int _unity_tests_failed = 0;
static int _unity_current_test_failed = false;
static const char* _unity_current_test_name = nullptr;

// Real Unity aborts the current test on the first failing assert, and this mock
// must match that. Merely recording the failure and continuing is not safe: a
// failed precondition (e.g. an "is not empty" check) would run straight into
// the out-of-bounds access it was guarding and segfault the whole binary.
//
// The abort is implemented by throwing rather than by longjmp, because these
// are C++ tests with RAII locals (RX-task-joining drivers, lock guards) and
// only exception unwinding runs their destructors. A longjmp would leak the RX
// task or leave a mutex locked, deadlocking the next test.
struct _UnityTestAbort {};
static bool _unity_abort_enabled = false;

static inline void _unity_abort() {
    if (_unity_abort_enabled) {
        throw _UnityTestAbort{};
    }
}

// ============================================================================
// Test Setup/Teardown
// ============================================================================

extern "C" {
    void setUp(void);
    void tearDown(void);
}

// ============================================================================
// Assertion Macros
// ============================================================================

#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            printf("[FAIL] %s:%d - Assertion failed: %s\n", __FILE__, __LINE__, #condition); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT((condition))

#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))

#define TEST_ASSERT_EQUAL(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("[FAIL] %s:%d - Expected %lld but got %lld\n", \
                   __FILE__, __LINE__, (long long)(expected), (long long)(actual)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_UINT(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_INT8(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_UINT8(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_INT16(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_UINT16(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_INT32(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_UINT32(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_UINT64(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_NOT_EQUAL(expected, actual) \
    do { \
        if ((expected) == (actual)) { \
            printf("[FAIL] %s:%d - Values should not be equal: %lld\n", \
                   __FILE__, __LINE__, (long long)(expected)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { \
        if (std::strcmp((expected), (actual)) != 0) { \
            printf("[FAIL] %s:%d - Expected \"%s\" but got \"%s\"\n", \
                   __FILE__, __LINE__, (expected), (actual)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_FLOAT(expected, actual) \
    do { \
        float exp = (expected); \
        float act = (actual); \
        float diff = (exp > act) ? (exp - act) : (act - exp); \
        if (diff > 0.0001f) { \
            printf("[FAIL] %s:%d - Expected %f but got %f\n", \
                   __FILE__, __LINE__, exp, act); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_FLOAT_WITHIN(threshold, expected, actual) \
    do { \
        float exp = (expected); \
        float act = (actual); \
        float diff = std::fabs(exp - act); \
        if (diff > (threshold)) { \
            printf("[FAIL] %s:%d - Expected %f within %f but got %f\n", \
                   __FILE__, __LINE__, exp, (threshold), act); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_DOUBLE(expected, actual) \
    do { \
        double exp = (expected); \
        double act = (actual); \
        double diff = (exp > act) ? (exp - act) : (act - exp); \
        if (diff > 0.000001) { \
            printf("[FAIL] %s:%d - Expected %f but got %f\n", \
                   __FILE__, __LINE__, exp, act); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_NULL(pointer) \
    do { \
        if ((pointer) != nullptr) { \
            printf("[FAIL] %s:%d - Expected NULL but pointer is not null\n", \
                   __FILE__, __LINE__); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_NOT_NULL(pointer) \
    do { \
        if ((pointer) == nullptr) { \
            printf("[FAIL] %s:%d - Expected non-NULL pointer\n", \
                   __FILE__, __LINE__); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_HEX(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_HEX8(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_HEX16(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_HEX32(expected, actual) TEST_ASSERT_EQUAL((expected), (actual))

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, length) \
    do { \
        if (std::memcmp((expected), (actual), (length)) != 0) { \
            printf("[FAIL] %s:%d - Memory blocks not equal\n", __FILE__, __LINE__); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) \
    do { \
        if (!((actual) > (threshold))) { \
            printf("[FAIL] %s:%d - Expected %lld to be greater than %lld\n", \
                   __FILE__, __LINE__, (long long)(actual), (long long)(threshold)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual) \
    do { \
        if (!((actual) >= (threshold))) { \
            printf("[FAIL] %s:%d - Expected %lld to be >= %lld\n", \
                   __FILE__, __LINE__, (long long)(actual), (long long)(threshold)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) \
    do { \
        if (!((actual) < (threshold))) { \
            printf("[FAIL] %s:%d - Expected %lld to be less than %lld\n", \
                   __FILE__, __LINE__, (long long)(actual), (long long)(threshold)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_ASSERT_LESS_OR_EQUAL(threshold, actual) \
    do { \
        if (!((actual) <= (threshold))) { \
            printf("[FAIL] %s:%d - Expected %lld to be <= %lld\n", \
                   __FILE__, __LINE__, (long long)(actual), (long long)(threshold)); \
            _unity_current_test_failed = true; _unity_abort(); \
        } \
    } while(0)

#define TEST_FAIL_MESSAGE(message) \
    do { \
        printf("[FAIL] %s:%d - %s\n", __FILE__, __LINE__, (message)); \
        _unity_current_test_failed = true; _unity_abort(); \
    } while(0)

#define TEST_FAIL() TEST_FAIL_MESSAGE("Test failed")

// ============================================================================
// Test Registration and Execution
// ============================================================================

// Macro to register and run a test
#define REGISTER_AND_RUN_TEST(test_func) \
    do { \
        _unity_current_test_name = #test_func; \
        _unity_current_test_failed = false; \
        printf("[RUN]  %s\n", _unity_current_test_name); \
        /* Run setUp + the test body in the abort region so the first failing */ \
        /* assert throws back to here (unwinding RAII locals) instead of */ \
        /* running on into the UB the assert was guarding. */ \
        _unity_abort_enabled = true; \
        try { \
            setUp(); \
            test_func(); \
        } catch (const _UnityTestAbort&) { \
            /* recorded via _unity_current_test_failed */ \
        } catch (...) { \
            printf("[FAIL] %s - unexpected exception\n", _unity_current_test_name); \
            _unity_current_test_failed = true; \
        } \
        _unity_abort_enabled = false; \
        tearDown(); \
        if (_unity_current_test_failed) { \
            printf("[FAIL] %s\n", _unity_current_test_name); \
            _unity_tests_failed++; \
        } else { \
            printf("[PASS] %s\n", _unity_current_test_name); \
        } \
        _unity_tests_run++; \
    } while(0)

// ============================================================================
// Test Summary
// ============================================================================

inline int print_unity_summary() {
    printf("\n");
    printf("=============================================\n");
    printf("Unity Test Results\n");
    printf("=============================================\n");
    printf("Tests Run:     %d\n", _unity_tests_run);
    printf("Tests Failed:  %d\n", _unity_tests_failed);
    printf("Tests Passed:  %d\n", _unity_tests_run - _unity_tests_failed);
    printf("=============================================\n");
    
    if (_unity_tests_failed == 0) {
        printf("✓ All tests passed!\n");
        return 0;
    } else {
        printf("✗ %d test(s) failed\n", _unity_tests_failed);
        return 1;
    }
}

// Unity-style convenience macros
#define UNITY_BEGIN() do { _unity_tests_run = 0; _unity_tests_failed = 0; } while(0)
#define RUN_TEST(test_func) REGISTER_AND_RUN_TEST(test_func)
#define UNITY_END() print_unity_summary()
