// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_persistence_factory.cpp
 * @brief objects::setPersistenceFactory: a product supplies its own storage
 *
 * A bare-metal build has no default backend, so the factory is the only source
 * of storage there. Hosts keep their file backend unless a factory is set.
 */

#include "unity.h"

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/object_persistence.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace knx;
using namespace knx::objects;

namespace {

int g_factoryCalls = 0;
int g_initCalls = 0;

/// Smallest backend that satisfies the interface; counts init() so a test can
/// tell that the manager used it rather than the platform default.
class CountingPersistence final : public ObjectPersistence {
public:
    util::Result<void> init(const std::string&) override {
        ++g_initCalls;
        return util::Result<void>::ok();
    }
    void close() override {}
    util::Result<void> save(const std::string& key, std::span<const uint8_t> data) override {
        _named[key].assign(data.begin(), data.end());
        return util::Result<void>::ok();
    }
    util::Result<void> saveById(uint16_t id, std::span<const uint8_t> data) override {
        _byId[id].assign(data.begin(), data.end());
        return util::Result<void>::ok();
    }
    util::Result<void> load(const std::string& key, std::vector<uint8_t>& out) override {
        const auto it = _named.find(key);
        if (it == _named.end()) return util::ErrorCode::NotFound;
        out = it->second;
        return util::Result<void>::ok();
    }
    util::Result<void> loadById(uint16_t id, std::vector<uint8_t>& out) override {
        const auto it = _byId.find(id);
        if (it == _byId.end()) return util::ErrorCode::NotFound;
        out = it->second;
        return util::Result<void>::ok();
    }
    util::Result<bool> exists(const std::string& key) override { return _named.count(key) != 0; }
    util::Result<bool> existsById(uint16_t id) override { return _byId.count(id) != 0; }
    util::Result<void> erase(const std::string& key) override {
        _named.erase(key);
        return util::Result<void>::ok();
    }
    util::Result<void> eraseById(uint16_t id) override {
        _byId.erase(id);
        return util::Result<void>::ok();
    }
    util::Result<void> eraseAll() override {
        _named.clear();
        _byId.clear();
        return util::Result<void>::ok();
    }
    std::vector<std::string> listKeys() override { return {}; }

private:
    std::map<std::string, std::vector<uint8_t>> _named;
    std::map<uint16_t, std::vector<uint8_t>> _byId;
};

std::unique_ptr<ObjectPersistence> makeCounting() {
    ++g_factoryCalls;
    return std::make_unique<CountingPersistence>();
}

std::unique_ptr<ObjectPersistence> makeNothing() {
    ++g_factoryCalls;
    return nullptr;
}

}  // namespace

void setUp(void) {
    g_factoryCalls = 0;
    g_initCalls = 0;
    setPersistenceFactory(nullptr);
}

void tearDown(void) { setPersistenceFactory(nullptr); }

void test_registered_factory_supplies_the_backend(void) {
    setPersistenceFactory(&makeCounting);
    auto backend = createPersistence();
    TEST_ASSERT_NOT_NULL(backend.get());
    TEST_ASSERT_EQUAL_INT(1, g_factoryCalls);

    const std::vector<uint8_t> bytes{1, 2, 3};
    TEST_ASSERT_TRUE(backend->saveById(7, bytes).isOk());
    std::vector<uint8_t> read;
    TEST_ASSERT_TRUE(backend->loadById(7, read).isOk());
    TEST_ASSERT_EQUAL_MEMORY(bytes.data(), read.data(), 3);
}

void test_clearing_the_factory_restores_the_platform_default(void) {
    setPersistenceFactory(&makeCounting);
    setPersistenceFactory(nullptr);
    auto backend = createPersistence();
#if defined(KNX_PLATFORM_BAREMETAL)
    TEST_ASSERT_NULL(backend.get());  // bare metal has no default
#else
    TEST_ASSERT_NOT_NULL(backend.get());
#endif
    TEST_ASSERT_EQUAL_INT(0, g_factoryCalls);
}

void test_interface_object_manager_initialises_the_factory_backend(void) {
    setPersistenceFactory(&makeCounting);
    InterfaceObjectManager manager;
    TEST_ASSERT_TRUE(manager.init(true, "factory_test", 1).isOk());
    TEST_ASSERT_EQUAL_INT(1, g_factoryCalls);
    TEST_ASSERT_EQUAL_INT(1, g_initCalls);
}

void test_missing_backend_fails_init_instead_of_crashing(void) {
    setPersistenceFactory(&makeNothing);
    InterfaceObjectManager manager;
    const auto result = manager.init(true, "factory_test", 1);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::NotInitialized, result.error());
}

void test_persistence_disabled_never_asks_for_a_backend(void) {
    setPersistenceFactory(&makeNothing);
    InterfaceObjectManager manager;
    TEST_ASSERT_TRUE(manager.init(false, "factory_test", 1).isOk());
    TEST_ASSERT_EQUAL_INT(0, g_factoryCalls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_registered_factory_supplies_the_backend);
    RUN_TEST(test_clearing_the_factory_restores_the_platform_default);
    RUN_TEST(test_interface_object_manager_initialises_the_factory_backend);
    RUN_TEST(test_missing_backend_fails_init_instead_of_crashing);
    RUN_TEST(test_persistence_disabled_never_asks_for_a_backend);
    return UNITY_END();
}
