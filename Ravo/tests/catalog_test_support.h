#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <QByteArray>
#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/json.h"
#include "ravo/services/catalog_service.h"

#include "recovery_publication_internal.h"

namespace ravo
{

[[nodiscard]] std::filesystem::path make_catalog_test_temp_root();
[[nodiscard]] std::string repository_path(const std::filesystem::path &relative);
[[nodiscard]] std::string png_fixture_path();
[[nodiscard]] std::string raw_fixture_path();
[[nodiscard]] std::string xtrans_fixture_path();
[[nodiscard]] QByteArray file_sha256(const std::string &path);
[[nodiscard]] std::string sha256_text(std::string_view text);
[[nodiscard]] std::string recovery_document_with_mutated_payload(
    const std::filesystem::path &source,
    const std::function<void(JsonValue::Object &)> &mutate_payload);

struct RecoveryPublicationHookState
{
    recovery_publication_internal::Checkpoint target =
        recovery_publication_internal::Checkpoint::kBeforeTemporaryOpen;
    std::error_code injected_error;
    CancellationSource *cancellation = nullptr;
    std::string competitor_output;
    bool probe_temporary_rename = false;
    std::vector<std::string> observed_paths;
};

[[nodiscard]] std::error_code
recovery_publication_hook(void *context, recovery_publication_internal::Checkpoint checkpoint,
                          std::string_view path, std::uint64_t bytes_processed) noexcept;

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
