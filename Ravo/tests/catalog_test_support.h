#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/engine/engine.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

[[nodiscard]] std::filesystem::path make_catalog_test_temp_root();

class CatalogServiceTest : public ::testing::Test
{
protected:
    CatalogServiceTest();
    ~CatalogServiceTest() override;

    void SetUp() override;
    void TearDown() override;
    Result<void> open_service(bool create, bool resume_recovery = true);

    EngineFacade engine;
    std::filesystem::path root;
    std::string database_path;
    std::unique_ptr<CatalogService> service;
    SqliteCatalogRepository *sqlite_repository = nullptr;
};

} // namespace ravo
